#!/usr/bin/env python3
"""Run the Debian or RPM packaging lifecycle and emit honest evidence.

The driver renders the packaging metadata from the release context and, when the
environment can actually do it, walks the lifecycle: lint, build, inspect,
prefix isolation, install (or sealed N-1 install→seed→upgrade→verify when
GLIFISTORE_N1_PACKAGE_DIR supplies predecessor packages), external consumer,
systemd service, protocol exercise, durable restart, configuration preservation
and removal. Every check that did not run says so, with the reason.

Three execution modes, in decreasing order of preference:

* ``--inner``: this process *is* the disposable target. Used inside the pinned
  container and by a caller who accepts that packages will be installed.
* container dispatch: re-runs this driver inside the digest-pinned image the
  package matrix declares for the target, with systemd as PID 1 when the host
  can offer cgroup + privileged (so service-lifecycle can start the unit). Opt
  in with ``GLIFISTORE_PACKAGE_CI_CONTAINER=1``. Falls back to a one-shot entry
  (service-lifecycle BLOCKED) when systemd cannot become PID 1.
* native dispatch on the caller's host: installs and removes system packages, so
  it needs root and ``GLIFISTORE_PACKAGE_CI_NATIVE=1``.

Without any of those the backend reports the metadata rows it genuinely resolved
and BLOCKED or NOT_RUN for the rest. Nothing here fabricates a PASS.
"""

from __future__ import annotations

import argparse
import json
import lzma
import os
import platform
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Sequence

if __package__ in (None, ""):  # direct script execution
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from engineering.tools.generate_package_matrix import (
    PackageMatrixError,
    backend as matrix_backend,
    check_plan,
    load_matrix,
)
from engineering.tools.generate_release_context import ReleaseContextError, load_release_context
from engineering.tools.n1_package_artifacts import (
    CONTAINER_N1_MOUNT,
    N1_PACKAGE_DIR_ENVIRONMENT,
    N1PackageError,
    select_linux_n1_packages,
    supplied_n1_package_dir,
    upgrade_exercise_requested,
)
from engineering.tools.package_framework import (
    MATRIX_PATH,
    PROFILES,
    REPO_ROOT,
    SETTLED_RESULTS,
    STAGES,
    PackageFrameworkError,
    digest,
    write_json,
)
from engineering.tools.render_package_metadata import RenderError, render
from engineering.tools.validate_package_evidence import (
    PackageEvidenceError,
    emit_evidence,
    evidence_filename,
)

LINUX_BACKENDS = ("deb", "rpm")
NATIVE_ENVIRONMENT = "GLIFISTORE_PACKAGE_CI_NATIVE"
CONTAINER_ENVIRONMENT = "GLIFISTORE_PACKAGE_CI_CONTAINER"
CANDIDATE_ENVIRONMENT = "GLIFISTORE_CANDIDATE_DIR"
SEAL_ENVIRONMENT = "CANDIDATE_SEAL_SHA256"
# Forwarded into the container so the evidence it emits carries the producer identity
# of this run (engineering/tools/package_framework.py::producer_identity).
CI_IDENTITY_ENVIRONMENT = (
    "GITHUB_RUN_ATTEMPT",
    "GITHUB_RUN_ID",
    "GITHUB_WORKFLOW_REF",
    "RUNNER_ARCH",
    "RUNNER_OS",
)

SHORT_TIMEOUT = 15 * 60
BUILD_TIMEOUT = 120 * 60
CONTAINER_TIMEOUT = 180 * 60
DAEMON_READY_TIMEOUT = 90.0

# Each state is earned by the checks that prove it; the state stops at the first
# stage whose proof is incomplete. NOT_APPLICABLE_INITIAL_BASELINE never advances
# it, so a first release cannot claim UPGRADE_VERIFIED.
LIFECYCLE_CHAIN = (
    ("STRUCTURAL", ("structural-metadata", "package-metadata-render")),
    ("BUILT", ("package-lint", "package-build")),
    ("INSTALLED", ("package-inspect", "prefix-isolation", "package-install")),
    ("FUNCTIONALLY_VERIFIED", ("external-consumer", "put-get-erase", "restart-recovery")),
    ("LIFECYCLE_VERIFIED", ("service-lifecycle", "config-preservation", "package-remove")),
    ("UPGRADE_VERIFIED", ("package-upgrade",)),
)

# Components of packaging/common/file-lists/payload.yaml that a complete install
# owns, and the split between what survives a removal and what must disappear.
ALL_COMPONENTS = (
    "runtime",
    "library",
    "development",
    "documentation",
    "configuration",
    "service",
    "data",
)
REMOVED_COMPONENTS = ("runtime", "documentation", "service")
RETAINED_COMPONENTS = ("configuration", "data")

MACHINE_ARCHITECTURES = {
    "x86_64": "amd64",
    "amd64": "amd64",
    "aarch64": "arm64",
    "arm64": "arm64",
}


class LinuxBackendError(RuntimeError):
    pass


@dataclass(frozen=True)
class BackendSpec:
    """The native tooling each backend needs and the packages its target must have."""

    build_tool: str
    query_tool: str
    dependencies: tuple[str, ...]
    install_command: tuple[str, ...]
    refresh_command: tuple[str, ...]


SPECS: dict[str, BackendSpec] = {
    "deb": BackendSpec(
        build_tool="dpkg-buildpackage",
        query_tool="dpkg-deb",
        dependencies=(
            "binutils",
            "build-essential",
            "ca-certificates",
            "cmake",
            "cpio",
            "dbus",
            "debhelper",
            "dpkg-dev",
            "file",
            "git",
            "libssl-dev",
            "ninja-build",
            "pkgconf",
            "python3",
            "python3-jsonschema",
            "python3-yaml",
            "systemd",
            "systemd-sysv",
            "util-linux",
            "xz-utils",
        ),
        install_command=("apt-get", "install", "-y", "--no-install-recommends"),
        refresh_command=("apt-get", "update"),
    ),
    "rpm": BackendSpec(
        build_tool="rpmbuild",
        query_tool="rpm",
        dependencies=(
            "binutils",
            "cmake",
            "cpio",
            "dbus",
            "file",
            "gcc-c++",
            "git",
            "ninja-build",
            "openssl-devel",
            "pkgconf",
            "python3-jsonschema",
            "python3-pyyaml",
            "rpm-build",
            "shadow-utils",
            "systemd",
            "systemd-rpm-macros",
            "tar",
            "util-linux",
            "xz",
        ),
        install_command=(
            "dnf",
            "install",
            "-y",
            # Fedora container images often set tsflags=nodocs. The payload inventory
            # requires the manuals, so this install must not inherit that exclusion.
            "--setopt=tsflags=",
        ),
        refresh_command=("dnf", "makecache"),
    ),
}


@dataclass
class Recorder:
    """Per-check bookkeeping; a status is written exactly once."""

    directory: Path
    statuses: dict[str, str] = field(default_factory=dict)
    evidence_refs: dict[str, str] = field(default_factory=dict)
    details: dict[str, str] = field(default_factory=dict)
    limitations: list[str] = field(default_factory=list)
    residuals: list[str] = field(default_factory=list)

    def record(
        self, check: str, status: str, *, log: str | None = None, detail: str | None = None
    ) -> None:
        if check in self.statuses:
            raise LinuxBackendError(f"check {check} was already recorded")
        self.statuses[check] = status
        if log is not None:
            self.evidence_refs[check] = log
        if detail is not None:
            self.details[check] = detail

    def passed(self, check: str) -> bool:
        return self.statuses.get(check) == "PASS"

    def write_log(self, name: str, text: str) -> str:
        path = self.directory / name
        path.write_text(text if text.endswith("\n") else text + "\n", encoding="utf-8")
        return name

    def pending(self, checks: Sequence[str], status: str, detail: str) -> None:
        for check in checks:
            if check not in self.statuses:
                self.record(check, status, detail=detail)


def _note(log: Path, text: str) -> None:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("a", encoding="utf-8") as stream:
        stream.write(text if text.endswith("\n") else text + "\n")


