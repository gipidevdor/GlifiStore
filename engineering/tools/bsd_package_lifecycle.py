#!/usr/bin/env python3
"""Turn observed BSD packaging facts into honest package evidence.

`scripts/lib/package-backend-bsd.sh` probes the host, the reference port, the
upstream service-account marker and the sealed candidate, then optionally runs
`scripts/test-freebsd-package-lifecycle.sh` or its OpenBSD analogue. This module
owns the only mapping from those observations to package-framework check
statuses, the lifecycle state, the result and the emit arguments, so that:

* structural, native-build, package, service and upstream-acceptance rows stay
  separate and a structural pass never implies any of the others;
* `PORTS_ACCOUNT_REGISTERED` is read, never created, and an absent marker keeps
  the native lifecycle BLOCKED;
* a native log without its own PASSED marker is a FAIL, never an inferred pass;
* upgrade from N-1 stays NOT_APPLICABLE_INITIAL_BASELINE or NOT_RUN until a
  sealed prior release package exists; FreeBSD/OpenBSD native producers run
  install→seed→upgrade→verify when GLIFISTORE_N1_PACKAGE_DIR supplies those
  bytes (Linux deb/rpm do the same);
* the result stays OPEN_GATE while upstream acceptance is unproven, however far
  the native lifecycle got.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

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
    N1_PACKAGE_DIR_ENVIRONMENT,
    N1PackageError,
    supplied_n1_package_dir,
)
from engineering.tools.package_framework import (
    MATRIX_PATH,
    PROFILES,
    STAGES,
    PackageFrameworkError,
    write_json,
)


BACKENDS = ("freebsd", "openbsd")
HOSTS = {"freebsd": "FreeBSD", "openbsd": "OpenBSD"}
MARKER_PREFIXES = {"freebsd": "FREEBSD-PACKAGE", "openbsd": "OPENBSD-PACKAGE"}
LIFECYCLE_SCRIPTS = {
    "freebsd": "scripts/test-freebsd-package-lifecycle.sh",
    "openbsd": "scripts/test-openbsd-package-lifecycle.sh",
}
PACKAGE_PATTERNS = {
    "freebsd": "glifistore-*-freebsd*.pkg",
    "openbsd": "glifistore-*-openbsd*.tgz",
}
PORTS_MAKEFILES = {
    "freebsd": "Mk/bsd.port.mk",
    "openbsd": "infrastructure/mk/bsd.port.mk",
}

# Framework check -> the native lifecycle steps that must all have retained their
# own PASSED marker, in the order the native script executes them.
NATIVE_STEPS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("package-build", ("package-build",)),
    ("package-install", ("package-install",)),
    ("package-inspect", ("file-inventory",)),
    ("external-consumer", ("external-consumer",)),
    ("service-lifecycle", ("service-start", "graceful-shutdown")),
    ("put-get-erase", ("put-get-erase",)),
    ("restart-recovery", ("restart-recovery",)),
    ("config-preservation", ("config-preservation",)),
    ("package-remove", ("uninstall",)),
)

STRUCTURAL_LOGS = {
    "structural-metadata": "structural-metadata.log",
    "reference-port-structure": "reference-port-structure.log",
    "ports-account-registration": "ports-account-registration.log",
    "upstream-ports-acceptance": "upstream-ports-acceptance.log",
    "sealed-source-admission": "sealed-source-admission.log",
}
# Every blocker the preflight found is retained once here; the blocked rows point at
# it instead of repeating the reason in each detail.
PREREQUISITE_LOG = "native-prerequisites.log"

NATIVE_RESULTS = ("passed", "failed", "skipped")
PORT_STRUCTURE_RESULTS = ("PASS", "FAIL")
MARKER_STATES = ("present", "absent")
SEALED_SOURCE_STATES = ("verified", "unverified", "absent")
PRESENCE_STATES = ("present", "absent")
BOOLEAN_STATES = ("yes", "no")


class BsdLifecycleError(RuntimeError):
    pass


def _log(directory: Path, name: str) -> str | None:
    """Retained, non-empty log name, or None; an unretained log never backs a pass."""
    path = directory / name
    if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
        return None
    return name


def _step_log(backend: str, step: str) -> str:
    return f"{backend}-{step}.log"


def _step_passed(directory: Path, backend: str, step: str) -> bool:
    name = _log(directory, _step_log(backend, step))
    if name is None:
        return False
    marker = f"{MARKER_PREFIXES[backend]} {step} PASSED"
    return marker in (directory / name).read_text(encoding="utf-8", errors="replace")


def preflight(
    *,
    backend: str,
    stage: str,
    host: str,
    ports_account: str,
    sealed_source: str,
    ports_tree: str,
    ports_root: str,
    privileged: str,
    lifecycle_script: str,
) -> tuple[str, str]:
    """Decide whether the native lifecycle may run, and state every blocker."""
    if backend not in BACKENDS:
        raise BsdLifecycleError(f"unsupported BSD backend: {backend}")
    expected = HOSTS[backend]
    reasons: list[str] = []
    if stage != "full":
        reasons.append(
            f"the native {expected} lifecycle is monolithic and only runs in the full stage, "
            f"not in stage {stage}"
        )
    if host != expected:
        reasons.append(f"a native {expected} host is required and this runner is {host}")
    if lifecycle_script != "present":
        reasons.append(f"{LIFECYCLE_SCRIPTS[backend]} is not available in this tree")
    if ports_account != "present":
        reasons.append(
            f"packaging/{backend}/PORTS_ACCOUNT_REGISTERED is absent, so the upstream service "
            "account is unallocated and this run never creates the marker"
        )
    if sealed_source != "verified":
        reasons.append(
            "no sealed candidate source archive was admitted"
            if sealed_source == "absent"
            else "the sealed candidate source archive failed admission"
        )
    if ports_tree != "present":
        reasons.append(f"a complete native ports tree is required at {ports_root}")
    if privileged != "yes":
        reasons.append("installing the package and driving the service requires root")
    if reasons:
        return "BLOCKED", "; ".join(reasons)
    return "RUN", ""


def _blocked_detail(display: str, prerequisites: str | None) -> str:
    detail = f"the native {display} package and service lifecycle did not run"
    if prerequisites is None:
        return f"{detail}; its blockers were not retained"
    return f"{detail}; every blocker is recorded in {prerequisites}"


def _native_statuses(
    *, backend: str, directory: Path, native_lifecycle: str, native_reason: str
) -> tuple[dict[str, str], dict[str, str], dict[str, str]]:
    statuses: dict[str, str] = {}
    details: dict[str, str] = {}
    references: dict[str, str] = {}
    display = HOSTS[backend]

    if native_lifecycle == "skipped":
        prerequisites = _log(directory, PREREQUISITE_LOG)
        for check, _ in NATIVE_STEPS:
            statuses[check] = "BLOCKED"
            details[check] = _blocked_detail(display, prerequisites)
            if prerequisites is not None:
                references[check] = prerequisites
        return statuses, details, references

    run_log = _log(directory, f"{backend}-native-lifecycle.log")
    aborted = False
    for check, steps in NATIVE_STEPS:
        if all(_step_passed(directory, backend, step) for step in steps):
            statuses[check] = "PASS"
            references[check] = _step_log(backend, steps[0])
            if len(steps) > 1:
                details[check] = "proven by " + ", ".join(
                    _step_log(backend, step) for step in steps
                )
            continue
        missing = [step for step in steps if not _step_passed(directory, backend, step)]
        if aborted:
            statuses[check] = "NOT_RUN"
            details[check] = (
                f"the native {display} lifecycle aborted before this check retained "
                f"{', '.join(_step_log(backend, step) for step in missing)}"
            )
            continue
        aborted = True
        statuses[check] = "FAIL"
        if native_lifecycle == "passed":
            details[check] = (
                f"the native {display} lifecycle reported success without retaining "
                f"{', '.join(_step_log(backend, step) for step in missing)}; a pass is never inferred"
            )
        else:
            details[check] = (
                f"the native {display} lifecycle did not retain "
                f"{', '.join(_step_log(backend, step) for step in missing)}"
                + (f"; see {run_log}" if run_log else "")
            )
    return statuses, details, references


def lifecycle_state(statuses: dict[str, str]) -> str:
    """The deepest state every underlying category actually proved."""

    def passed(*identifiers: str) -> bool:
        return all(statuses.get(identifier) == "PASS" for identifier in identifiers)

    if not passed("structural-metadata", "reference-port-structure"):
        return "NONE"
    if not passed("sealed-source-admission", "package-build"):
        return "STRUCTURAL"
    if not passed("package-install", "package-inspect"):
        return "BUILT"
    if not passed("service-lifecycle", "put-get-erase", "restart-recovery"):
        return "INSTALLED"
    # LIFECYCLE_VERIFIED additionally owes an external consumer against the
    # installed package prefix, which the native scripts do not build yet.
    if not passed("config-preservation", "package-remove", "external-consumer"):
        return "FUNCTIONALLY_VERIFIED"
    if not passed("package-upgrade"):
        return "LIFECYCLE_VERIFIED"
    return "UPGRADE_VERIFIED"


def decide(
    *,
    backend: str,
    profile: str,
    stage: str,
    directory: Path,
    context: dict[str, Any],
    matrix: dict[str, Any],
    port_structure: str,
    ports_account: str,
    sealed_source: str,
    native_lifecycle: str,
    native_reason: str,
) -> dict[str, Any]:
    if backend not in BACKENDS:
        raise BsdLifecycleError(f"unsupported BSD backend: {backend}")
    if profile not in PROFILES:
        raise BsdLifecycleError(f"unsupported CI profile: {profile}")
    if stage not in STAGES:
        raise BsdLifecycleError(f"unsupported lifecycle stage: {stage}")
    if port_structure not in PORT_STRUCTURE_RESULTS:
        raise BsdLifecycleError(f"unsupported reference-port result: {port_structure}")
    if ports_account not in MARKER_STATES:
        raise BsdLifecycleError(f"unsupported ports-account state: {ports_account}")
    if sealed_source not in SEALED_SOURCE_STATES:
        raise BsdLifecycleError(f"unsupported sealed-source state: {sealed_source}")
    if native_lifecycle not in NATIVE_RESULTS:
        raise BsdLifecycleError(f"unsupported native lifecycle result: {native_lifecycle}")
    if native_lifecycle == "skipped" and not native_reason.strip():
        raise BsdLifecycleError("a skipped native lifecycle must state why it was skipped")

    display = HOSTS[backend]
    statuses: dict[str, str] = {}
    details: dict[str, str] = {}
    references: dict[str, str] = {}
    for check, name in STRUCTURAL_LOGS.items():
        retained = _log(directory, name)
        if retained is not None:
            references[check] = retained

    statuses["structural-metadata"] = (
        "PASS" if "structural-metadata" in references else "FAIL"
    )
    if statuses["structural-metadata"] == "FAIL":
        details["structural-metadata"] = "the structural metadata log was not retained"

    statuses["reference-port-structure"] = port_structure
    if port_structure == "FAIL":
        details["reference-port-structure"] = (
            "validate_bsd_packaging.py refused the in-repo reference port"
        )

    if ports_account == "present":
        statuses["ports-account-registration"] = "PASS"
        details["ports-account-registration"] = (
            f"packaging/{backend}/PORTS_ACCOUNT_REGISTERED records a maintainer-attested upstream "
            "allocation; this run only reads it"
        )
    else:
        statuses["ports-account-registration"] = "OPEN_GATE"
        details["ports-account-registration"] = (
            f"packaging/{backend}/PORTS_ACCOUNT_REGISTERED is absent: the upstream {display} "
            "service-account allocation does not exist and is never synthesised here"
        )

    # Reading a marker, or shipping a reference port, is not upstream acceptance.
    statuses["upstream-ports-acceptance"] = "OPEN_GATE"
    details["upstream-ports-acceptance"] = (
        f"no upstream {display} ports tree has accepted this packaging; an in-repo reference port "
        "is the project pipeline only (docs/distribution/bsd-packaging.md)"
    )

    if sealed_source == "verified":
        statuses["sealed-source-admission"] = "PASS"
    elif sealed_source == "unverified":
        statuses["sealed-source-admission"] = "FAIL"
        details["sealed-source-admission"] = (
            "the candidate directory did not admit its seal digest and sealed source archive"
        )
    elif profile == "release":
        statuses["sealed-source-admission"] = "BLOCKED"
        details["sealed-source-admission"] = (
            "the release profile requires the sealed candidate archive and none was provided"
        )
    else:
        statuses["sealed-source-admission"] = "NOT_RUN"
        details["sealed-source-admission"] = (
            "no candidate directory was provided, so no sealed source archive could be admitted"
        )

    native_statuses, native_details, native_references = _native_statuses(
        backend=backend,
        directory=directory,
        native_lifecycle=native_lifecycle,
        native_reason=native_reason,
    )
    statuses.update(native_statuses)
    details.update(native_details)
    references.update(native_references)

    previous = context["previous"]
    if previous["available"]:
        try:
            n1_dir = supplied_n1_package_dir()
        except N1PackageError as error:
            statuses["package-upgrade"] = "FAIL"
            details["package-upgrade"] = str(error)
        else:
            upgrade_log = _log(directory, _step_log(backend, "package-upgrade"))
            if upgrade_log is not None and _step_passed(directory, backend, "package-upgrade"):
                statuses["package-upgrade"] = "PASS"
                references["package-upgrade"] = upgrade_log
            elif n1_dir is not None and upgrade_log is not None:
                statuses["package-upgrade"] = "FAIL"
                references["package-upgrade"] = upgrade_log
                details["package-upgrade"] = (
                    f"previous release {previous['tag']} is selected and "
                    f"{N1_PACKAGE_DIR_ENVIRONMENT}={n1_dir} was set, but the {display} "
                    "install→seed→upgrade→verify walk did not retain its PASSED marker. "
                    "This run never rebuilds N-1 from HEAD."
                )
            elif n1_dir is not None:
                statuses["package-upgrade"] = "NOT_RUN"
                details["package-upgrade"] = (
                    f"previous release {previous['tag']} is selected and "
                    f"{N1_PACKAGE_DIR_ENVIRONMENT}={n1_dir} was set, but the {display} "
                    "install→seed→upgrade→verify walk did not run because the native "
                    "package lifecycle was not reached in this process. "
                    "This run never rebuilds N-1 from HEAD."
                )
            else:
                statuses["package-upgrade"] = "NOT_RUN"
                details["package-upgrade"] = (
                    f"previous release {previous['tag']} is selected; sealed N-1 {display} package "
                    "artifacts were not supplied via GLIFISTORE_N1_PACKAGE_DIR, so upgrade "
                    "continuity was not exercised. This run never rebuilds N-1 from HEAD."
                )
    else:
        statuses["package-upgrade"] = "NOT_APPLICABLE_INITIAL_BASELINE"
        details["package-upgrade"] = (
            f"no annotated release precedes {context['product_version']}, so there is no sealed "
            "N-1 package to upgrade from"
        )

    declared = matrix_backend(matrix, backend)["checks"]
    unclassified = sorted(set(declared) - set(statuses))
    if unclassified:
        raise BsdLifecycleError(
            f"the {backend} backend module does not classify declared checks: {unclassified}"
        )
    foreign = sorted(set(statuses) - set(declared))
    if foreign:
        raise BsdLifecycleError(
            f"the {backend} backend module classified checks the matrix does not declare: {foreign}"
        )

    result = "FAIL" if ("FAIL" in statuses.values() or native_lifecycle == "failed") else "OPEN_GATE"
    state = lifecycle_state(statuses)

    limitations = [
        f"An in-repo {display} reference port is the project packaging pipeline, not an upstream "
        "ports-tree acceptance (docs/distribution/bsd-packaging.md).",
        "PORTS_ACCOUNT_REGISTERED is a real upstream UID/GID allocation and is never synthesised "
        "by this run.",
    ]
    if backend == "openbsd":
        limitations.append(
            "LibreSSL from OpenBSD base remains the only supported TLS backend; no TLS "
            "implementation is bundled."
        )
    if native_lifecycle == "skipped":
        limitations.append(f"No native {display} package or service lifecycle ran here: {native_reason}.")
    elif native_lifecycle == "passed":
        limitations.append(
            f"The native {display} lifecycle covers this runner's package and service behaviour "
            "only; it is not power-loss durability certification."
        )
    else:
        limitations.append(
            f"The native {display} lifecycle exited non-zero; see {backend}-native-lifecycle.log."
        )
    if profile == "release" and sealed_source != "verified":
        limitations.append(
            "The release profile requires the sealed candidate archive; this run admitted none."
        )

    residuals = [
        f"upstream-ports-acceptance=No upstream {display} ports tree has accepted the GlifiStore "
        "packaging|upstream ports review",
    ]
    if statuses.get("external-consumer") != "PASS":
        residuals.append(
            "external-consumer-installed-prefix=The native package lifecycle has not proven an "
            f"external consumer against the installed {display} package prefix|a retained "
            f"{backend}-external-consumer.log with PASSED"
        )
    if ports_account != "present":
        residuals.append(
            f"ports-account-registration=The upstream {display} service-account UID/GID "
            "allocation does not exist|upstream ports account allocation"
        )
    if native_lifecycle != "passed":
        residuals.append(
            f"native-package-lifecycle=No retained native {display} package and service lifecycle "
            f"for this commit|a native {display} runner with the sealed candidate"
        )
    if statuses["package-upgrade"] == "NOT_APPLICABLE_INITIAL_BASELINE":
        residuals.append(
            "package-upgrade-n1=No prior annotated release exists, so N-1 upgrade continuity is "
            "an initial baseline|the first sealed release"
        )
    elif statuses["package-upgrade"] == "PASS":
        pass
    elif statuses["package-upgrade"] == "FAIL":
        residuals.append(
            f"package-upgrade-n1=Sealed N-1 {display} package selection or continuity failed|"
            "valid sealed N-1 packages and a successful upgrade walk"
        )
    elif "was set" in details.get("package-upgrade", ""):
        residuals.append(
            f"package-upgrade-n1=Sealed N-1 {display} packages were supplied but the "
            f"{display} install→seed→upgrade→verify walk did not retain a PASS|"
            f"a retained {backend}-package-upgrade.log with PASSED"
        )
    else:
        residuals.append(
            f"package-upgrade-n1=Previous release is selected but sealed N-1 {display} packages "
            "were not supplied via GLIFISTORE_N1_PACKAGE_DIR|sealed N-1 package artifacts"
        )

    plan = check_plan(
        matrix,
        backend,
        profile,
        default_status="NOT_RUN",
        statuses=statuses,
        evidence_refs=references,
        details=details,
    )
    return {
        "backend": backend,
        "profile": profile,
        "stage": stage,
        "result": result,
        "lifecycle_state": state,
        "statuses": statuses,
        "plan": plan,
        "limitations": limitations,
        "residuals": residuals,
        "subject": _subject(backend, directory),
    }


def _subject(backend: str, directory: Path) -> str | None:
    """The built package, when this run actually produced exactly one."""
    packages = sorted(
        path
        for path in directory.glob(PACKAGE_PATTERNS[backend])
        if path.is_file() and not path.is_symlink()
    )
    if not packages:
        return None
    if len(packages) > 1:
        raise BsdLifecycleError(
            f"expected at most one native {HOSTS[backend]} package, found {len(packages)}"
        )
    return str(packages[0])


def emit_arguments(decision: dict[str, Any]) -> list[str]:
    arguments = [
        "--result",
        decision["result"],
        "--lifecycle-state",
        decision["lifecycle_state"],
    ]
    for limitation in decision["limitations"]:
        arguments += ["--limitation", limitation]
    for residual in decision["residuals"]:
        arguments += ["--residual", residual]
    if decision["subject"] is not None:
        arguments += ["--subject", decision["subject"]]
    return arguments


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)

    gate = commands.add_parser("preflight")
    gate.add_argument("--backend", choices=BACKENDS, required=True)
    gate.add_argument("--stage", choices=STAGES, required=True)
    gate.add_argument("--host", required=True)
    gate.add_argument("--ports-account", choices=MARKER_STATES, required=True)
    gate.add_argument("--sealed-source", choices=SEALED_SOURCE_STATES, required=True)
    gate.add_argument("--ports-tree", choices=PRESENCE_STATES, required=True)
    gate.add_argument("--ports-root", required=True)
    gate.add_argument("--privileged", choices=BOOLEAN_STATES, required=True)
    gate.add_argument("--lifecycle-script", choices=PRESENCE_STATES, required=True)

    plan = commands.add_parser("plan")
    plan.add_argument("--backend", choices=BACKENDS, required=True)
    plan.add_argument("--profile", choices=PROFILES, required=True)
    plan.add_argument("--stage", choices=STAGES, required=True)
    plan.add_argument("--directory", type=Path, required=True)
    plan.add_argument("--release-context", type=Path, required=True)
    plan.add_argument("--matrix", type=Path, default=MATRIX_PATH)
    plan.add_argument("--port-structure", choices=PORT_STRUCTURE_RESULTS, required=True)
    plan.add_argument("--ports-account", choices=MARKER_STATES, required=True)
    plan.add_argument("--sealed-source", choices=SEALED_SOURCE_STATES, required=True)
    plan.add_argument("--native-lifecycle", choices=NATIVE_RESULTS, required=True)
    plan.add_argument("--native-reason", default="")
    plan.add_argument("--check-plan", type=Path, required=True)
    plan.add_argument("--emit-arguments", type=Path, required=True)
    plan.add_argument("--replace", action="store_true")
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        if arguments.command == "preflight":
            decision, reason = preflight(
                backend=arguments.backend,
                stage=arguments.stage,
                host=arguments.host,
                ports_account=arguments.ports_account,
                sealed_source=arguments.sealed_source,
                ports_tree=arguments.ports_tree,
                ports_root=arguments.ports_root,
                privileged=arguments.privileged,
                lifecycle_script=arguments.lifecycle_script,
            )
            print(f"{decision}\t{reason}")
            return 0

        decision = decide(
            backend=arguments.backend,
            profile=arguments.profile,
            stage=arguments.stage,
            directory=arguments.directory,
            context=load_release_context(arguments.release_context),
            matrix=load_matrix(arguments.matrix),
            port_structure=arguments.port_structure,
            ports_account=arguments.ports_account,
            sealed_source=arguments.sealed_source,
            native_lifecycle=arguments.native_lifecycle,
            native_reason=arguments.native_reason,
        )
        write_json(arguments.check_plan, decision["plan"], replace=arguments.replace)
        arguments.emit_arguments.parent.mkdir(parents=True, exist_ok=True)
        arguments.emit_arguments.write_text(
            "".join(f"{argument}\n" for argument in emit_arguments(decision)), encoding="utf-8"
        )
        print(f"{decision['result']}\t{decision['lifecycle_state']}")
    except (
        BsdLifecycleError,
        PackageFrameworkError,
        PackageMatrixError,
        ReleaseContextError,
        OSError,
    ) as error:
        print(f"BSD package lifecycle FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
