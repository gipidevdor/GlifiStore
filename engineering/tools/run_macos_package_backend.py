#!/usr/bin/env python3
"""Run the MacPorts or Homebrew packaging lifecycle and emit honest evidence.

The adapter renders the backend metadata from the release context and, when the
host can actually do it, walks the lifecycle: lint or audit, build, install,
inspect, prefix isolation, external consumer, daemon exercise, deactivate and
activate or uninstall, cleanup. Every check that did not run says so.

MacPorts and Homebrew declare init integrations (unprivileged launchd via
`startupitem.user`/`group`, and `brew services`). When the native lifecycle runs,
those integrations are started, health-checked and stopped. When native is
skipped, both report OPEN_GATE until a retained native run exists.

The native lifecycle mutates the host package manager, so it is opt-in:
set GLIFISTORE_PACKAGE_CI_NATIVE=1 on a runner that may install packages.
"""

from __future__ import annotations

import argparse
import copy
import lzma
import os
import platform
import shutil
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
from engineering.tools.macos_prefix_isolation import PrefixIsolationError, inspect as inspect_links
from engineering.tools.n1_package_artifacts import (
    N1_PACKAGE_DIR_ENVIRONMENT,
    N1PackageError,
    select_macos_n1_source,
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
from engineering.tools.render_macos_packaging import (
    MACOS_BACKENDS,
    TEMPLATES,
    MacPackagingError,
    SourceArchive,
    render_to_file,
    resolve_source,
    source_basename,
)
from engineering.tools.semver_policy import SemverError, normalize_all, parse
from engineering.tools.validate_package_evidence import (
    PackageEvidenceError,
    emit_evidence,
    evidence_filename,
)

NATIVE_ENVIRONMENT = "GLIFISTORE_PACKAGE_CI_NATIVE"
SOURCE_URL_ENVIRONMENT = "GLIFISTORE_SOURCE_ARCHIVE_URL"
SOURCE_SHA256_ENVIRONMENT = "GLIFISTORE_SOURCE_ARCHIVE_SHA256"
SOURCE_SIZE_ENVIRONMENT = "GLIFISTORE_SOURCE_ARCHIVE_SIZE"
SOURCE_RMD160_ENVIRONMENT = "GLIFISTORE_SOURCE_ARCHIVE_RMD160"

TOOLS = {"macports": "port", "homebrew": "brew"}
TEMPLATE_PATHS = {backend: path.as_posix() for backend, path in TEMPLATES.items()}
LIFECYCLE_CHAIN = (
    ("STRUCTURAL", ("structural-metadata", "package-metadata-render")),
    ("BUILT", ("package-lint", "package-build")),
    ("INSTALLED", ("package-install", "package-inspect", "prefix-isolation")),
    ("FUNCTIONALLY_VERIFIED", ("external-consumer", "put-get-erase", "restart-recovery")),
    ("LIFECYCLE_VERIFIED", ("service-lifecycle", "package-remove")),
    ("UPGRADE_VERIFIED", ("package-upgrade",)),
)
SERVICE_GATE = {
    "macports": (
        "The port declares an unprivileged launchd startup item "
        "(startupitem.user/group glifistore), but this run did not exercise "
        "`port load/unload glifistore` (native lifecycle not enabled)."
    ),
    "homebrew": (
        "The formula declares a brew services block, but this run did not exercise "
        "`brew services start/stop glifistore` (native lifecycle not enabled)."
    ),
}
BUILD_TIMEOUT = 60 * 60
SHORT_TIMEOUT = 15 * 60
DAEMON_READY_TIMEOUT = 60.0
UPGRADE_SEED_KEY = b"macos-package-upgrade".hex()
UPGRADE_SEED_VALUE = b"durable-across-n1-upgrade".hex()


class MacBackendError(RuntimeError):
    pass


def context_for_n1_version(context: dict[str, Any], version: str) -> dict[str, Any]:
    """Release-context clone whose product/package versions describe sealed N-1."""
    try:
        parsed = parse(version, allow_v_prefix=False)
    except SemverError as error:
        raise N1PackageError(f"sealed N-1 product version is invalid: {error}") from error
    cloned = copy.deepcopy(context)
    cloned["product_version"] = version
    cloned["semver"] = parsed.as_dict()
    cloned["package_versions"] = {
        backend: package.as_dict()
        for backend, package in normalize_all(parsed, context["package_revision"]).items()
    }
    return cloned


def resolve_n1_macos_source(context: dict[str, Any]) -> tuple[str, SourceArchive]:
    """Select the sealed N-1 source archive and pin it as a file:// SourceArchive."""
    previous = context["previous"]
    version = previous["version"]
    n1_dir = supplied_n1_package_dir()
    if n1_dir is None:
        raise N1PackageError(
            f"{N1_PACKAGE_DIR_ENVIRONMENT} was unset while an upgrade exercise was requested"
        )
    archive = select_macos_n1_source(n1_dir, version)
    source = resolve_source(
        url=archive.resolve().as_uri(),
        sha256=digest(archive),
        size=archive.stat().st_size,
        product_version=version,
        profile="pr",
    )
    return version, source


def seed_and_verify_upgrade(
    *,
    root: Path,
    executable: Path,
    work: Path,
    log: Path,
) -> bool:
    """PUT a durable seed under N-1 semantics, then GET it after upgrade to N."""
    daemon = InstalledDaemon(executable, work / "upgrade-data", log)
    seeded = False
    if daemon.start():
        command, environment = client_command(
            root, daemon.port, "put", "--key-hex", UPGRADE_SEED_KEY, "--value-hex", UPGRADE_SEED_VALUE
        )
        seeded = _run(command, log=log, environment=environment, timeout=300)
    daemon.stop()
    if not seeded:
        _note(log, "could not seed durable data under the installed N-1 package")
        return False
    _note(log, f"seeded key={UPGRADE_SEED_KEY!r} under N-1")
    return True


def verify_upgrade_seed(*, root: Path, executable: Path, work: Path, log: Path) -> bool:
    daemon = InstalledDaemon(executable, work / "upgrade-data", log)
    recovered = False
    if daemon.start():
        command, environment = client_command(root, daemon.port, "get", "--key-hex", UPGRADE_SEED_KEY)
        got = _capture(command, log=log, environment=environment, timeout=300)
        _note(log, f"exact GET after upgrade: expected {UPGRADE_SEED_VALUE}, got {got}")
        recovered = got == UPGRADE_SEED_VALUE
    daemon.stop()
    return recovered


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
            raise MacBackendError(f"check {check} was already recorded")
        self.statuses[check] = status
        if log is not None:
            self.evidence_refs[check] = log
        if detail is not None:
            self.details[check] = detail

    def write_log(self, name: str, text: str) -> str:
        path = self.directory / name
        path.write_text(text if text.endswith("\n") else text + "\n", encoding="utf-8")
        return name

    def pending(self, checks: Sequence[str], status: str, detail: str) -> None:
        for check in checks:
            if check not in self.statuses:
                self.record(check, status, detail=detail)


def _run(
    command: Sequence[str],
    *,
    log: Path,
    cwd: Path | None = None,
    environment: dict[str, str] | None = None,
    timeout: int = SHORT_TIMEOUT,
) -> bool:
    """Run one lifecycle command, appending its full transcript to `log`."""
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
    return completed.returncode == 0


def _capture(
    command: Sequence[str],
    *,
    log: Path,
    environment: dict[str, str] | None = None,
    timeout: int = SHORT_TIMEOUT,
) -> str:
    """Run a command for its stdout; an empty string means it produced nothing usable."""
    try:
        completed = subprocess.run(
            list(command),
            check=False,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        with log.open("a", encoding="utf-8") as stream:
            stream.write(f"$ {' '.join(command)}\nrefused: {error}\n")
        return ""
    with log.open("a", encoding="utf-8") as stream:
        stream.write(f"$ {' '.join(command)}\n{completed.stderr}")
        stream.write(f"exit status: {completed.returncode}\n")
    return completed.stdout.strip() if completed.returncode == 0 else ""


def _note(log: Path, text: str) -> None:
    with log.open("a", encoding="utf-8") as stream:
        stream.write(text if text.endswith("\n") else text + "\n")


def working_source_archive(root: Path, directory: Path, product_version: str) -> Path:
    """A build-from-tree archive of HEAD; never a release source."""
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / source_basename(product_version)
    if archive.is_file():
        return archive
    tarball = directory / f"{archive.stem}.tar"
    try:
        with tarball.open("wb") as stream:
            completed = subprocess.run(
                [
                    "git",
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
                text=False,
                timeout=SHORT_TIMEOUT,
            )
        if completed.returncode != 0:
            raise MacBackendError(
                f"git archive failed: {completed.stderr.decode('utf-8', 'replace').strip()}"
            )
        with tarball.open("rb") as source, lzma.open(archive, "wb", preset=0) as target:
            shutil.copyfileobj(source, target)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise MacBackendError(f"cannot build the working source archive: {error}") from error
    finally:
        tarball.unlink(missing_ok=True)
    return archive


def resolve_profile_source(
    *, backend: str, profile: str, context: dict[str, Any], root: Path, work: Path
) -> SourceArchive:
    """Sealed archive for release; a HEAD archive for the build-from-tree profiles."""
    product_version = context["product_version"]
    url = os.environ.get(SOURCE_URL_ENVIRONMENT, "").strip()
    sha256 = os.environ.get(SOURCE_SHA256_ENVIRONMENT, "").strip()
    raw_size = os.environ.get(SOURCE_SIZE_ENVIRONMENT, "").strip()
    size = int(raw_size) if raw_size.isdigit() else None
    rmd160 = os.environ.get(SOURCE_RMD160_ENVIRONMENT, "").strip() or None

    if profile == "release" and not (url and sha256):
        raise MacPackagingError(
            f"the release profile requires a sealed source archive: set "
            f"{SOURCE_URL_ENVIRONMENT} and {SOURCE_SHA256_ENVIRONMENT}; "
            f"a checkout of HEAD is never a release source for {backend}"
        )
    if not url:
        archive = working_source_archive(root, work, product_version)
        url = archive.resolve().as_uri()
        sha256 = digest(archive)
        size = archive.stat().st_size
    return resolve_source(
        url=url,
        sha256=sha256,
        size=size,
        rmd160=rmd160,
        product_version=product_version,
        profile=profile,
    )


def native_availability(backend: str, *, stage: str) -> tuple[bool, str, str]:
    host = platform.system()
    if host != "Darwin":
        return False, "BLOCKED", f"requires a macOS host; this runner is {host}"
    tool = TOOLS[backend]
    if shutil.which(tool) is None:
        return False, "BLOCKED", f"the {tool} command is not installed on this host"
    if stage != "full":
        return False, "NOT_RUN", f"the native lifecycle needs --stage full, not {stage}"
    if os.environ.get(NATIVE_ENVIRONMENT) != "1":
        return (
            False,
            "NOT_RUN",
            f"the native lifecycle installs into the host package manager and is opt-in; "
            f"set {NATIVE_ENVIRONMENT}=1 to run it",
        )
    return True, "NOT_RUN", ""


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def wait_for_port(port: int, deadline: float) -> bool:
    while time.monotonic() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.settimeout(1.0)
            if probe.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.25)
    return False


class InstalledDaemon:
    """The installed glifistored under a driver-owned config and data directory."""

    def __init__(self, executable: Path, work: Path, log: Path) -> None:
        self.executable = executable
        self.data = work / "data"
        self.configuration = work / "glifistored.conf"
        self.log = log
        self.port = free_port()
        self.process: subprocess.Popen[bytes] | None = None
        self.data.mkdir(parents=True, exist_ok=True)
        self.configuration.write_text(
            "profile=production\n"
            "bind=127.0.0.1\n"
            f"port={self.port}\n"
            "shard-pairs=1\n"
            "storage-mode=durable-periodic\n"
            f"data-dir={self.data}\n"
            "open-mode=open-or-create\n"
            "log-format=json\n"
            "shutdown-drain-ms=5000\n",
            encoding="utf-8",
        )

    def start(self) -> bool:
        _note(self.log, f"$ {self.executable} --config {self.configuration}")
        stream = (self.log.parent / f"{self.log.stem}-daemon.log").open("ab")
        self.process = subprocess.Popen(  # noqa: S603 - fixed installed executable
            [str(self.executable), "--config", str(self.configuration)],
            stdout=stream,
            stderr=subprocess.STDOUT,
        )
        started = wait_for_port(self.port, time.monotonic() + DAEMON_READY_TIMEOUT)
        _note(self.log, f"daemon ready on 127.0.0.1:{self.port}: {started}")
        return started

    def stop(self) -> bool:
        if self.process is None:
            return True
        self.process.terminate()
        try:
            status = self.process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=30)
            _note(self.log, "daemon did not stop on SIGTERM and was killed")
            return False
        finally:
            self.process = None
        _note(self.log, f"daemon exit status: {status}")
        return status == 0


def client_command(root: Path, port: int, *arguments: str) -> tuple[list[str], dict[str, str]]:
    environment = dict(os.environ)
    environment["PYTHONPATH"] = str(root / "sdk/python/src")
    command = [
        sys.executable,
        str(root / "scripts/sdk_interop_py.py"),
        "--host",
        "127.0.0.1",
        "--port",
        str(port),
        *arguments,
    ]
    return command, environment


def run_homebrew_service_lifecycle(
    recorder: Recorder, *, root: Path, prefix: Path
) -> None:
    """Start, health-check and stop the formula through `brew services`."""
    log = recorder.directory / "homebrew-service-lifecycle.log"
    sample = prefix / "etc/glifistore/glifistored.conf.sample"
    config = prefix / "etc/glifistore/glifistored.conf"
    if not sample.is_file():
        recorder.record(
            "service-lifecycle",
            "FAIL",
            log=log.name,
            detail=f"missing packaged sample configuration: {sample}",
        )
        return
    if not config.exists():
        shutil.copy2(sample, config)
        _note(log, f"installed operator configuration from {sample.name}")
    started = _run(["brew", "services", "start", "glifistore"], log=log)
    ready = started and wait_for_port(7379, time.monotonic() + DAEMON_READY_TIMEOUT)
    _note(log, f"brew services reached 127.0.0.1:7379: {ready}")
    healthy = False
    if ready:
        command, environment = client_command(
            root,
            7379,
            "put",
            "--key-hex",
            b"brew-services".hex(),
            "--value-hex",
            b"lifecycle".hex(),
        )
        healthy = _run(command, log=log, environment=environment, timeout=300)
    stopped = _run(["brew", "services", "stop", "glifistore"], log=log)
    # Ensure the port is released before later direct-daemon rows reuse the host.
    if ready:
        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
                if probe.connect_ex(("127.0.0.1", 7379)) != 0:
                    break
            time.sleep(0.25)
    recorder.record(
        "service-lifecycle",
        "PASS" if started and ready and healthy and stopped else "FAIL",
        log=log.name,
    )


def exercise_daemon(
    recorder: Recorder, *, root: Path, executable: Path, work: Path, backend: str
) -> None:
    """PUT/GET/ERASE and restart recovery against the installed daemon binary."""
    log = recorder.directory / f"{backend}-daemon.log"
    _note(
        log,
        "The installed daemon is started directly under a driver-owned configuration and data "
        "directory. This is not an init-system managed service run; see the service-lifecycle "
        "check for brew services (Homebrew native) or the MacPorts launchd open gate.",
    )
    daemon = InstalledDaemon(executable, work, log)
    key = b"macos-package".hex()
    value = b"lifecycle".hex()
    recovery_key = b"macos-recovery".hex()
    recovery_value = b"durable-restart".hex()

    if not daemon.start():
        recorder.record("put-get-erase", "FAIL", log=log.name)
        recorder.record("restart-recovery", "NOT_RUN", detail="the daemon never became ready")
        daemon.stop()
        return

    def client(*arguments: str) -> bool:
        command, environment = client_command(root, daemon.port, *arguments)
        return _run(command, log=log, environment=environment, timeout=300)

    def read_back(expected_key: str, expected_value: str) -> bool:
        command, environment = client_command(root, daemon.port, "get", "--key-hex", expected_key)
        got = _capture(command, log=log, environment=environment, timeout=300)
        _note(log, f"exact GET: expected {expected_value}, got {got}")
        return got == expected_value

    exercised = (
        client("put", "--key-hex", key, "--value-hex", value)
        and read_back(key, value)
        and client("erase", "--key-hex", key)
        and client("expect-not-found", "--key-hex", key)
    )
    recorder.record("put-get-erase", "PASS" if exercised else "FAIL", log=log.name)

    if not exercised:
        recorder.record("restart-recovery", "NOT_RUN", detail="the protocol exercise failed first")
        daemon.stop()
        return

    stored = client("put", "--key-hex", recovery_key, "--value-hex", recovery_value)
    stopped = daemon.stop()
    restarted = daemon.start() if stored and stopped else False
    recovered = restarted and read_back(recovery_key, recovery_value)
    daemon.stop()
    recorder.record("restart-recovery", "PASS" if recovered else "FAIL", log=log.name)


def build_external_consumer(
    recorder: Recorder, *, root: Path, prefix: Path, work: Path, backend: str
) -> bool:
    """Build the consumer fixture against the installed prefix, outside the source tree."""
    log = recorder.directory / f"{backend}-external-consumer.log"
    source = work / "consumer-source"
    build = work / "consumer-build"
    if source.exists():
        shutil.rmtree(source)
    shutil.copytree(root / "tests/consumer", source)
    environment = dict(os.environ)
    environment.pop("GITHUB_WORKSPACE", None)
    ok = _run(
        [
            "cmake",
            "-S",
            str(source),
            "-B",
            str(build),
            f"-DCMAKE_PREFIX_PATH={prefix}",
            "-DCMAKE_BUILD_TYPE=Release",
        ],
        log=log,
        environment=environment,
    ) and _run(["cmake", "--build", str(build)], log=log, environment=environment)
    if ok:
        ok = _run(["ctest", "--test-dir", str(build), "--output-on-failure"], log=log)
    recorder.record("external-consumer", "PASS" if ok else "FAIL", log=log.name)
    return ok


def check_prefix_isolation(
    recorder: Recorder, *, backend: str, root: Path, paths: list[Path]
) -> bool:
    log = recorder.directory / f"{backend}-prefix-isolation.log"
    # Install trees expose ABI symlinks; otool must see the real Mach-O files.
    paths = [path.resolve() for path in paths]
    try:
        violations = inspect_links(backend, paths, root=root)
    except PrefixIsolationError as error:
        _note(log, f"prefix isolation refused: {error}")
        recorder.record("prefix-isolation", "FAIL", log=log.name)
        return False
    for path in paths:
        _note(log, f"inspected {path}")
    for violation in violations:
        _note(log, f"violation: {violation}")
    _note(log, f"foreign links: {len(violations)}")
    recorder.record("prefix-isolation", "FAIL" if violations else "PASS", log=log.name)
    return not violations


def _sudo(command: Sequence[str]) -> list[str]:
    escalation = os.environ.get("GLIFISTORE_PACKAGE_CI_SUDO", "sudo")
    return [*escalation.split(), *command] if escalation else list(command)


def _inventory(
    recorder: Recorder, log: Path, expected: Sequence[Path], *, satisfied: bool
) -> bool:
    for path in expected:
        present = path.exists()
        _note(log, f"{path}: {'present' if present else 'MISSING'}")
        satisfied = satisfied and present
    return satisfied


def run_macports_service_lifecycle(recorder: Recorder, *, root: Path, prefix: Path) -> None:
    """Start, health-check and stop the port through `port load` / `port unload`."""
    log = recorder.directory / "macports-service-lifecycle.log"
    sample = prefix / "etc/glifistore/glifistored.conf.sample"
    config = prefix / "etc/glifistore/glifistored.conf"
    if not sample.is_file():
        recorder.record(
            "service-lifecycle",
            "FAIL",
            log=log.name,
            detail=f"missing packaged sample configuration: {sample}",
        )
        return
    if not config.exists():
        shutil.copy2(sample, config)
        _note(log, f"installed operator configuration from {sample.name}")
    started = _run(_sudo(["port", "load", "glifistore"]), log=log)
    ready = started and wait_for_port(7379, time.monotonic() + DAEMON_READY_TIMEOUT)
    _note(log, f"port load reached 127.0.0.1:7379: {ready}")
    healthy = False
    if ready:
        command, environment = client_command(
            root,
            7379,
            "put",
            "--key-hex",
            b"macports-services".hex(),
            "--value-hex",
            b"lifecycle".hex(),
        )
        healthy = _run(command, log=log, environment=environment, timeout=300)
    stopped = _run(_sudo(["port", "unload", "glifistore"]), log=log)
    if ready:
        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
                if probe.connect_ex(("127.0.0.1", 7379)) != 0:
                    break
            time.sleep(0.25)
    recorder.record(
        "service-lifecycle",
        "PASS" if started and ready and healthy and stopped else "FAIL",
        log=log.name,
    )


def run_macports_lifecycle(
    recorder: Recorder,
    *,
    root: Path,
    port_directory: Path,
    work: Path,
    abi_version: str,
    context: dict[str, Any] | None = None,
) -> None:
    prefix = Path(shutil.which("port")).resolve().parent.parent
    logs = recorder.directory

    lint = logs / "macports-lint.log"
    linted = _run(["port", "lint", "--nitpick"], log=lint, cwd=port_directory)
    recorder.record("package-lint", "PASS" if linted else "FAIL", log=lint.name)

    build = logs / "macports-build.log"
    built = linted and _run(
        ["port", "-v", "destroot"], log=build, cwd=port_directory, timeout=BUILD_TIMEOUT
    )
    recorder.record("package-build", "PASS" if built else "FAIL", log=build.name)
    if not built:
        return

    install = logs / "macports-install.log"
    upgrade_log = logs / "macports-package-upgrade.log"
    daemon_binary = prefix / "bin/glifistored"
    installed = False
    try:
        exercise = context is not None and upgrade_exercise_requested(context["previous"])
    except N1PackageError as error:
        recorder.record("package-upgrade", "FAIL", detail=str(error))
        exercise = False

    if exercise and context is not None:
        try:
            n1_version, n1_source = resolve_n1_macos_source(context)
            n1_context = context_for_n1_version(context, n1_version)
            n1_rendered = render_to_file(
                "macports",
                n1_context,
                n1_source,
                profile="pr",
                directory=work / "n1-rendered",
                root=root,
            )
            n1_port = work / "n1-ports/databases/glifistore"
            if n1_port.exists():
                shutil.rmtree(n1_port)
            n1_port.mkdir(parents=True)
            shutil.copy2(n1_rendered, n1_port / n1_rendered.name)
            shutil.copytree(root / "packaging/macports/files", n1_port / "files")
            _note(upgrade_log, f"previous release {context['previous']['tag']} (version {n1_version})")
            _note(upgrade_log, f"{N1_PACKAGE_DIR_ENVIRONMENT} source={n1_source.basename}")
            _note(install, f"upgrade path: installing sealed N-1 {n1_version} before N")
            n1_installed = _run(
                _sudo(["port", "-v", "install"]),
                log=install,
                cwd=n1_port,
                timeout=BUILD_TIMEOUT,
            ) and _run(["port", "installed", "glifistore"], log=install)
            if not n1_installed:
                raise MacBackendError(f"sealed N-1 MacPorts package {n1_version} failed to install")
            if not seed_and_verify_upgrade(
                root=root, executable=daemon_binary, work=work, log=upgrade_log
            ):
                raise MacBackendError("could not seed durable data under the installed N-1 package")
            _note(upgrade_log, "upgrading to N Portfile")
            installed = _run(
                _sudo(["port", "-v", "install"]),
                log=install,
                cwd=port_directory,
                timeout=BUILD_TIMEOUT,
            ) and _run(["port", "installed", "glifistore"], log=install)
            if not installed:
                raise MacBackendError("upgrade from sealed N-1 to the built N Portfile failed")
            recorder.record("package-install", "PASS", log=install.name)
            if verify_upgrade_seed(
                root=root, executable=daemon_binary, work=work, log=upgrade_log
            ):
                _note(upgrade_log, "seeded value recovered byte-exact after upgrade to N")
                recorder.record("package-upgrade", "PASS", log=upgrade_log.name)
            else:
                raise MacBackendError(
                    f"value seeded under {context['previous']['tag']} was not recovered "
                    f"byte-exact after upgrading to {context['product_version']}"
                )
        except (MacBackendError, MacPackagingError, N1PackageError) as error:
            _note(upgrade_log, f"refused: {error}")
            if "package-upgrade" not in recorder.statuses:
                recorder.record(
                    "package-upgrade", "FAIL", log=upgrade_log.name, detail=str(error)
                )
            if "package-install" not in recorder.statuses:
                recorder.record("package-install", "FAIL", log=install.name, detail=str(error))
            return
    else:
        installed = _run(
            _sudo(["port", "-v", "install"]),
            log=install,
            cwd=port_directory,
            timeout=BUILD_TIMEOUT,
        ) and _run(["port", "installed", "glifistore"], log=install)
        recorder.record("package-install", "PASS" if installed else "FAIL", log=install.name)
    if not installed and "package-install" not in recorder.statuses:
        recorder.record("package-install", "FAIL", log=install.name)
    if recorder.statuses.get("package-install") != "PASS":
        return

    library = prefix / f"lib/libglifistore.{abi_version}.dylib"
    sample = prefix / "etc/glifistore/glifistored.conf.sample"
    data = prefix / "var/db/glifistore"
    inspection = logs / "macports-inspect.log"
    inspected = _inventory(
        recorder,
        inspection,
        (daemon_binary, library, sample, data),
        satisfied=_run(["port", "contents", "glifistore"], log=inspection),
    )
    recorder.record("package-inspect", "PASS" if inspected else "FAIL", log=inspection.name)

    check_prefix_isolation(
        recorder, backend="macports", root=root, paths=[daemon_binary, library]
    )
    build_external_consumer(recorder, root=root, prefix=prefix, work=work, backend="macports")
    run_macports_service_lifecycle(recorder, root=root, prefix=prefix)
    exercise_daemon(recorder, root=root, executable=daemon_binary, work=work, backend="macports")

    remove = logs / "macports-remove.log"
    probe = data / "RETENTION-PROBE"
    removed = _run(_sudo(["/usr/bin/touch", str(probe)]), log=remove)
    removed = removed and _run(_sudo(["port", "-v", "deactivate", "glifistore"]), log=remove)
    _note(remove, f"deactivated binary absent: {not daemon_binary.exists()}")
    removed = removed and not daemon_binary.exists()
    removed = removed and _run(_sudo(["port", "-v", "activate", "glifistore"]), log=remove)
    _note(remove, f"reactivated binary present: {daemon_binary.exists()}")
    removed = removed and daemon_binary.exists()
    removed = removed and _run(_sudo(["port", "-v", "uninstall", "glifistore"]), log=remove)
    removed = removed and _run(["port", "clean", "--all"], log=remove, cwd=port_directory)
    _note(remove, f"durable data retained after uninstall: {probe.exists()}")
    removed = removed and probe.exists()
    recorder.record("package-remove", "PASS" if removed else "FAIL", log=remove.name)


def run_homebrew_lifecycle(
    recorder: Recorder,
    *,
    root: Path,
    formula: Path,
    work: Path,
    abi_version: str,
    context: dict[str, Any] | None = None,
) -> None:
    logs = recorder.directory
    audit = logs / "homebrew-audit.log"
    audited = _run(["brew", "audit", "--strict", "--formula", str(formula)], log=audit)
    recorder.record("package-lint", "PASS" if audited else "FAIL", log=audit.name)
    if not audited:
        recorder.record("package-build", "FAIL", detail="brew audit refused the formula")
        return

    build = logs / "homebrew-build.log"
    install = logs / "homebrew-install.log"
    upgrade_log = logs / "homebrew-package-upgrade.log"
    prefix_output = subprocess.run(
        ["brew", "--prefix"], check=False, stdout=subprocess.PIPE, text=True, timeout=120
    )
    prefix = Path(prefix_output.stdout.strip() or "/opt/homebrew")
    keg = prefix / "opt/glifistore"
    daemon_binary = prefix / "bin/glifistored"
    library = keg / f"lib/libglifistore.{abi_version}.dylib"
    sample = prefix / "etc/glifistore/glifistored.conf.sample"
    data = prefix / "var/glifistore"

    try:
        exercise = context is not None and upgrade_exercise_requested(context["previous"])
    except N1PackageError as error:
        recorder.record("package-upgrade", "FAIL", detail=str(error))
        exercise = False

    installed = False
    if exercise and context is not None:
        try:
            n1_version, n1_source = resolve_n1_macos_source(context)
            n1_context = context_for_n1_version(context, n1_version)
            n1_formula = render_to_file(
                "homebrew",
                n1_context,
                n1_source,
                profile="pr",
                directory=work / "n1-rendered",
                root=root,
            )
            _note(upgrade_log, f"previous release {context['previous']['tag']} (version {n1_version})")
            _note(upgrade_log, f"{N1_PACKAGE_DIR_ENVIRONMENT} source={n1_source.basename}")
            _note(install, f"upgrade path: installing sealed N-1 {n1_version} before N")
            n1_installed = _run(
                [
                    "brew",
                    "install",
                    "--build-from-source",
                    "--verbose",
                    "--formula",
                    str(n1_formula),
                ],
                log=build,
                timeout=BUILD_TIMEOUT,
            )
            if not n1_installed:
                raise MacBackendError(f"sealed N-1 Homebrew formula {n1_version} failed to install")
            if not seed_and_verify_upgrade(
                root=root, executable=daemon_binary, work=work, log=upgrade_log
            ):
                raise MacBackendError("could not seed durable data under the installed N-1 package")
            _note(upgrade_log, "upgrading to N formula")
            n_built = _run(
                [
                    "brew",
                    "install",
                    "--build-from-source",
                    "--verbose",
                    "--formula",
                    str(formula),
                ],
                log=build,
                timeout=BUILD_TIMEOUT,
            )
            if not n_built:
                raise MacBackendError("upgrade from sealed N-1 to the built N formula failed")
            recorder.record("package-build", "PASS", log=build.name)
            installed = _inventory(
                recorder,
                install,
                (daemon_binary,),
                satisfied=_run(["brew", "list", "--verbose", "glifistore"], log=install),
            )
            if not installed:
                raise MacBackendError("upgrade from sealed N-1 to the built N formula failed")
            recorder.record("package-install", "PASS", log=install.name)
            if verify_upgrade_seed(
                root=root, executable=daemon_binary, work=work, log=upgrade_log
            ):
                _note(upgrade_log, "seeded value recovered byte-exact after upgrade to N")
                recorder.record("package-upgrade", "PASS", log=upgrade_log.name)
            else:
                raise MacBackendError(
                    f"value seeded under {context['previous']['tag']} was not recovered "
                    f"byte-exact after upgrading to {context['product_version']}"
                )
        except (MacBackendError, MacPackagingError, N1PackageError) as error:
            _note(upgrade_log, f"refused: {error}")
            if "package-build" not in recorder.statuses:
                recorder.record("package-build", "FAIL", log=build.name, detail=str(error))
            if "package-upgrade" not in recorder.statuses:
                recorder.record(
                    "package-upgrade", "FAIL", log=upgrade_log.name, detail=str(error)
                )
            if "package-install" not in recorder.statuses:
                recorder.record("package-install", "FAIL", log=install.name, detail=str(error))
            return
    else:
        built = _run(
            ["brew", "install", "--build-from-source", "--verbose", "--formula", str(formula)],
            log=build,
            timeout=BUILD_TIMEOUT,
        )
        recorder.record("package-build", "PASS" if built else "FAIL", log=build.name)
        if not built:
            return
        installed = _inventory(
            recorder,
            install,
            (daemon_binary,),
            satisfied=_run(["brew", "list", "--verbose", "glifistore"], log=install),
        )
        recorder.record("package-install", "PASS" if installed else "FAIL", log=install.name)

    if recorder.statuses.get("package-install") != "PASS":
        return

    inspection = logs / "homebrew-inspect.log"
    inspected = _inventory(
        recorder,
        inspection,
        (library, sample, data),
        satisfied=_run(["brew", "info", "glifistore"], log=inspection),
    )
    recorder.record("package-inspect", "PASS" if inspected else "FAIL", log=inspection.name)

    check_prefix_isolation(
        recorder, backend="homebrew", root=root, paths=[daemon_binary, library]
    )
    if build_external_consumer(recorder, root=root, prefix=keg, work=work, backend="homebrew"):
        _run(["brew", "test", "glifistore"], log=logs / "homebrew-external-consumer.log")
    run_homebrew_service_lifecycle(recorder, root=root, prefix=prefix)
    exercise_daemon(recorder, root=root, executable=daemon_binary, work=work, backend="homebrew")

    remove = logs / "homebrew-remove.log"
    probe = data / "RETENTION-PROBE"
    removed = _run(["/usr/bin/touch", str(probe)], log=remove)
    removed = removed and _run(["brew", "uninstall", "glifistore"], log=remove)
    _note(remove, f"uninstalled binary absent: {not daemon_binary.exists()}")
    removed = removed and not daemon_binary.exists()
    removed = removed and _run(["brew", "cleanup", "--prune=all"], log=remove)
    _note(remove, f"durable data retained after uninstall: {probe.exists()}")
    removed = removed and probe.exists()
    recorder.record("package-remove", "PASS" if removed else "FAIL", log=remove.name)

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


def execute(
    *,
    backend: str,
    profile: str,
    stage: str,
    root: Path,
    release_context: Path,
    directory: Path,
    work: Path,
    matrix_path: Path = MATRIX_PATH,
) -> tuple[str, Path]:
    if backend not in MACOS_BACKENDS:
        raise MacBackendError(f"unsupported macOS packaging backend: {backend}")
    if profile not in PROFILES:
        raise MacBackendError(f"unsupported CI profile: {profile}")
    if stage not in STAGES:
        raise MacBackendError(f"unsupported lifecycle stage: {stage}")
    directory.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)

    context = load_release_context(release_context)
    matrix = load_matrix(matrix_path)
    declared = list(matrix_backend(matrix, backend)["checks"])
    recorder = Recorder(directory=directory)

    package = context["package_versions"][backend]
    structural = recorder.write_log(
        "structural-metadata.log",
        "\n".join(
            (
                f"backend={backend} profile={profile} stage={stage}",
                f"release_context={release_context}",
                f"product_version={context['product_version']} "
                f"package_version={package['package_version']} "
                f"package_revision={context['package_revision']}",
                f"commit={context['git']['commit']} tree_clean={context['git']['tree_clean']}",
                f"template={TEMPLATE_PATHS[backend]}",
                "structural metadata resolved from the release context, not a hard-coded version",
            )
        ),
    )
    recorder.record("structural-metadata", "PASS", log=structural)
    recorder.limitations.extend(package["limitations"])

    render_log = directory / f"{backend}-package-metadata-render.log"
    rendered: Path | None = None
    try:
        source = resolve_profile_source(
            backend=backend, profile=profile, context=context, root=root, work=work
        )
        rendered = render_to_file(
            backend,
            context,
            source,
            profile=profile,
            directory=directory / "rendered",
            root=root,
        )
        _note(render_log, f"source url: {source.url}")
        _note(render_log, f"source sha256: {source.sha256}")
        _note(render_log, f"source size: {source.size}")
        _note(render_log, f"rendered: {rendered}")
        recorder.record("package-metadata-render", "PASS", log=render_log.name)
    except (MacBackendError, MacPackagingError) as error:
        _note(render_log, f"refused: {error}")
        recorder.record(
            "package-metadata-render", "FAIL", log=render_log.name, detail=str(error)
        )

    # Service integrations are exercised only when the native lifecycle runs.
    # Structural/PR hosts keep an honest OPEN_GATE until that retained run exists.

    distribution = "the MacPorts ports tree" if backend == "macports" else "an official tap"
    upstream = recorder.write_log(
        "upstream-ports-acceptance.log",
        "\n".join(
            (
                f"backend={backend}",
                f"in_repo_packaging={TEMPLATE_PATHS[backend]}",
                f"upstream_acceptance=not granted by {distribution}",
                f"authority=packaging/{backend}/README.md",
            )
        ),
    )
    recorder.record(
        "upstream-ports-acceptance",
        "OPEN_GATE",
        log=upstream,
        detail=f"{distribution} has not accepted the GlifiStore packaging",
    )
    recorder.residuals.append(
        f"{backend}-upstream-acceptance=In-repo packaging is the GlifiStore pipeline, not an "
        f"accepted ports tree or official tap|upstream review by the {backend} project"
    )

    if context["previous"]["available"]:
        tag = context["previous"]["tag"]
        try:
            n1_dir = supplied_n1_package_dir()
        except N1PackageError as error:
            recorder.record("package-upgrade", "FAIL", detail=str(error))
            n1_dir = None
            defer_upgrade = False
        else:
            # When sealed N-1 bytes are supplied, the native lifecycle records PASS/FAIL.
            defer_upgrade = n1_dir is not None
            if n1_dir is None:
                recorder.record(
                    "package-upgrade",
                    "NOT_RUN",
                    detail=(
                        f"previous release {tag} is selected; sealed N-1 package artifacts were not "
                        f"supplied via {N1_PACKAGE_DIR_ENVIRONMENT}, so upgrade continuity was not "
                        "exercised. This run never rebuilds N-1 from HEAD."
                    ),
                )
    else:
        defer_upgrade = False
        try:
            if supplied_n1_package_dir() is not None:
                recorder.record(
                    "package-upgrade",
                    "FAIL",
                    detail=(
                        f"{N1_PACKAGE_DIR_ENVIRONMENT} is set but the release context has no "
                        f"SemVer predecessor ({context['previous']['reason']})"
                    ),
                )
            else:
                recorder.record(
                    "package-upgrade",
                    "NOT_APPLICABLE_INITIAL_BASELINE",
                    detail=context["previous"]["reason"],
                )
        except N1PackageError as error:
            recorder.record("package-upgrade", "FAIL", detail=str(error))

    native, fallback, reason = native_availability(backend, stage=stage)
    abi_version = context["abi"]["version"]
    if native and rendered is not None:
        if backend == "macports":
            # port lint checks the ports-tree layout, so the rendered Portfile is
            # staged as <category>/<name>/Portfile rather than linted in place.
            port_directory = work / "ports/databases/glifistore"
            if port_directory.exists():
                shutil.rmtree(port_directory)
            port_directory.mkdir(parents=True)
            shutil.copy2(rendered, port_directory / rendered.name)
            shutil.copytree(root / "packaging/macports/files", port_directory / "files")
            run_macports_lifecycle(
                recorder,
                root=root,
                port_directory=port_directory,
                work=work,
                abi_version=abi_version,
                context=context,
            )
        else:
            run_homebrew_lifecycle(
                recorder,
                root=root,
                formula=rendered,
                work=work,
                abi_version=abi_version,
                context=context,
            )
    elif native:
        fallback, reason = "NOT_RUN", "no rendered metadata to build from"

    if defer_upgrade and "package-upgrade" not in recorder.statuses:
        tag = context["previous"]["tag"]
        detail_dir = ""
        try:
            detail_dir = str(supplied_n1_package_dir() or "")
        except N1PackageError:
            detail_dir = ""
        recorder.record(
            "package-upgrade",
            "NOT_RUN",
            detail=(
                f"previous release {tag} is selected and {N1_PACKAGE_DIR_ENVIRONMENT}="
                f"{detail_dir} was set, but the {backend} install→seed→upgrade→verify walk "
                "did not run because the native install lifecycle was not reached in this "
                "process. This run never rebuilds N-1 from HEAD."
            ),
        )

    if "service-lifecycle" not in recorder.statuses:
        recorder.record("service-lifecycle", "OPEN_GATE", detail=SERVICE_GATE[backend])
        recorder.residuals.append(
            f"{backend}-service-integration={SERVICE_GATE[backend]}"
            "|a retained native start/health/stop run under GLIFISTORE_PACKAGE_CI_NATIVE=1"
        )

    recorder.pending(declared, fallback, reason or "not reached in this run")

    if profile == "release":
        recorder.limitations.append(
            "The release profile renders from the sealed source archive only; no sealed "
            f"{backend} artifact is produced or admitted yet."
        )
    recorder.limitations.append(
        f"In-repo {backend} packaging is the GlifiStore packaging pipeline; it is not a claim "
        "of upstream ports-tree or official-tap acceptance."
    )
    if not native:
        recorder.limitations.append(f"The native {backend} lifecycle did not run: {reason}.")

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
    result.add_argument("--backend", choices=MACOS_BACKENDS, required=True)
    result.add_argument("--profile", choices=PROFILES, required=True)
    result.add_argument("--stage", choices=STAGES, default="full")
    result.add_argument("--root", type=Path, default=REPO_ROOT)
    result.add_argument("--release-context", type=Path, required=True)
    result.add_argument("--output-dir", type=Path, required=True)
    result.add_argument("--work-dir", type=Path)
    result.add_argument("--matrix", type=Path, default=MATRIX_PATH)
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
            matrix_path=arguments.matrix,
        )
    except (
        MacBackendError,
        MacPackagingError,
        PackageEvidenceError,
        PackageFrameworkError,
        PackageMatrixError,
        PrefixIsolationError,
        ReleaseContextError,
        OSError,
    ) as error:
        print(f"{arguments.backend} packaging FAILED: {error}", file=sys.stderr)
        return 1
    print(f"PACKAGE-CI {arguments.backend} {arguments.profile} {arguments.stage} {result} {evidence}")
    return 0 if result != "FAIL" else 1


if __name__ == "__main__":
    raise SystemExit(main())