def _run(
    command: Sequence[str],
    *,
    log: Path,
    cwd: Path | None = None,
    environment: dict[str, str] | None = None,
    timeout: int = SHORT_TIMEOUT,
    expect_failure: bool = False,
) -> bool:
    """Run one lifecycle command, appending its full transcript to `log`."""
    log.parent.mkdir(parents=True, exist_ok=True)
    header = f"$ {' '.join(command)}\n"
    try:
        completed = subprocess.run(
            list(command),
            check=False,
            cwd=None if cwd is None else str(cwd),
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        with log.open("a", encoding="utf-8") as stream:
            stream.write(f"{header}refused: {error}\n")
        return False
    with log.open("a", encoding="utf-8") as stream:
        stream.write(header)
        stream.write(completed.stdout)
        stream.write(f"exit status: {completed.returncode}\n")
    succeeded = completed.returncode == 0
    return not succeeded if expect_failure else succeeded


def _capture(command: Sequence[str], *, timeout: int = 60) -> str | None:
    try:
        completed = subprocess.run(
            list(command),
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return completed.stdout.strip() if completed.returncode == 0 else None


def host_architecture() -> str | None:
    return MACHINE_ARCHITECTURES.get(platform.machine().lower())


def systemd_is_pid_one() -> bool:
    """The canonical test: systemd only creates /run/systemd/system when it is PID 1."""
    return Path("/run/systemd/system").is_dir()


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def wait_for_port(port: int, deadline: float, *, gone: bool = False) -> bool:
    while time.monotonic() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.settimeout(1.0)
            reachable = probe.connect_ex(("127.0.0.1", port)) == 0
        if reachable is not gone:
            return True
        time.sleep(0.25)
    return False


def configured_port(configuration: Path, fallback: int = 7379) -> int:
    """Read the port out of the installed configuration rather than assuming it."""
    try:
        for line in configuration.read_text(encoding="utf-8").splitlines():
            name, separator, value = line.partition("=")
            if separator and name.strip() == "port" and value.strip().isdigit():
                return int(value.strip())
    except OSError:
        return fallback
    return fallback


# --------------------------------------------------------------------------- #
# Source admission
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class SourceArchive:
    path: Path
    sha256: str
    sealed: bool
    origin: str


def working_source_archive(root: Path, directory: Path, product_version: str) -> Path:
    """An archive of HEAD for the build-from-tree profiles; never a release source."""
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / f"GlifiStore-{product_version}.tar.xz"
    if archive.is_file():
        return archive
    tarball = archive.with_suffix("")
    try:
        with tarball.open("wb") as stream:
            completed = subprocess.run(
                [
                    "git",
                    "-c",
                    "safe.directory=*",
                    "-C",
                    str(root),
                    "archive",
                    "--format=tar",
                    f"--prefix=GlifiStore-{product_version}/",
                    "HEAD",
                ],
                check=False,
                stdout=stream,
                stderr=subprocess.PIPE,
                timeout=SHORT_TIMEOUT,
            )
        if completed.returncode != 0:
            raise LinuxBackendError(
                f"git archive failed: {completed.stderr.decode('utf-8', 'replace').strip()}"
            )
        with tarball.open("rb") as source, lzma.open(archive, "wb", preset=0) as target:
            shutil.copyfileobj(source, target)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise LinuxBackendError(f"cannot build the working source archive: {error}") from error
    finally:
        tarball.unlink(missing_ok=True)
    return archive


def admit_source(
    recorder: Recorder,
    *,
    root: Path,
    work: Path,
    profile: str,
    product_version: str,
) -> SourceArchive | None:
    """Admit the sealed candidate when there is one; refuse HEAD in the release profile."""
    log = recorder.directory / "sealed-source-admission.log"
    candidate = os.environ.get(CANDIDATE_ENVIRONMENT, "").strip()
    archive_name = f"GlifiStore-{product_version}.tar.xz"

    if candidate:
        directory = Path(candidate)
        seal = directory / "candidate-seal.json"
        archive = directory / archive_name
        _note(log, f"candidate directory: {directory}")
        expected = os.environ.get(SEAL_ENVIRONMENT, "").strip()
        if expected:
            observed = digest(seal) if seal.is_file() else "<missing>"
            _note(log, f"candidate seal sha256: {observed} (expected {expected})")
            if observed != expected:
                recorder.record(
                    "sealed-source-admission",
                    "FAIL",
                    log=log.name,
                    detail="the candidate seal digest does not match CANDIDATE_SEAL_SHA256",
                )
                return None
        verified = _run(
            [
                sys.executable,
                str(root / "engineering/tools/release_bundle.py"),
                "verify-seal",
                "--directory",
                str(directory),
                "--seal",
                "candidate-seal.json",
            ],
            log=log,
        )
        if not verified or not archive.is_file():
            recorder.record(
                "sealed-source-admission",
                "FAIL",
                log=log.name,
                detail=f"the candidate does not carry a verified {archive_name}",
            )
            return None
        recorder.record("sealed-source-admission", "PASS", log=log.name)
        return SourceArchive(archive, digest(archive), True, f"sealed candidate {directory}")

    if profile == "release":
        reason = (
            "the release profile builds only from the sealed candidate source archive; "
            "a checkout of HEAD is never a release source"
        )
        _note(log, f"refused: {reason}")
        recorder.record("sealed-source-admission", "BLOCKED", log=log.name, detail=reason)
        return None

    archive = working_source_archive(root, work / "source", product_version)
    _note(log, f"no candidate was given; built {archive.name} from HEAD for profile {profile}")
    recorder.record(
        "sealed-source-admission",
        "NOT_RUN",
        log=log.name,
        detail=f"no sealed candidate was given; the {profile} profile built from the working tree",
    )
    recorder.limitations.append(
        "The package was built from an archive of HEAD, not from a sealed release candidate, "
        "so no artifact from this run is admissible for publication."
    )
    return SourceArchive(archive, digest(archive), False, "archive of HEAD")


# --------------------------------------------------------------------------- #
# Execution mode
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class Target:
    identifier: str
    image: str
    image_digest: str | None


def matrix_target(matrix: dict[str, Any], backend: str, profile: str) -> Target | None:
    architecture = host_architecture()
    for target in matrix_backend(matrix, backend)["targets"]:
        if profile not in target["profiles"]:
            continue
        if architecture is not None and target["arch"] != architecture:
            continue
        return Target(target["id"], target["container"], target["container_digest"])
    return None


def native_availability(backend: str, *, stage: str, inner: bool) -> tuple[bool, str, str]:
    host = platform.system()
    if host != "Linux":
        return False, "BLOCKED", f"requires a Linux host; this runner is {host}"
    if stage != "full":
        return False, "NOT_RUN", f"the package lifecycle needs --stage full, not {stage}"
    if os.geteuid() != 0:
        return False, "BLOCKED", "installing and removing system packages requires root"
    if not inner and os.environ.get(NATIVE_ENVIRONMENT) != "1":
        return (
            False,
            "NOT_RUN",
            "the lifecycle installs and removes system packages on this host and is opt-in; "
            f"set {NATIVE_ENVIRONMENT}=1 on a disposable target",
        )
    if shutil.which(SPECS[backend].install_command[0]) is None:
        return (
            False,
            "BLOCKED",
            f"the {SPECS[backend].install_command[0]} package manager is not available",
        )
    return True, "NOT_RUN", ""


def container_runtime() -> str | None:
    for candidate in ("docker", "podman"):
        if shutil.which(candidate) is not None:
            return candidate
    return None


def bootstrap_dependencies(backend: str, *, log: Path) -> bool:
    """Install the toolchain the lifecycle needs; a missing network is not a defect."""
    spec = SPECS[backend]
    environment = dict(os.environ)
    environment["DEBIAN_FRONTEND"] = "noninteractive"
    missing = [
        tool
        for tool in (spec.build_tool, spec.query_tool, "cmake", "readelf")
        if shutil.which(tool) is None
    ]
    if not missing:
        _note(log, f"the {backend} toolchain is already present; nothing to bootstrap")
        return True
    _note(log, f"missing before bootstrap: {', '.join(missing)}")
    if not _run(list(spec.refresh_command), log=log, environment=environment):
        return False
    return _run(
        [*spec.install_command, *spec.dependencies],
        log=log,
        environment=environment,
        timeout=BUILD_TIMEOUT,
    )


def container_mount_and_environment(
    *,
    root: Path,
    output: Path,
    release_context: Path,
    candidate: str = "",
    n1_packages: str = "",
    host_uid: int | None = None,
    host_gid: int | None = None,
    seal: str = "",
    ci_identity: dict[str, str] | None = None,
) -> tuple[list[str], list[str]]:
    """Shared bind mounts and environment for packaging containers."""
    mounts = [
        "-v",
        f"{root}:/src:ro",
        "-v",
        f"{output}:/out",
        "-v",
        f"{release_context}:/release-context.json:ro",
    ]
    environment = ["-e", f"{SEAL_ENVIRONMENT}={seal}", "-e", "container=docker"]
    for name, value in (ci_identity or {}).items():
        if value:
            environment += ["-e", f"{name}={value}"]
    uid = os.getuid() if host_uid is None else host_uid
    gid = os.getgid() if host_gid is None else host_gid
    environment += ["-e", f"HOST_UID={uid}", "-e", f"HOST_GID={gid}"]
    if candidate:
        mounts += ["-v", f"{candidate}:/candidate:ro"]
        environment += ["-e", f"{CANDIDATE_ENVIRONMENT}=/candidate"]
    if n1_packages:
        mounts += ["-v", f"{n1_packages}:{CONTAINER_N1_MOUNT}:ro"]
        environment += ["-e", f"{N1_PACKAGE_DIR_ENVIRONMENT}={CONTAINER_N1_MOUNT}"]
    return mounts, environment


def container_run_arguments(
    *,
    runtime: str,
    image: str,
    root: Path,
    output: Path,
    release_context: Path,
    backend: str,
    profile: str,
    stage: str,
    candidate: str = "",
    n1_packages: str = "",
    host_uid: int | None = None,
    host_gid: int | None = None,
    seal: str = "",
    ci_identity: dict[str, str] | None = None,
) -> list[str]:
    """Fallback one-shot `docker`/`podman run` without systemd as PID 1.

    Used only when the systemd-as-PID-1 path cannot start. service-lifecycle
    stays BLOCKED in that mode; nothing claims a systemd service run.
    """
    mounts, environment = container_mount_and_environment(
        root=root,
        output=output,
        release_context=release_context,
        candidate=candidate,
        n1_packages=n1_packages,
        host_uid=host_uid,
        host_gid=host_gid,
        seal=seal,
        ci_identity=ci_identity,
    )
    return [
        runtime,
        "run",
        "--rm",
        "--workdir",
        "/out",
        *mounts,
        *environment,
        image,
        "/bin/sh",
        "/src/scripts/packaging/linux-container-entry.sh",
        "--backend",
        backend,
        "--profile",
        profile,
        "--stage",
        stage,
    ]


def container_create_arguments(
    *,
    runtime: str,
    image: str,
    name: str,
    root: Path,
    output: Path,
    release_context: Path,
    candidate: str = "",
    n1_packages: str = "",
    host_uid: int | None = None,
    host_gid: int | None = None,
    seal: str = "",
    ci_identity: dict[str, str] | None = None,
    variant: str = "cgroupns-host",
) -> list[str]:
    """Detached container whose PID 1 is systemd (required for service-lifecycle)."""
    mounts, environment = container_mount_and_environment(
        root=root,
        output=output,
        release_context=release_context,
        candidate=candidate,
        n1_packages=n1_packages,
        host_uid=host_uid,
        host_gid=host_gid,
        seal=seal,
        ci_identity=ci_identity,
    )
    # cgroup + privileged: stock Debian/Fedora images need a real systemd PID 1
    # for systemctl enable/start of glifistored.service. Without them the
    # dispatcher falls back to the one-shot entry and service-lifecycle stays BLOCKED.
    # Two host layouts are tried: cgroupns=host (common on GHA) and private +
    # docker.slice parent (cgroup v2 runners that refuse host namespace).
    # Docker --tmpfs defaults to noexec; the installed-SDK matrix builds C++ peers
    # under TMPDIR and needs an executable scratch mount.
    variants = {
        "cgroupns-host": [
            "--privileged",
            "--cgroupns=host",
            "-v",
            "/sys/fs/cgroup:/sys/fs/cgroup:rw",
            "--tmpfs",
            "/tmp:rw,exec,nosuid,nodev",
            "--tmpfs",
            "/run",
            "--tmpfs",
            "/run/lock",
        ],
        "cgroupns-private-parent": [
            "--privileged",
            "--cgroupns=private",
            "--cgroup-parent=docker.slice",
            "-v",
            "/sys/fs/cgroup:/sys/fs/cgroup:rw",
            "--tmpfs",
            "/tmp:rw,exec,nosuid,nodev",
            "--tmpfs",
            "/run",
            "--tmpfs",
            "/run/lock",
        ],
    }
    if variant not in variants:
        raise LinuxBackendError(f"unknown systemd container create variant: {variant}")
    return [
        runtime,
        "run",
        "-d",
        "--name",
        name,
        *variants[variant],
        "--workdir",
        "/out",
        *mounts,
        *environment,
        image,
        "/bin/sh",
        "/src/scripts/packaging/linux-systemd-pid1.sh",
    ]


SYSTEMD_CREATE_VARIANTS = ("cgroupns-host", "cgroupns-private-parent")


def container_exec_arguments(
    *, runtime: str, name: str, backend: str, profile: str, stage: str
) -> list[str]:
    return [
        runtime,
        "exec",
        "--workdir",
        "/out",
        name,
        "/bin/sh",
        "/src/scripts/packaging/linux-container-entry.sh",
        "--backend",
        backend,
        "--profile",
        profile,
        "--stage",
        stage,
    ]


def wait_for_systemd_pid1(
    runtime: str, name: str, *, log: Path, timeout: float = 180.0
) -> bool:
    """Poll until systemd has created /run/systemd/system inside the container."""
    deadline = time.monotonic() + timeout
    probe = [
        runtime,
        "exec",
        name,
        "/bin/sh",
        "-c",
        "test -d /run/systemd/system",
    ]
    while time.monotonic() < deadline:
        if _run(probe, log=log, timeout=30):
            _note(log, f"systemd is PID 1 in container {name}")
            return True
        running = _capture([runtime, "inspect", "-f", "{{.State.Running}}", name], timeout=30)
        if running != "true":
            _note(log, f"container {name} is not running while waiting for systemd ({running!r})")
            return False
        time.sleep(2)
    _note(log, f"timed out waiting for systemd PID 1 in container {name}")
    return False


def prefer_inner_container_evidence(
    directory: Path, backend: str, profile: str, stage: str
) -> tuple[str, Path] | None:
    """Return the inner container's evidence when it wrote a report.

    The inner driver exits non-zero on FAIL *after* emitting evidence. The outer
    process must prefer that report over inventing BLOCKED, or a real packaging
    failure is laundered into a dishonest non-failure.
    """
    path = directory / evidence_filename(backend, profile, stage)
    if not path.is_file():
        return None
    payload = json.loads(path.read_text(encoding="utf-8"))
    return str(payload["result"]), path


def dispatch_container(
    recorder: Recorder,
    *,
    backend: str,
    profile: str,
    stage: str,
    root: Path,
    release_context: Path,
    target: Target,
    runtime: str,
) -> bool:
    """Re-run this driver inside the digest-pinned image with systemd as PID 1."""
    log = recorder.directory / "container-dispatch.log"
    image = f"{target.image}@{target.image_digest}"
    candidate = os.environ.get(CANDIDATE_ENVIRONMENT, "").strip()
    n1_packages = os.environ.get(N1_PACKAGE_DIR_ENVIRONMENT, "").strip()
    ci_identity = {
        name: os.environ.get(name, "").strip() for name in CI_IDENTITY_ENVIRONMENT
    }
    seal = os.environ.get(SEAL_ENVIRONMENT, "")
    name = f"glifistore-{backend}-{os.getpid()}-{int(time.time())}"
    _note(log, f"target={target.identifier} runtime={runtime} image={image}")
    _note(log, f"host_uid={os.getuid()} host_gid={os.getgid()}")
    _note(log, f"systemd container name={name}")
    if n1_packages:
        _note(log, f"{N1_PACKAGE_DIR_ENVIRONMENT}={n1_packages} → {CONTAINER_N1_MOUNT}")

    def one_shot() -> bool:
        return _run(
            container_run_arguments(
                runtime=runtime,
                image=image,
                root=root,
                output=recorder.directory,
                release_context=release_context,
                backend=backend,
                profile=profile,
                stage=stage,
                candidate=candidate,
                n1_packages=n1_packages,
                seal=seal,
                ci_identity=ci_identity,
            ),
            log=log,
            timeout=CONTAINER_TIMEOUT,
        )

    for variant in SYSTEMD_CREATE_VARIANTS:
        _run([runtime, "rm", "-f", name], log=log, timeout=60)
        _note(log, f"trying systemd create variant={variant}")
        created = _run(
            container_create_arguments(
                runtime=runtime,
                image=image,
                name=name,
                root=root,
                output=recorder.directory,
                release_context=release_context,
                candidate=candidate,
                n1_packages=n1_packages,
                seal=seal,
                ci_identity=ci_identity,
                variant=variant,
            ),
            log=log,
            timeout=CONTAINER_TIMEOUT,
        )
        if not created:
            _note(log, f"systemd container create failed for variant={variant}")
            continue
        try:
            if not wait_for_systemd_pid1(runtime, name, log=log):
                _note(log, f"systemd never became PID 1 for variant={variant}")
                continue
            return _run(
                container_exec_arguments(
                    runtime=runtime, name=name, backend=backend, profile=profile, stage=stage
                ),
                log=log,
                timeout=CONTAINER_TIMEOUT,
            )
        finally:
            _run([runtime, "rm", "-f", name], log=log, timeout=60)

    _note(log, "all systemd create variants failed; falling back to one-shot entry")
    return one_shot()


# --------------------------------------------------------------------------- #
# Lifecycle phases
# --------------------------------------------------------------------------- #


@dataclass
class Lifecycle:
    """State shared by the phases that run inside the disposable target."""

    recorder: Recorder
    backend: str
    root: Path
    work: Path
    metadata: Path
    layout: dict[str, str]
    tokens: dict[str, str]
    artifacts: Path
    installed: list[Path] = field(default_factory=list)
    inspect_root: Path | None = None
    consumer_build: Path | None = None
    marker: str = ""

    @property
    def configuration(self) -> Path:
        return Path(self.tokens["CONFIG_FILE"])

    @property
    def state_directory(self) -> Path:
        return Path(self.layout["statedir"])

    def log(self, name: str) -> Path:
        return self.recorder.directory / f"{name}.log"

    def tool(self, name: str) -> str:
        return f"{self.layout['bindir']}/{name}"

    def forbidden_roots(self) -> list[str]:
        """Everywhere an installed artifact or a consumer must never point back to.

        Both build trees are listed regardless of backend: a rendered path is
        cheaper to forbid than to explain after it leaks.
        """
        return [
            argument
            for path in (self.root, self.work / "build", self.work / "rpmbuild")
            for argument in ("--forbidden-root", str(path))
        ]


def run_lint(lifecycle: Lifecycle) -> None:
    """Validate the rendered metadata with the native tooling before any build."""
    recorder, log = lifecycle.recorder, lifecycle.log("package-lint")
    expected = lifecycle.tokens["PACKAGE_VERSION"]
    if lifecycle.backend == "deb":
        version = _capture(
            [
                "dpkg-parsechangelog",
                "--file",
                str(lifecycle.metadata / "debian/changelog"),
                "--show-field",
                "Version",
            ]
        )
        _note(log, f"dpkg-parsechangelog Version: {version} (expected {expected})")
        scripts = sorted(lifecycle.metadata.glob("debian/*.post*")) + sorted(
            lifecycle.metadata.glob("debian/*.pre*")
        )
        syntax = all(_run(["sh", "-n", str(script)], log=log) for script in scripts)
        _note(log, f"maintainer scripts checked: {[script.name for script in scripts]}")
        passed = version == expected and syntax and bool(scripts)
    else:
        spec = next(lifecycle.metadata.glob("*.spec"), None)
        if spec is None:
            _note(log, "no rendered spec to lint")
            recorder.record("package-lint", "FAIL", log=log.name, detail="no rendered spec")
            return
        version = _capture(
            ["rpmspec", "--query", "--srpm", "--qf", "%{version}-%{release}\n", str(spec)]
        )
        _note(log, f"rpmspec version-release: {version} (expected {expected} plus %dist)")
        listed = _run(["rpmspec", "--query", "--rpms", str(spec)], log=log)
        passed = listed and version is not None and version.startswith(expected)
    recorder.record("package-lint", "PASS" if passed else "FAIL", log=log.name)
    recorder.residuals.append(
        "linux-package-lint-depth=lintian and rpmlint findings are recorded with the inspect "
        "logs but are not yet a gate|a triaged tag baseline per distribution"
    )


def run_build(lifecycle: Lifecycle, source: SourceArchive) -> None:
    recorder, log = lifecycle.recorder, lifecycle.log("package-build")
    tokens = lifecycle.tokens
    build = lifecycle.work / "build"
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir(parents=True)
    _note(log, f"source: {source.origin} sha256={source.sha256}")

    if not _run(["tar", "-C", str(build), "-xJf", str(source.path)], log=log):
        recorder.record("package-build", "FAIL", log=log.name, detail="the source archive refused")
        return
    tree = build / tokens["SOURCE_DIRECTORY"]
    if not tree.is_dir():
        recorder.record(
            "package-build",
            "FAIL",
            log=log.name,
            detail=f"the archive has no {tokens['SOURCE_DIRECTORY']} directory",
        )
        return

    if lifecycle.backend == "deb":
        shutil.copytree(lifecycle.metadata / "debian", tree / "debian")
        built = _run(
            ["dpkg-buildpackage", "-b", "-us", "-uc", f"-j{os.cpu_count() or 1}"],
            log=log,
            cwd=tree,
            timeout=BUILD_TIMEOUT,
        )
        produced = sorted(build.glob("*.deb"))
        expected_packages = 3
    else:
        top = lifecycle.work / "rpmbuild"
        shutil.rmtree(top, ignore_errors=True)
        for name in ("SOURCES", "SPECS", "BUILD", "BUILDROOT", "RPMS", "SRPMS"):
            (top / name).mkdir(parents=True)
        shutil.copy2(source.path, top / "SOURCES" / tokens["SOURCE_ARCHIVE"])
        shutil.copy2(lifecycle.metadata / "glifistored.service", top / "SOURCES")
        spec = next(lifecycle.metadata.glob("*.spec"))
        shutil.copy2(spec, top / "SPECS" / spec.name)
        built = _run(
            ["rpmbuild", "-bb", "--define", f"_topdir {top}", str(top / "SPECS" / spec.name)],
            log=log,
            timeout=BUILD_TIMEOUT,
        )
        produced = sorted(top.glob("RPMS/*/*.rpm"))
        expected_packages = 3

    lifecycle.artifacts.mkdir(parents=True, exist_ok=True)
    for package in produced:
        shutil.copy2(package, lifecycle.artifacts / package.name)
    lifecycle.installed = sorted(lifecycle.artifacts.glob(f"*.{lifecycle.backend}"))
    _note(log, f"produced: {[package.name for package in lifecycle.installed]}")
    # Counted over the primary packages only: rpmbuild also emits debuginfo and
    # debugsource, which would otherwise mask a missing real package.
    primary = _primary_packages(lifecycle)
    if not built or len(primary) < expected_packages:
        recorder.record(
            "package-build",
            "FAIL",
            log=log.name,
            detail=f"expected {expected_packages} packages, got {len(primary)}",
        )
        return
    recorder.record("package-build", "PASS", log=log.name)


def _primary_packages(lifecycle: Lifecycle) -> list[Path]:
    """The three GlifiStore packages, without RPM's debuginfo and debugsource."""
    excluded = ("debuginfo", "debugsource", "-dbgsym")
    return [
        package
        for package in lifecycle.installed
        if not any(token in package.name for token in excluded)
    ]


def run_inspect(lifecycle: Lifecycle) -> None:
    recorder, log = lifecycle.recorder, lifecycle.log("package-inspect")
    expected = lifecycle.tokens["PACKAGE_VERSION"]
    root = lifecycle.work / "inspect-root"
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir(parents=True)
    ok = True
    for package in _primary_packages(lifecycle):
        if lifecycle.backend == "deb":
            ok = _run(["dpkg-deb", "--info", str(package)], log=log) and ok
            ok = _run(["dpkg-deb", "--contents", str(package)], log=log) and ok
            version = _capture(["dpkg-deb", "--field", str(package), "Version"])
            ok = _run(["dpkg-deb", "--extract", str(package), str(root)], log=log) and ok
        else:
            ok = _run(["rpm", "--query", "--info", "--package", str(package)], log=log) and ok
            ok = _run(["rpm", "--query", "--list", "--package", str(package)], log=log) and ok
            ok = _run(["rpm", "--query", "--configfiles", "--package", str(package)], log=log) and ok
            version = _capture(
                ["rpm", "--query", "--queryformat", "%{version}-%{release}", "--package", str(package)]
            )
            ok = (
                _run(
                    ["/bin/sh", "-c", f"rpm2cpio {shlex.quote(str(package))} | cpio -idmu --quiet"],
                    log=log,
                    cwd=root,
                )
                and ok
            )
        _note(log, f"{package.name}: version {version} (expected {expected} plus distribution tag)")
        if version is None or not version.startswith(expected):
            ok = False
    lifecycle.inspect_root = root

    linter = "lintian" if lifecycle.backend == "deb" else "rpmlint"
    if shutil.which(linter) is not None:
        _note(log, f"{linter} findings below are recorded, not yet gated")
        for package in _primary_packages(lifecycle):
            _run([linter, str(package)], log=log)
    else:
        _note(log, f"{linter} is not installed; no distribution lint findings were collected")

    payload = _run(
        [
            sys.executable,
            str(lifecycle.root / "engineering/tools/verify_package_payload.py"),
            "--root",
            str(root),
            "--layout",
            str(lifecycle.metadata / "rendered-metadata.json"),
            *[argument for component in ALL_COMPONENTS for argument in ("--component", component)],
        ],
        log=log,
    )
    recorder.record("package-inspect", "PASS" if ok and payload else "FAIL", log=log.name)


def run_prefix_isolation(lifecycle: Lifecycle) -> None:
    """The staged binaries must be hardened and must not point back at the build tree."""
    recorder, log = lifecycle.recorder, lifecycle.log("prefix-isolation")
    root = lifecycle.inspect_root
    if root is None:
        recorder.record(
            "prefix-isolation", "NOT_RUN", detail="no extracted package payload to inspect"
        )
        return
    libdir = lifecycle.layout["libdir"].lstrip("/")
    binaries = [
        root / lifecycle.layout["bindir"].lstrip("/") / "glifistored",
        root / libdir / f"libglifistore.so.{lifecycle.tokens['ABI_VERSION']}",
    ]
    ok = True
    for binary in binaries:
        if not binary.is_file():
            _note(log, f"missing staged binary: {binary}")
            ok = False
            continue
        dynamic = _capture(["readelf", "--dynamic", "--wide", str(binary)]) or ""
        headers = _capture(["readelf", "--program-headers", "--wide", str(binary)]) or ""
        kinds = _capture(["readelf", "--file-header", str(binary)]) or ""
        _note(log, f"--- {binary} ---")
        _note(log, dynamic)
        _note(log, headers)
        runpath = [line for line in dynamic.splitlines() if "RUNPATH" in line or "RPATH" in line]
        relro = "GNU_RELRO" in headers
        stack = "".join(line for line in headers.splitlines() if "GNU_STACK" in line)
        executable_stack = "RWE" in stack
        position_independent = "DYN (" in kinds
        bind_now = "NOW" in dynamic
        _note(
            log,
            f"runpath={runpath} relro={relro} executable_stack={executable_stack} "
            f"pie={position_independent} bind_now={bind_now}",
        )
        if runpath:
            _note(log, "a distributed binary must resolve its libraries from the system prefix")
            ok = False
        if not relro or executable_stack or not position_independent:
            ok = False
        if not bind_now:
            _note(log, "note: BIND_NOW is absent; recorded, not gated by this check")

    metadata_files = [
        root / libdir / "pkgconfig/glifistore-abi.pc",
        *sorted((root / libdir / "cmake/GlifiStore").glob("*.cmake")),
    ]
    present = [str(path) for path in metadata_files if path.is_file()]
    isolated = _run(
        [
            sys.executable,
            str(lifecycle.root / "engineering/tools/assert_consumer_isolation.py"),
            *lifecycle.forbidden_roots(),
            *present,
        ],
        log=log,
    )
    recorder.record("prefix-isolation", "PASS" if ok and isolated else "FAIL", log=log.name)


def _install_package_files(
    lifecycle: Lifecycle,
    packages: Sequence[Path],
    log: Path,
    *,
    verify_rendered_payload: bool = True,
) -> bool:
    """Install the given .deb/.rpm files through the target's package manager.

    ``verify_rendered_payload`` must stay True for the built N packages (layout
    vs this tip's rendered metadata). For sealed N-1 packages it must be False:
    N's rendered metadata must not be used as a substitute for N-1-era layout.
    """
    paths = [str(package) for package in packages]
    spec = SPECS[lifecycle.backend]
    environment = dict(os.environ)
    environment["DEBIAN_FRONTEND"] = "noninteractive"
    _note(log, f"installing: {[Path(path).name for path in paths]}")
    installed = _run(
        [*spec.install_command, *paths], log=log, environment=environment, timeout=BUILD_TIMEOUT
    )
    runtime_package = lifecycle.tokens["RUNTIME_PACKAGE"]
    if lifecycle.backend == "deb":
        installed = _run(["dpkg-query", "--show", runtime_package], log=log) and installed
    else:
        installed = _run(["rpm", "--query", runtime_package], log=log) and installed
    account = _run(["id", lifecycle.tokens["SERVICE_USER"]], log=log)
    if not verify_rendered_payload:
        _note(log, "skipping rendered-payload verify for sealed N-1 packages")
        return bool(installed and account)
    payload = _run(
        [
            sys.executable,
            str(lifecycle.root / "engineering/tools/verify_package_payload.py"),
            "--root",
            "/",
            "--layout",
            str(lifecycle.metadata / "rendered-metadata.json"),
            *[argument for component in ALL_COMPONENTS for argument in ("--component", component)],
        ],
        log=log,
    )
    return bool(installed and account and payload)


def run_install(lifecycle: Lifecycle) -> None:
    recorder, log = lifecycle.recorder, lifecycle.log("package-install")
    packages = _primary_packages(lifecycle)
    ok = _install_package_files(lifecycle, packages, log)
    recorder.record("package-install", "PASS" if ok else "FAIL", log=log.name)


def _build_external_consumer(lifecycle: Lifecycle, log: Path) -> bool:
    """Build the packaged smoke consumer against the currently installed prefix."""
    source = lifecycle.work / "external-consumer/src"
    build = lifecycle.work / "external-consumer/build"
    shutil.rmtree(lifecycle.work / "external-consumer", ignore_errors=True)
    shutil.copytree(lifecycle.root / "packaging/common/consumer", source)
    environment = {
        key: value
        for key, value in os.environ.items()
        if key not in ("GITHUB_WORKSPACE", "CMAKE_PREFIX_PATH", "CPATH", "LIBRARY_PATH")
    }
    configured = _run(
        [
            "cmake",
            "-S",
            str(source),
            "-B",
            str(build),
            "-GNinja",
            f"-DCMAKE_PREFIX_PATH={lifecycle.layout['prefix']}",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ],
        log=log,
        environment=environment,
    )
    compiled = configured and _run(
        ["cmake", "--build", str(build)], log=log, environment=environment, timeout=BUILD_TIMEOUT
    )
    if not compiled:
        return False
    lifecycle.consumer_build = build
    return True


def write_package_file_list(lifecycle: Lifecycle) -> Path | None:
    """Retain the package manager inventory used to prove daemon ownership."""
    log = lifecycle.log("package-file-list")
    destination = lifecycle.recorder.directory / "package-file-list.txt"
    runtime_package = lifecycle.tokens["RUNTIME_PACKAGE"]
    if lifecycle.backend == "deb":
        command = ["dpkg", "-L", runtime_package]
    else:
        command = ["rpm", "-ql", runtime_package]
    completed = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=SHORT_TIMEOUT,
    )
    destination.write_text(completed.stdout or "", encoding="utf-8")
    _note(log, f"$ {' '.join(command)}")
    _note(log, completed.stdout or "")
    if completed.returncode != 0 or not destination.read_text(encoding="utf-8").strip():
        _note(log, f"refused: inventory exit {completed.returncode}")
        return None
    return destination


def run_installed_sdk_matrix(lifecycle: Lifecycle) -> None:
    """Emit Wave F admission evidence; does not gate package-CI check rows."""
    log = lifecycle.log("installed-sdk-matrix")
    inventory = write_package_file_list(lifecycle)
    if inventory is None:
        _note(log, "no package file inventory; skipping installed SDK matrix harness")
        return
    bindir = Path(lifecycle.layout["bindir"])
    if not bindir.is_absolute():
        bindir = Path("/") / bindir
    daemon = bindir / "glifistored"
    prefix = Path(lifecycle.layout["prefix"])
    if not prefix.is_absolute():
        prefix = Path("/") / prefix
    tools = lifecycle.root / "scripts/packaging/ensure-installed-sdk-matrix-tools.sh"
    if not _run(["bash", str(tools)], log=log, timeout=BUILD_TIMEOUT):
        _note(log, "matrix toolchains were not installed; harness will record the gap")
    report = lifecycle.recorder.directory / "installed-sdk-matrix.json"
    environment = dict(os.environ)
    libdir = Path(lifecycle.layout["libdir"])
    if not libdir.is_absolute():
        libdir = Path("/") / libdir
    library_path = os.pathsep.join(
        part
        for part in (
            str(libdir),
            str(prefix / "lib"),
            str(prefix / "lib64"),
            environment.get("LD_LIBRARY_PATH", ""),
        )
        if part
    )
    environment.update(
        {
            "GLIFISTORE_PACKAGE_DAEMON": str(daemon),
            "GLIFISTORE_PACKAGE_FILE_LIST": str(inventory),
            "GLIFISTORE_PACKAGE_PREFIX": str(prefix),
            "INSTALLED_INTEROP_PROFILE": "plain",
            # Docker --tmpfs /tmp defaults to noexec; keep scratch under /out.
            "TMPDIR": str(lifecycle.recorder.directory / "tmp"),
            "PATH": f"/usr/local/go/bin:/usr/local/bin:{environment.get('PATH', '')}",
            "LD_LIBRARY_PATH": library_path,
        }
    )
    Path(environment["TMPDIR"]).mkdir(parents=True, exist_ok=True)
    _run(
        [
            "bash",
            str(lifecycle.root / "scripts/test-package-installed-sdk-matrix.sh"),
            "--report",
            str(report),
            "--replace",
        ],
        log=log,
        environment=environment,
        timeout=BUILD_TIMEOUT,
    )


def run_external_consumer(lifecycle: Lifecycle) -> None:
    """Build the packaged consumer from outside the checkout, against the install prefix."""
    recorder, log = lifecycle.recorder, lifecycle.log("external-consumer")
    if not _build_external_consumer(lifecycle, log):
        recorder.record(
            "external-consumer",
            "FAIL",
            log=log.name,
            detail="the external consumer failed to configure or compile",
        )
        return
    build = lifecycle.consumer_build
    assert build is not None
    environment = {
        key: value
        for key, value in os.environ.items()
        if key not in ("GITHUB_WORKSPACE", "CMAKE_PREFIX_PATH", "CPATH", "LIBRARY_PATH")
    }
    exercised = _run(
        ["ctest", "--output-on-failure"], log=log, cwd=build, environment=environment
    )
    isolated = _run(
        [
            sys.executable,
            str(lifecycle.root / "engineering/tools/assert_consumer_isolation.py"),
            "--compile-commands",
            str(build / "compile_commands.json"),
            *lifecycle.forbidden_roots(),
            str(lifecycle.recorder.directory / "external-consumer.log"),
        ],
        log=lifecycle.log("consumer-isolation"),
    )
    if not isolated:
        lifecycle.consumer_build = None
    recorder.record(
        "external-consumer",
        "PASS" if exercised and isolated else "FAIL",
        log=log.name,
        detail=None if isolated else "the consumer build referenced the source checkout",
    )


class InstalledDaemon:
    """The installed glifistored, started from the installed configuration."""

    def __init__(self, lifecycle: Lifecycle, log: Path) -> None:
        self.lifecycle = lifecycle
        self.log = log
        self.port = configured_port(lifecycle.configuration)
        self.process: subprocess.Popen[bytes] | None = None
        self.systemd = systemd_is_pid_one()

    def start(self) -> bool:
        if self.systemd:
            if not _run(["systemctl", "start", "glifistored.service"], log=self.log):
                return False
        else:
            _note(self.log, "no systemd; starting the installed daemon as the service account")
            stream = (self.log.parent / f"{self.log.stem}-daemon-output.log").open("ab")
            try:
                self.process = subprocess.Popen(  # noqa: S603 - fixed installed executable
                    [
                        "runuser",
                        "-u",
                        self.lifecycle.tokens["SERVICE_USER"],
                        "--",
                        self.lifecycle.tool("glifistored"),
                        "--config",
                        str(self.lifecycle.configuration),
                    ],
                    stdout=stream,
                    stderr=subprocess.STDOUT,
                )
            except OSError as error:
                _note(self.log, f"cannot start the installed daemon: {error}")
                return False
        started = wait_for_port(self.port, time.monotonic() + DAEMON_READY_TIMEOUT)
        _note(self.log, f"daemon ready on 127.0.0.1:{self.port}: {started}")
        return started

    def stop(self) -> bool:
        if self.systemd:
            stopped = _run(["systemctl", "stop", "glifistored.service"], log=self.log)
        elif self.process is None:
            return True
        else:
            self.process.terminate()
            try:
                status = self.process.wait(timeout=120)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=30)
                _note(self.log, "the daemon ignored SIGTERM and was killed")
                return False
            finally:
                self.process = None
            # Python reports death-by-signal as a negative returncode (-SIGTERM == -15).
            # That is the orderly stop we asked for, not a packaging failure.
            _note(self.log, f"daemon exit status: {status}")
            stopped = status == 0 or status == -signal.SIGTERM
        gone = wait_for_port(self.port, time.monotonic() + DAEMON_READY_TIMEOUT, gone=True)
        _note(self.log, f"port {self.port} released: {gone}")
        return stopped and gone

    def client(self, *arguments: str) -> bool:
        build = self.lifecycle.consumer_build
        if build is None:
            return False
        return _run(
            [
                str(build / "glifistore_daemon_smoke"),
                "--port",
                str(self.port),
                *arguments,
            ],
            log=self.log,
            timeout=300,
        )


