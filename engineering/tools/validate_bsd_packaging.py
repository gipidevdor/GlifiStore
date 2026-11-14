#!/usr/bin/env python3
"""Validate BSD reference-port invariants against repository authorities."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


class PackagingError(RuntimeError):
    pass


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise PackagingError(f"cannot read {path}: {error}") from error


def require(text: str, pattern: str, description: str) -> None:
    if re.search(pattern, text, flags=re.MULTILINE) is None:
        raise PackagingError(f"missing {description}")


def validate(root: Path, release: bool = False) -> None:
    root = root.resolve()
    product = read(root / "VERSION").strip()
    abi = read(root / "ABI_VERSION").strip()
    license_text = read(root / "LICENSE")
    if re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", product) is None:
        raise PackagingError("VERSION authority is invalid")
    abi_match = re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", abi)
    if abi_match is None:
        raise PackagingError("ABI_VERSION authority is invalid")
    abi_major = abi_match.group(1)

    required = [
        "packaging/freebsd/Makefile",
        "packaging/freebsd/pkg-descr",
        "packaging/freebsd/pkg-plist",
        "packaging/freebsd/files/glifistored.in",
        "packaging/freebsd/files/glifistored.conf.sample",
        "packaging/openbsd/Makefile",
        "packaging/openbsd/pkg/DESCR",
        "packaging/openbsd/pkg/PLIST",
        "packaging/openbsd/pkg/glifistored.rc",
        "packaging/openbsd/files/glifistored.conf",
    ]
    missing = [name for name in required if not (root / name).is_file()]
    if missing:
        raise PackagingError(f"reference ports miss required files: {', '.join(missing)}")

    freebsd_make = read(root / "packaging/freebsd/Makefile")
    freebsd_plist = read(root / "packaging/freebsd/pkg-plist")
    freebsd_rc = read(root / "packaging/freebsd/files/glifistored.in")
    freebsd_config = read(root / "packaging/freebsd/files/glifistored.conf.sample")
    require(freebsd_make, rf"^DISTVERSION=\s*{re.escape(product)}\s*$", "FreeBSD product version")
    if "BSD 3-Clause License" not in license_text:
        raise PackagingError("repository LICENSE is not the expected BSD-3-Clause text")
    require(freebsd_make, r"^LICENSE=\s*BSD3CLAUSE\s*$", "FreeBSD BSD-3-Clause license metadata")
    require(freebsd_make, r"^LICENSE_FILE=\s*\$\{WRKSRC\}/LICENSE\s*$",
            "FreeBSD license authority")
    require(freebsd_make, r"^USES=.*\bcmake:testing\b.*\bssl\b", "FreeBSD system TLS/CMake policy")
    require(freebsd_make, r"^USE_LDCONFIG=\s*yes\s*$", "FreeBSD shared-library cache integration")
    require(freebsd_make, r"^USE_RC_SUBR=\s*glifistored\s*$", "FreeBSD rc.subr integration")
    require(freebsd_make, r"^USERS=\s*glifistore\s*$", "FreeBSD dedicated service user")
    require(freebsd_plist, rf"^lib/libglifistore\.so\.{abi_major}$", "FreeBSD ABI-major library")
    require(freebsd_plist, rf"^lib/libglifistore\.so\.{re.escape(abi)}$", "FreeBSD full ABI library")
    require(freebsd_plist, r"^@sample etc/glifistored\.conf\.sample$", "FreeBSD preserved config")
    require(freebsd_plist, r"^share/GlifiStore/examples/glifistored\.conf\.sample$",
            "FreeBSD portable runtime sample")
    require(freebsd_plist, r"^@dir\(glifistore,glifistore,0750\) /var/db/glifistore$",
            "FreeBSD owned data directory")
    require(freebsd_rc, r"^# KEYWORD: shutdown$", "FreeBSD shutdown ordering")
    require(freebsd_rc, r"command_args=.*-p \$\{pidfile\}.*-u \$\{glifistored_user\}",
            "FreeBSD daemon pid/user isolation")
    require(freebsd_config, r"^data-dir=/var/db/glifistore$", "FreeBSD native data path")

    openbsd_make = read(root / "packaging/openbsd/Makefile")
    openbsd_plist = read(root / "packaging/openbsd/pkg/PLIST")
    openbsd_rc = read(root / "packaging/openbsd/pkg/glifistored.rc")
    openbsd_config = read(root / "packaging/openbsd/files/glifistored.conf")
    require(openbsd_make, rf"^V\s*=\s*{re.escape(product)}\s*$", "OpenBSD product version")
    require(openbsd_make, r"^PERMIT_PACKAGE\s*=\s*Yes\s*$", "OpenBSD package permission")
    require(openbsd_make, rf"^SHARED_LIBS \+=\s*glifistore {re.escape(abi)}\s*$",
            "OpenBSD ABI major/minor library")
    require(openbsd_make, r"^MODULES\s*=\s*devel/cmake\s*$", "OpenBSD native CMake module")
    require(openbsd_make, r"^WANTLIB \+=.*\bcrypto\b.*\bssl\b", "OpenBSD base LibreSSL dependency")
    require(openbsd_plist, r"^@newuser _glifistore:", "OpenBSD dedicated service user")
    require(openbsd_plist, r"^@lib lib/libglifistore\.so\.\$\{LIBglifistore_VERSION\}$",
            "OpenBSD ports-controlled shared library")
    sample_file = openbsd_plist.find("share/examples/glifistore/glifistored.conf")
    sample_directive = openbsd_plist.find("@sample ${SYSCONFDIR}/glifistored.conf")
    if sample_file < 0 or sample_directive <= sample_file:
        raise PackagingError("OpenBSD @sample must immediately follow the installed example")
    require(openbsd_plist, r"^@rcscript \$\{RCDIR\}/glifistored$", "OpenBSD rc.d packaging")
    require(openbsd_plist, r"^share/GlifiStore/examples/glifistored\.conf\.sample$",
            "OpenBSD portable runtime sample")
    require(openbsd_plist, r"^@sample /var/glifistore/$", "OpenBSD preserved data directory")
    require(openbsd_rc, r'^daemon="\$\{TRUEPREFIX\}/bin/glifistored"$', "OpenBSD TRUEPREFIX daemon")
    require(openbsd_rc, r'^daemon_user="_glifistore"$', "OpenBSD daemon user")
    require(openbsd_rc, r"^rc_bg=YES$", "OpenBSD background service policy")
    require(openbsd_rc, r"^\. /etc/rc\.d/rc\.subr$", "OpenBSD rc.subr integration")
    require(openbsd_config, r"^data-dir=/var/glifistore$", "OpenBSD native data path")

    combined = "\n".join((freebsd_make, openbsd_make))
    if re.search(r"bundled|vendored|FetchContent", combined, flags=re.IGNORECASE):
        raise PackagingError("BSD ports must not request bundled/vendored dependencies")

    if release:
        release_requirements = [
            "packaging/freebsd/PORTS_ACCOUNT_REGISTERED",
            "packaging/openbsd/PORTS_ACCOUNT_REGISTERED",
        ]
        absent = [name for name in release_requirements if not (root / name).is_file()]
        if absent:
            raise PackagingError(
                "native release package prerequisites are not proven: " + ", ".join(absent)
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--release", action="store_true")
    arguments = parser.parse_args()
    try:
        validate(arguments.root, arguments.release)
    except PackagingError as error:
        print(f"BSD packaging FAILED: {error}", file=sys.stderr)
        return 1
    state = "release prerequisites present" if arguments.release else "structural only; native proof pending"
    print(f"BSD packaging OK ({state})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
