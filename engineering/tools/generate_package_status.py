#!/usr/bin/env python3
"""Generate the derived packaging status view from the package matrix.

docs/distribution/package-status.md is a rendering of
engineering/distribution/package-matrix.yaml plus the CI policy that
engineering/tools/package_ci_plan.py owns. Nothing is maintained by hand, so a
hand-written table can never claim a backend depth the matrix does not declare.

The rendered document describes what the matrix declares and what CI is allowed
to attempt. It is never evidence: the only packaging proofs are the retained
package-evidence documents produced by scripts/package-ci.sh and validated by
engineering/tools/validate_package_evidence.py.

Usage mirrors engineering/tools/validate_assurance.py: the default run fails
when the committed file is missing or stale, and --write-generated refreshes it.
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
    check_vocabulary,
    load_matrix,
    profile_backends,
)
from engineering.tools.package_ci_plan import (
    CONTAINER_BACKENDS,
    CONTAINER_PROFILES,
    DISPATCH_PROFILES,
    EVENT_PROFILES,
    NATIVE_BACKENDS,
    RETENTION_DAYS,
)
from engineering.tools.package_framework import MATRIX_PATH, PROFILES, PackageFrameworkError

REPO_ROOT = Path(__file__).resolve().parents[2]
STATUS_PATH = REPO_ROOT / "docs" / "distribution" / "package-status.md"
NONE = "—"


def _cell(value: Any) -> str:
    if value is None or value == "":
        return NONE
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, (list, tuple)):
        return ", ".join(f"`{item}`" for item in value) if value else NONE
    return str(value)


def _text(value: str) -> str:
    """Matrix prose as one Markdown line; angle brackets never become HTML tags."""
    return " ".join(value.split()).replace("<", "&lt;").replace(">", "&gt;")


def _table(header: list[str], rows: list[list[str]]) -> list[str]:
    lines = ["| " + " | ".join(header) + " |", "| " + " | ".join("---" for _ in header) + " |"]
    lines.extend("| " + " | ".join(row) + " |" for row in rows)
    lines.append("")
    return lines


def _events(profile: str) -> list[str]:
    triggers = sorted(event for event, value in EVENT_PROFILES.items() if value == profile)
    if profile in DISPATCH_PROFILES:
        triggers.append("workflow_dispatch")
    triggers.append("workflow_call")
    return triggers


def _depth(matrix: dict[str, Any], profile: str) -> str:
    available = set(profile_backends(matrix, profile))
    depths = ["structural"]
    if profile in CONTAINER_PROFILES:
        containers = sorted(available.intersection(CONTAINER_BACKENDS))
        if containers:
            depths.append(
                "digest-pinned container for " + ", ".join(f"`{name}`" for name in containers)
            )
    natives = sorted(available.intersection(NATIVE_BACKENDS))
    if natives:
        depths.append(
            "opt-in native (`--allow-native`) for " + ", ".join(f"`{name}`" for name in natives)
        )
    return "; ".join(depths)


def render(matrix: dict[str, Any]) -> str:
    backends = sorted(matrix["backends"], key=lambda entry: entry["id"])
    vocabulary = check_vocabulary(matrix)
    lines: list[str] = [
        "<!-- GENERATED FILE. Do not edit by hand.",
        "     Authority: engineering/distribution/package-matrix.yaml",
        "                engineering/tools/package_ci_plan.py (CI profile policy)",
        "     Regenerate: python3 engineering/tools/generate_package_status.py --write-generated",
        "-->",
        "",
        "# Package backend status",
        "",
        "> **Derived view.** The machine-readable authority is",
        "> [`engineering/distribution/package-matrix.yaml`](../../engineering/distribution/package-matrix.yaml).",
        "> A row below states what the matrix declares and how deep CI is allowed to go; it is never",
        "> evidence that a package was built, installed, started or accepted upstream. The only",
        "> packaging proofs are the retained `*-package-evidence.json` documents produced by",
        "> [`scripts/package-ci.sh`](../../scripts/package-ci.sh) and validated by",
        "> [`engineering/tools/validate_package_evidence.py`](../../engineering/tools/validate_package_evidence.py).",
        "> GlifiStore remains an **architectural prototype**; no packaging gate is closed.",
        "",
        "Operator guide: [package CI](package-ci.md) · Open residuals:",
        "[release residuals](wave5-l7-residuals.md).",
        "",
        "## Backends",
        "",
        "`status` is the repository-side maturity of the backend, `declared lifecycle state` is the",
        "furthest point of the chain the matrix claims for it, and `required for release` says whether",
        "[`release_bundle.validate_release_policy`](../../engineering/tools/release_bundle.py) demands",
        "its artifact. A backend is promoted by a gate and an ADR, never by editing this page.",
        "",
    ]
    lines.extend(
        _table(
            ["Backend", "Package kind", "Status", "Declared lifecycle state", "Required for release", "Wave", "Profiles"],
            [
                [
                    f"`{entry['id']}`",
                    f"`{entry['package_kind']}`",
                    f"`{entry['status']}`",
                    f"`{entry['lifecycle_state']}`",
                    _cell(entry["required_for_release"]),
                    _cell(entry["wave"]),
                    _cell(sorted(entry["profiles"])),
                ]
                for entry in backends
            ],
        )
    )

    lines.extend(["## Targets", "", "A container target is pinned by digest; a host target runs on the runner itself.", ""])
    target_rows: list[list[str]] = []
    for entry in backends:
        for target in sorted(entry["targets"], key=lambda value: value["id"]):
            container = target["container"]
            digest = target["container_digest"]
            image = NONE if container is None else f"`{container}`"
            pin = NONE if digest is None else f"`{digest[:19]}…`"
            target_rows.append(
                [
                    f"`{target['id']}`",
                    f"`{entry['id']}`",
                    f"`{target['platform']}`",
                    f"`{target['arch']}`",
                    f"`{target['runner']}`",
                    image,
                    pin,
                    _cell(sorted(target["profiles"])),
                ]
            )
    lines.extend(
        _table(
            ["Target", "Backend", "Platform", "Arch", "Runner", "Image", "Digest", "Profiles"],
            target_rows,
        )
    )

    lines.extend(
        [
            "## Required checks per profile",
            "",
            "A backend that does not run in a profile shows " + NONE + ". Every other check the backend",
            "declares still has to report an honest status; only the checks below may not be omitted.",
            "",
        ]
    )
    lines.extend(
        _table(
            ["Backend", *[f"`{profile}`" for profile in PROFILES]],
            [
                [
                    f"`{entry['id']}`",
                    *[
                        _cell(sorted(entry["required_checks"].get(profile, [])))
                        for profile in PROFILES
                    ],
                ]
                for entry in backends
            ],
        )
    )

    lines.extend(
        [
            "## Lifecycle check vocabulary",
            "",
            "Evidence may only use these ids. The category bounds how a passing row may ever be read:",
            "a `structural` pass is never a package, service or upstream-acceptance proof.",
            "",
        ]
    )
    lines.extend(
        _table(
            ["Check", "Stage", "Category", "What a pass means"],
            [
                [
                    f"`{identifier}`",
                    f"`{value['stage']}`",
                    NONE if value["category"] is None else f"`{value['category']}`",
                    _text(value["description"]),
                ]
                for identifier, value in sorted(vocabulary.items())
            ],
        )
    )

    lines.extend(
        [
            "## Lifecycle states and statuses",
            "",
            "The lifecycle chain a backend may climb, in order:",
            "",
            " → ".join(f"`{state}`" for state in matrix["lifecycle_states"]),
            "",
            "A check reports exactly one status. `NOT_RUN`, `BLOCKED` and `OPEN_GATE` are first-class",
            "outcomes and are never rounded up to `PASS`; [package-ci.md](package-ci.md) documents how",
            "to read each one.",
            "",
        ]
    )

    lines.extend(
        [
            "## CI profile policy",
            "",
            "Owned by [`engineering/tools/package_ci_plan.py`](../../engineering/tools/package_ci_plan.py):",
            "[`package-ci.yml`](../../.github/workflows/package-ci.yml) restates no profile, retention or",
            "depth of its own. The `release` profile is only reachable through a call from",
            "[`release.yml`](../../.github/workflows/release.yml) with sealed candidate bytes.",
            "",
        ]
    )
    lines.extend(
        _table(
            ["Profile", "Events", "Sealed artifacts required", "Evidence retention (days)", "Lifecycle depth available"],
            [
                [
                    f"`{profile}`",
                    _cell([event for event in _events(profile)]),
                    _cell(matrix["profiles"][profile]["requires_sealed_artifacts"]),
                    _cell(RETENTION_DAYS[profile]),
                    _depth(matrix, profile),
                ]
                for profile in PROFILES
            ],
        )
    )

    lines.extend(["## Backend limitations", "", "Copied verbatim from the matrix; each one bounds what the backend may ever claim.", ""])
    for entry in backends:
        lines.append(f"### `{entry['id']}` — {entry['display_name']}")
        lines.append("")
        for limitation in entry["limitations"]:
            lines.append(f"- {_text(limitation)}")
        lines.append("")

    lines.extend(
        [
            "## Release policy artifacts",
            "",
            "The artifact set",
            "[`release_bundle.validate_release_policy`](../../engineering/tools/release_bundle.py) already",
            "requires for every release. A backend outside this set cannot be admitted as a release",
            "artifact, however deep its lifecycle ran.",
            "",
        ]
    )
    lines.extend(
        _table(
            ["Artifact", "Description", "Required for release"],
            [
                [
                    f"`{entry['id']}`",
                    _text(entry["description"]),
                    _cell(entry["required_for_release"]),
                ]
                for entry in sorted(matrix["release_policy_artifacts"], key=lambda e: e["id"])
            ],
        )
    )

    lines.extend(
        [
            "## Out of scope",
            "",
            "Refused by [`generate_package_matrix.py`](../../engineering/tools/generate_package_matrix.py)",
            "rather than merely unimplemented: adding one of these needs the stated prerequisites first.",
            "",
        ]
    )
    lines.extend(
        _table(
            ["Target", "Reason", "Required before it may enter scope"],
            [
                [
                    f"`{entry['id']}`",
                    _text(entry["reason"]),
                    _text(entry["required_before_scope"]),
                ]
                for entry in sorted(matrix["out_of_scope"], key=lambda e: e["id"])
            ],
        )
    )

    return "\n".join(lines).rstrip("\n") + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=Path, default=MATRIX_PATH)
    parser.add_argument("--output", type=Path, default=STATUS_PATH)
    parser.add_argument(
        "--write-generated",
        action="store_true",
        help="Regenerate docs/distribution/package-status.md instead of checking it",
    )
    arguments = parser.parse_args(argv)

    try:
        content = render(load_matrix(arguments.matrix))
    except (PackageFrameworkError, PackageMatrixError, OSError) as error:
        print(f"package status FAILED: {error}", file=sys.stderr)
        return 1

    path = arguments.output
    if arguments.write_generated:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        print(f"Wrote {path.relative_to(REPO_ROOT) if path.is_relative_to(REPO_ROOT) else path}")
        return 0

    if path.is_symlink() or not path.is_file():
        print(
            f"package status FAILED: missing {path} "
            "(run: python3 engineering/tools/generate_package_status.py --write-generated)",
            file=sys.stderr,
        )
        return 1
    if path.read_text(encoding="utf-8") != content:
        print(
            f"package status FAILED: {path} is stale "
            "(run: python3 engineering/tools/generate_package_status.py --write-generated)",
            file=sys.stderr,
        )
        return 1
    print("package status OK (docs/distribution/package-status.md matches the package matrix)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