def run_service_lifecycle(lifecycle: Lifecycle) -> None:
    """Start, health-check and stop the packaged unit through systemd itself."""
    recorder, log = lifecycle.recorder, lifecycle.log("service-lifecycle")
    unit = f"{lifecycle.layout['unitdir']}/glifistored.service"
    # Always retain a non-empty log: emit_evidence refuses an empty evidence_ref, and
    # containers without systemd-analyze previously recorded BLOCKED with a missing file.
    _note(log, f"unit={unit}")
    if shutil.which("systemd-analyze") is not None:
        _note(log, "unit verification below is syntax only, not a service run")
        _run(["systemd-analyze", "verify", unit], log=log)
    if not systemd_is_pid_one():
        _note(log, "systemd is not PID 1; the packaged unit cannot be started here")
        recorder.record(
            "service-lifecycle",
            "BLOCKED",
            log=log.name,
            detail="systemd is not PID 1 in this target, so the packaged unit cannot be started",
        )
        recorder.limitations.append(
            "The packaged systemd unit was not started: this target does not run systemd as "
            "PID 1. The protocol and restart rows used the installed daemon directly instead."
        )
        recorder.residuals.append(
            "linux-systemd-service-run=No retained run has started, health-checked and stopped "
            "glifistored.service through systemd|a systemd-enabled packaging target in CI"
        )
        return
    port = configured_port(lifecycle.configuration)
    started = (
        _run(["systemctl", "daemon-reload"], log=log)
        and _run(["systemctl", "enable", "--now", "glifistored.service"], log=log)
        and wait_for_port(port, time.monotonic() + DAEMON_READY_TIMEOUT)
        and _run(["systemctl", "is-active", "--quiet", "glifistored.service"], log=log)
    )
    _note(log, f"service reached 127.0.0.1:{port}: {started}")
    stopped = (
        _run(["systemctl", "stop", "glifistored.service"], log=log)
        and _run(
            ["systemctl", "is-active", "--quiet", "glifistored.service"],
            log=log,
            expect_failure=True,
        )
        and _run(["systemctl", "disable", "glifistored.service"], log=log)
    )
    recorder.record("service-lifecycle", "PASS" if started and stopped else "FAIL", log=log.name)


def run_protocol(lifecycle: Lifecycle) -> None:
    """PUT, exact GET, ERASE, NOT_FOUND and a fenced BACKUP through the packaged daemon."""
    recorder, log = lifecycle.recorder, lifecycle.log("put-get-erase")
    daemon = InstalledDaemon(lifecycle, log)
    if not daemon.start():
        daemon.stop()
        recorder.record("put-get-erase", "FAIL", log=log.name, detail="the daemon never became ready")
        return
    key, value = "linux-package-lifecycle", "installed-daemon-value"
    exercised = (
        daemon.client("--command", "health")
        and daemon.client("--command", "put", "--key", key, "--value", value)
        and daemon.client("--command", "get", "--key", key, "--expect", value)
        and daemon.client("--command", "erase", "--key", key)
        and daemon.client("--command", "expect-not-found", "--key", key)
    )
    # The fenced BACKUP destination has to live under the state directory: that is
    # the only path the packaged unit may write to. It is a test artefact, so it is
    # removed again to leave the durable Store exactly as the protocol left it.
    # Do not pre-create the destination: Store backup opens it with create_new and
    # refuses a path that already exists (sequence_conflict → wire INTERNAL_ERROR).
    backup = lifecycle.state_directory / f"backup-{os.getpid()}"
    if exercised:
        exercised = daemon.client("--command", "backup", "--destination", str(backup))
        _note(log, f"backup produced: {sorted(path.name for path in backup.glob('*'))}")
    daemon.stop()
    shutil.rmtree(backup, ignore_errors=True)
    recorder.record("put-get-erase", "PASS" if exercised else "FAIL", log=log.name)


def run_restart_recovery(lifecycle: Lifecycle) -> None:
    """A value committed before a graceful stop must come back byte-exact afterwards."""
    recorder, log = lifecycle.recorder, lifecycle.log("restart-recovery")
    daemon = InstalledDaemon(lifecycle, log)
    key, value = "linux-package-recovery", "durable-across-restart"
    if not daemon.start():
        daemon.stop()
        recorder.record("restart-recovery", "FAIL", log=log.name, detail="the daemon never started")
        return
    stored = daemon.client("--command", "put", "--key", key, "--value", value)
    stopped = daemon.stop()
    verified = stopped and _run(
        [
            "runuser",
            "-u",
            lifecycle.tokens["SERVICE_USER"],
            "--",
            lifecycle.tool("glifistore_verify_store"),
            "--",
            str(lifecycle.state_directory),
        ],
        log=log,
    )
    recovered = False
    if stored and verified and daemon.start():
        recovered = daemon.client("--command", "get", "--key", key, "--expect", value)
    daemon.stop()
    recorder.record("restart-recovery", "PASS" if recovered else "FAIL", log=log.name)


def run_config_preservation(lifecycle: Lifecycle) -> None:
    """An operator edit must survive reinstalling the same package."""
    recorder, log = lifecycle.recorder, lifecycle.log("config-preservation")
    configuration = lifecycle.configuration
    lifecycle.marker = f"# retained-config-{os.environ.get('GITHUB_RUN_ID', 'local')}"
    try:
        with configuration.open("a", encoding="utf-8") as stream:
            stream.write(f"{lifecycle.marker}\n")
    except OSError as error:
        recorder.record("config-preservation", "FAIL", log=log.name, detail=str(error))
        return
    _note(log, f"marker appended to {configuration}")
    packages = [str(package) for package in _primary_packages(lifecycle)]
    environment = dict(os.environ)
    environment["DEBIAN_FRONTEND"] = "noninteractive"
    if lifecycle.backend == "deb":
        # --force-confdef/--force-confold state the policy explicitly instead of
        # relying on whatever the frontend would have chosen: an operator edit wins.
        reinstalled = _run(
            [
                "apt-get",
                "install",
                "-y",
                "--reinstall",
                "-o",
                "Dpkg::Options::=--force-confdef",
                "-o",
                "Dpkg::Options::=--force-confold",
                *packages,
            ],
            log=log,
            environment=environment,
            timeout=BUILD_TIMEOUT,
        )
    else:
        reinstalled = _run(
            ["rpm", "--upgrade", "--replacepkgs", "--replacefiles", *packages],
            log=log,
            timeout=BUILD_TIMEOUT,
        )
    survived = reinstalled and lifecycle.marker in configuration.read_text(encoding="utf-8")
    replaced = sorted(configuration.parent.glob(f"{configuration.name}.*"))
    _note(log, f"marker survived: {survived}; sibling files: {[p.name for p in replaced]}")
    recorder.record("config-preservation", "PASS" if survived else "FAIL", log=log.name)


def run_remove(lifecycle: Lifecycle) -> None:
    """Remove, then purge, proving the configuration and data policy the package promises."""
    recorder, log = lifecycle.recorder, lifecycle.log("package-remove")
    runtime = lifecycle.tokens["RUNTIME_PACKAGE"]
    configuration = lifecycle.configuration
    environment = dict(os.environ)
    environment["DEBIAN_FRONTEND"] = "noninteractive"
    contents = (
        sorted(path.name for path in lifecycle.state_directory.iterdir())
        if lifecycle.state_directory.is_dir()
        else []
    )
    _note(log, f"durable data before removal: {contents}")
    if not contents:
        recorder.record(
            "package-remove",
            "NOT_RUN",
            log=log.name,
            detail="no durable data was written, so data retention cannot be proven",
        )
        return

    payload = [
        sys.executable,
        str(lifecycle.root / "engineering/tools/verify_package_payload.py"),
        "--root",
        "/",
        "--layout",
        str(lifecycle.metadata / "rendered-metadata.json"),
    ]
    if lifecycle.backend == "deb":
        removed = _run(["apt-get", "remove", "-y", runtime], log=log, environment=environment)
        retained = removed and _run(
            [
                *payload,
                *[a for c in REMOVED_COMPONENTS for a in ("--absent-component", c)],
                *[a for c in RETAINED_COMPONENTS for a in ("--component", c)],
            ],
            log=log,
        )
        kept_marker = lifecycle.marker in configuration.read_text(encoding="utf-8")
        _note(log, f"operator configuration survived remove: {kept_marker}")
        purged = retained and _run(
            ["apt-get", "purge", "-y", runtime], log=log, environment=environment
        )
        final = purged and _run(
            [
                *payload,
                "--absent-component",
                "configuration",
                "--component",
                "data",
            ],
            log=log,
        )
        _note(log, "purge removed the conffile and left the durable Store directory in place")
        passed = bool(kept_marker and final)
    else:
        erased = _run(["rpm", "--erase", runtime], log=log)
        # rpm either leaves a modified %config(noreplace) in place or renames it to
        # .rpmsave. Both preserve the operator edit; which one happens is rpm's
        # decision, so the check accepts either and records what it observed.
        saved = configuration.with_name(f"{configuration.name}.rpmsave")
        surviving = next(
            (path for path in (saved, configuration) if path.is_file()),
            None,
        )
        kept_marker = surviving is not None and lifecycle.marker in surviving.read_text(
            encoding="utf-8"
        )
        _note(
            log,
            f"operator configuration survived erase in {surviving}: {kept_marker}. RPM has no "
            "purge action, so erase is the terminal state for the configuration.",
        )
        final = erased and _run(
            [
                *payload,
                *[a for c in REMOVED_COMPONENTS for a in ("--absent-component", c)],
                "--component",
                "data",
            ],
            log=log,
        )
        passed = bool(kept_marker and final)

    library_packages = [
        lifecycle.tokens["DEV_PACKAGE"],
        lifecycle.tokens["LIB_PACKAGE"],
    ]
    if lifecycle.backend == "deb":
        _run(["apt-get", "purge", "-y", *library_packages], log=log, environment=environment)
    else:
        _run(["rpm", "--erase", f"{runtime}-devel", f"{runtime}-libs"], log=log)
    cleared = _run(
        [
            *payload,
            "--absent-component",
            "development",
            "--absent-component",
            "library",
            "--component",
            "data",
        ],
        log=log,
    )
    remaining = sorted(path.name for path in lifecycle.state_directory.iterdir())
    _note(log, f"durable data after removal: {remaining}")
    recorder.record(
        "package-remove",
        "PASS" if passed and cleared and remaining == contents else "FAIL",
        log=log.name,
    )


UPGRADE_SEED_KEY = "linux-package-upgrade"
UPGRADE_SEED_VALUE = "durable-across-n1-upgrade"


def run_upgrade(recorder: Recorder, context: dict[str, Any]) -> None:
    """Record pre-exercise upgrade status when the install walk will not run here.

    When a SemVer predecessor exists and GLIFISTORE_N1_PACKAGE_DIR supplies sealed
    packages, recording is deferred to ``run_upgrade_exercise`` inside the native
    lifecycle. N-1 is never rebuilt from HEAD.
    """
    previous = context["previous"]
    if not previous["available"]:
        try:
            if supplied_n1_package_dir() is not None:
                recorder.record(
                    "package-upgrade",
                    "FAIL",
                    detail=(
                        f"{N1_PACKAGE_DIR_ENVIRONMENT} is set but the release context has no "
                        f"SemVer predecessor ({previous['reason']})"
                    ),
                )
                return
        except N1PackageError as error:
            recorder.record("package-upgrade", "FAIL", detail=str(error))
            return
        recorder.record(
            "package-upgrade",
            "NOT_APPLICABLE_INITIAL_BASELINE",
            detail=previous["reason"],
        )
        return
    try:
        if upgrade_exercise_requested(previous):
            # Native lifecycle records PASS/FAIL after install→seed→upgrade→verify.
            return
    except N1PackageError as error:
        recorder.record("package-upgrade", "FAIL", detail=str(error))
        return
    tag = previous["tag"]
    recorder.record(
        "package-upgrade",
        "NOT_RUN",
        detail=(
            f"previous release {tag} is selected; sealed N-1 package artifacts were not "
            f"supplied via {N1_PACKAGE_DIR_ENVIRONMENT}, so upgrade continuity was not "
            "exercised. This run never rebuilds N-1 from HEAD."
        ),
    )


def finalize_deferred_upgrade(recorder: Recorder, context: dict[str, Any]) -> None:
    """If sealed N-1 bytes were supplied but the native walk never ran, say so."""
    if "package-upgrade" in recorder.statuses:
        return
    previous = context["previous"]
    if not previous.get("available"):
        return
    try:
        directory = supplied_n1_package_dir()
    except N1PackageError as error:
        recorder.record("package-upgrade", "FAIL", detail=str(error))
        return
    if directory is None:
        return
    tag = previous["tag"]
    recorder.record(
        "package-upgrade",
        "NOT_RUN",
        detail=(
            f"previous release {tag} is selected and {N1_PACKAGE_DIR_ENVIRONMENT}="
            f"{directory} was set, but the install→seed→upgrade→verify walk did not run "
            "because the native install lifecycle was not reached in this process. "
            "This run never rebuilds N-1 from HEAD."
        ),
    )


def run_upgrade_exercise(lifecycle: Lifecycle, context: dict[str, Any]) -> None:
    """Install sealed N-1, seed durable data, upgrade to N, verify byte-exact continuity."""
    recorder = lifecycle.recorder
    log = lifecycle.log("package-upgrade")
    install_log = lifecycle.log("package-install")
    previous = context["previous"]
    tag = previous["tag"]
    version = previous["version"]
    try:
        n1_dir = supplied_n1_package_dir()
        if n1_dir is None:
            raise N1PackageError(
                f"{N1_PACKAGE_DIR_ENVIRONMENT} was unset while an upgrade exercise was requested"
            )
        n1_packages = select_linux_n1_packages(n1_dir, lifecycle.backend, version)
    except N1PackageError as error:
        _note(log, f"refused: {error}")
        recorder.record("package-upgrade", "FAIL", log=log.name, detail=str(error))
        run_install(lifecycle)
        return

    _note(log, f"previous release {tag} (version {version})")
    _note(log, f"{N1_PACKAGE_DIR_ENVIRONMENT}={n1_dir}")
    _note(log, f"N-1 packages: {[package.name for package in n1_packages]}")
    _note(install_log, f"upgrade path: installing sealed N-1 from {tag} before N")

    if not _install_package_files(
        lifecycle, n1_packages, install_log, verify_rendered_payload=False
    ):
        detail = f"sealed N-1 packages from {tag} failed to install"
        _note(log, f"refused: {detail}")
        recorder.record("package-upgrade", "FAIL", log=log.name, detail=detail)
        recorder.record("package-install", "FAIL", log=install_log.name, detail=detail)
        return

    if not _build_external_consumer(lifecycle, log):
        detail = "the smoke consumer could not be built against the installed N-1 prefix"
        _note(log, f"refused: {detail}")
        recorder.record("package-upgrade", "FAIL", log=log.name, detail=detail)
        run_install(lifecycle)
        return

    daemon = InstalledDaemon(lifecycle, log)
    seeded = False
    if daemon.start():
        seeded = daemon.client(
            "--command", "put", "--key", UPGRADE_SEED_KEY, "--value", UPGRADE_SEED_VALUE
        )
    daemon.stop()
    if not seeded:
        detail = "could not seed durable data under the installed N-1 package"
        _note(log, f"refused: {detail}")
        recorder.record("package-upgrade", "FAIL", log=log.name, detail=detail)
        run_install(lifecycle)
        return
    _note(log, f"seeded key={UPGRADE_SEED_KEY!r} under N-1")

    n_packages = _primary_packages(lifecycle)
    _note(log, f"upgrading to N packages: {[package.name for package in n_packages]}")
    if not _install_package_files(lifecycle, n_packages, install_log):
        detail = "upgrade from sealed N-1 to the built N packages failed"
        _note(log, f"refused: {detail}")
        recorder.record("package-upgrade", "FAIL", log=log.name, detail=detail)
        recorder.record("package-install", "FAIL", log=install_log.name, detail=detail)
        return
    recorder.record("package-install", "PASS", log=install_log.name)

    if not _build_external_consumer(lifecycle, log):
        detail = "the smoke consumer could not be rebuilt against the upgraded N prefix"
        _note(log, f"refused: {detail}")
        recorder.record("package-upgrade", "FAIL", log=log.name, detail=detail)
        return

    recovered = False
    if daemon.start():
        recovered = daemon.client(
            "--command",
            "get",
            "--key",
            UPGRADE_SEED_KEY,
            "--expect",
            UPGRADE_SEED_VALUE,
        )
    daemon.stop()
    if recovered:
        _note(log, "seeded value recovered byte-exact after upgrade to N")
        recorder.record("package-upgrade", "PASS", log=log.name)
    else:
        detail = (
            f"value seeded under {tag} was not recovered byte-exact after upgrading to "
            f"{context['product_version']}"
        )
        _note(log, f"refused: {detail}")
        recorder.record("package-upgrade", "FAIL", log=log.name, detail=detail)


def run_target_lifecycle(
    lifecycle: Lifecycle, *, source: SourceArchive | None, context: dict[str, Any]
) -> None:
    """Walk the lifecycle in order, blocking a phase whose prerequisite did not pass."""
    recorder = lifecycle.recorder
    run_lint(lifecycle)
    if source is None:
        recorder.pending(
            ("package-build",), "BLOCKED", "no admitted source archive to build from"
        )
    elif recorder.passed("package-lint"):
        run_build(lifecycle, source)
    else:
        recorder.record(
            "package-build", "BLOCKED", detail="the rendered metadata did not pass its lint"
        )

    if recorder.passed("package-build"):
        run_inspect(lifecycle)
        run_prefix_isolation(lifecycle)
    else:
        recorder.pending(
            ("package-inspect", "prefix-isolation"), "BLOCKED", "no package was built to inspect"
        )

    if recorder.passed("package-inspect"):
        try:
            exercise = upgrade_exercise_requested(context["previous"])
        except N1PackageError as error:
            recorder.record("package-upgrade", "FAIL", detail=str(error))
            exercise = False
        if exercise and "package-upgrade" not in recorder.statuses:
            run_upgrade_exercise(lifecycle, context)
        else:
            run_install(lifecycle)
    else:
        recorder.record(
            "package-install", "BLOCKED", detail="the built package did not pass inspection"
        )

    if recorder.passed("package-install"):
        run_installed_sdk_matrix(lifecycle)
        run_external_consumer(lifecycle)
        run_service_lifecycle(lifecycle)
    else:
        recorder.pending(
            ("external-consumer", "service-lifecycle"),
            "BLOCKED",
            "no installed package to verify",
        )

    if recorder.passed("external-consumer"):
        run_protocol(lifecycle)
    else:
        recorder.record(
            "put-get-erase",
            "BLOCKED",
            detail="the external consumer that drives the protocol was not built",
        )

    if recorder.passed("put-get-erase"):
        run_restart_recovery(lifecycle)
    else:
        recorder.record(
            "restart-recovery", "BLOCKED", detail="the protocol exercise did not pass first"
        )

    if recorder.passed("package-install"):
        run_config_preservation(lifecycle)
    else:
        recorder.record(
            "config-preservation", "BLOCKED", detail="no installed configuration to preserve"
        )

    if recorder.passed("config-preservation"):
        run_remove(lifecycle)
    else:
        recorder.record(
            "package-remove",
            "BLOCKED",
            detail="removal is only meaningful after configuration preservation was proven",
        )


# --------------------------------------------------------------------------- #
# Driver
# --------------------------------------------------------------------------- #


def overall_result(statuses: dict[str, str]) -> str:
    reported = set(statuses.values())
    if "FAIL" in reported:
        return "FAIL"
    if reported <= SETTLED_RESULTS:
        return "PASS"
    if "OPEN_GATE" in reported:
        return "OPEN_GATE"
    if "BLOCKED" in reported:
        return "BLOCKED"
    return "NOT_RUN"


def lifecycle_state(statuses: dict[str, str]) -> str:
    state = "NONE"
    for name, required in LIFECYCLE_CHAIN:
        if not all(statuses.get(check) == "PASS" for check in required):
            break
        state = name
    return state


def resolve_overrides(backend: str) -> dict[str, str]:
    """Ask the native tooling where it puts libraries instead of guessing."""
    if backend != "rpm":
        return {}
    overrides: dict[str, str] = {}
    for key, macro in (("libdir", "%{_libdir}"), ("unitdir", "%{_unitdir}")):
        value = _capture(["rpm", "--eval", macro])
        if value and value.startswith("/") and "%" not in value:
            overrides[key] = value.rstrip("/")
    return overrides


def execute(
    *,
    backend: str,
    profile: str,
    stage: str,
    root: Path,
    release_context: Path,
    directory: Path,
    work: Path,
    inner: bool,
    matrix_path: Path = MATRIX_PATH,
) -> tuple[str, Path]:
    if backend not in LINUX_BACKENDS:
        raise LinuxBackendError(f"unsupported Linux packaging backend: {backend}")
    if profile not in PROFILES:
        raise LinuxBackendError(f"unsupported CI profile: {profile}")
    if stage not in STAGES:
        raise LinuxBackendError(f"unsupported lifecycle stage: {stage}")

    root = root.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)
    matrix = load_matrix(matrix_path)
    context = load_release_context(release_context)
    package = context["package_versions"][backend]
    declared = list(matrix_backend(matrix, backend)["checks"])
    recorder = Recorder(directory)

    target = matrix_target(matrix, backend, profile)
    runtime = None if inner else container_runtime()
    native, fallback, reason = native_availability(backend, stage=stage, inner=inner)

    recorder.record(
        "structural-metadata",
        "PASS",
        log=recorder.write_log(
            "structural-metadata.log",
            "\n".join(
                (
                    f"backend={backend} profile={profile} stage={stage}",
                    f"release_context={release_context}",
                    f"product_version={context['product_version']} "
                    f"package_version={package['package_version']} "
                    f"package_revision={context['package_revision']}",
                    f"commit={context['git']['commit']} tree_clean={context['git']['tree_clean']}",
                    f"target={target.identifier if target else 'none for this profile and arch'}",
                    f"host={platform.system()}/{platform.machine()} inner={inner}",
                    f"native_lifecycle={native} container_runtime={runtime}",
                    "structural metadata resolved from the release context, "
                    "not a hard-coded version",
                )
            ),
        ),
    )
    recorder.limitations.extend(package["limitations"])

    render_log = directory / "package-metadata-render.log"
    metadata = directory / "metadata"
    tokens: dict[str, str] = {}
    layout: dict[str, str] = {}
    try:
        manifest = render(
            release_context,
            backend,
            metadata,
            root=root,
            overrides=resolve_overrides(backend),
            replace=True,
        )
        tokens, layout = manifest["tokens"], manifest["layout"]
        _note(render_log, f"rendered {len(manifest['files'])} files into {metadata}")
        for entry in manifest["files"]:
            _note(render_log, f"  {entry['mode']} {entry['sha256'][:16]} {entry['path']}")
        _note(render_log, f"layout: {json.dumps(layout, sort_keys=True)}")
        recorder.record("package-metadata-render", "PASS", log=render_log.name)
    except (LinuxBackendError, PackageFrameworkError, ReleaseContextError, RenderError) as error:
        _note(render_log, f"refused: {error}")
        recorder.record(
            "package-metadata-render", "FAIL", log=render_log.name, detail=str(error)
        )

    run_upgrade(recorder, context)

    if recorder.passed("package-metadata-render") and native:
        bootstrap = directory / "dependency-bootstrap.log"
        if bootstrap_dependencies(backend, log=bootstrap):
            source = admit_source(
                recorder, root=root, work=work, profile=profile, product_version=context["product_version"]
            )
            run_target_lifecycle(
                Lifecycle(
                    recorder=recorder,
                    backend=backend,
                    root=root,
                    work=work,
                    metadata=metadata,
                    layout=layout,
                    tokens=tokens,
                    artifacts=directory / "artifacts",
                ),
                source=source,
                context=context,
            )
        else:
            recorder.pending(
                declared,
                "BLOCKED",
                f"the {backend} toolchain could not be installed in this target; "
                "see dependency-bootstrap.log",
            )
    elif recorder.passed("package-metadata-render") and runtime is not None and target is not None:
        if not target.image or not target.image_digest:
            recorder.pending(
                declared,
                "BLOCKED",
                f"target {target.identifier} does not pin a container image digest, and an "
                "unpinned image is never built",
            )
        elif os.environ.get(CONTAINER_ENVIRONMENT) != "1":
            recorder.pending(
                declared,
                "NOT_RUN",
                "the lifecycle runs inside the digest-pinned container and is opt-in; "
                f"set {CONTAINER_ENVIRONMENT}=1",
            )
        else:
            dispatched = dispatch_container(
                recorder,
                backend=backend,
                profile=profile,
                stage=stage,
                root=root,
                release_context=release_context,
                target=target,
                runtime=runtime,
            )
            # Prefer the inner report whether the container exited 0 or not: a FAIL
            # after writing evidence must not be rewritten as BLOCKED.
            preferred = prefer_inner_container_evidence(directory, backend, profile, stage)
            if preferred is not None:
                return preferred
            if dispatched:
                recorder.pending(
                    declared,
                    "BLOCKED",
                    "the container run produced no package evidence; see container-dispatch.log",
                )
            else:
                recorder.pending(
                    declared,
                    "BLOCKED",
                    f"the {runtime} run of {target.image} did not complete; "
                    "see container-dispatch.log",
                )
    else:
        recorder.pending(declared, fallback, reason or "the lifecycle was not reached in this run")

    finalize_deferred_upgrade(recorder, context)

    # Safety net: a check the matrix declares but this driver never reaches must
    # still say so rather than inherit someone else's status.
    recorder.pending(declared, "NOT_RUN", "this lifecycle never reached the check")

    if profile == "release":
        recorder.limitations.append(
            "No sealed deb or rpm artifact is admitted for publication yet; "
            "required_for_release stays false for both backends."
        )
    recorder.limitations.append(
        ".github/workflows/package-ci.yml retains this evidence per profile and "
        "GATE-PACKAGE-LIFECYCLE cites that workflow, but the gate is open, so this document "
        "proves only what its own checks say."
    )
    recorder.residuals.append(
        "linux-package-ci-gate=GATE-PACKAGE-LIFECYCLE stays IMPLEMENTATA for deb and rpm"
        "|no retained run has executed the container lifecycle"
    )

    plan = check_plan(
        matrix,
        backend,
        profile,
        default_status="NOT_RUN",
        statuses=recorder.statuses,
        evidence_refs=recorder.evidence_refs,
        details=recorder.details,
    )
    plan_path = write_json(directory / "check-plan.json", plan, replace=True)
    result = overall_result(recorder.statuses)
    evidence = directory / evidence_filename(backend, profile, stage)
    evidence.unlink(missing_ok=True)
    emit_evidence(
        backend=backend,
        profile=profile,
        stage=stage,
        result=result,
        lifecycle_state=lifecycle_state(recorder.statuses),
        context_path=release_context,
        check_plan=plan_path,
        output=evidence,
        limitations=recorder.limitations,
        residuals=recorder.residuals,
        matrix_path=matrix_path,
    )
    return result, evidence


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--backend", choices=LINUX_BACKENDS, required=True)
    result.add_argument("--profile", choices=PROFILES, required=True)
    result.add_argument("--stage", choices=STAGES, default="full")
    result.add_argument("--root", type=Path, default=REPO_ROOT)
    result.add_argument("--release-context", type=Path, required=True)
    result.add_argument("--output-dir", type=Path, required=True)
    result.add_argument("--work-dir", type=Path)
    result.add_argument("--matrix", type=Path, default=MATRIX_PATH)
    result.add_argument(
        "--inner",
        action="store_true",
        help="this process is the disposable target; never dispatch to a container",
    )
    return result


def main() -> int:
    arguments = parser().parse_args()
    work = arguments.work_dir or arguments.output_dir / "work"
    try:
        result, evidence = execute(
            backend=arguments.backend,
            profile=arguments.profile,
            stage=arguments.stage,
            root=arguments.root,
            release_context=arguments.release_context,
            directory=arguments.output_dir,
            work=work,
            inner=arguments.inner,
            matrix_path=arguments.matrix,
        )
    except (
        LinuxBackendError,
        PackageEvidenceError,
        PackageFrameworkError,
        PackageMatrixError,
        ReleaseContextError,
        RenderError,
        OSError,
    ) as error:
        print(f"{arguments.backend} packaging FAILED: {error}", file=sys.stderr)
        return 1
    print(f"PACKAGE-CI {arguments.backend} {arguments.profile} {arguments.stage} {result} {evidence}")
    return 0 if result != "FAIL" else 1


if __name__ == "__main__":
    raise SystemExit(main())
