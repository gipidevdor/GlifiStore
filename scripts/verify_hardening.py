#!/usr/bin/env python3
"""Verify Linux hardening from emitted commands and/or a distributed ELF executable."""

from __future__ import annotations

import argparse
import json
import pathlib
import shlex
import shutil
import subprocess
import sys


def fail(message: str) -> None:
    raise RuntimeError(message)


def require_token(tokens: list[str], token: str, context: str) -> None:
    if token not in tokens:
        fail(f"{context} is missing {token}")


def compile_tokens(build_dir: pathlib.Path) -> list[str]:
    database = json.loads((build_dir / "compile_commands.json").read_text(encoding="utf-8"))
    matches = [entry for entry in database if entry["file"].endswith("/src/server/daemon_main.cpp")]
    if len(matches) != 1:
        fail(f"expected one daemon_main.cpp command, found {len(matches)}")
    entry = matches[0]
    return entry.get("arguments") or shlex.split(entry["command"])


def expand_response_files(tokens: list[str], build_dir: pathlib.Path) -> list[str]:
    """Expand CMake/Ninja @rsp files so LINKER: flags are visible to checks."""
    expanded: list[str] = []
    for token in tokens:
        if not token.startswith("@"):
            expanded.append(token)
            continue
        candidate = pathlib.Path(token[1:])
        if not candidate.is_absolute():
            candidate = build_dir / candidate
        if not candidate.is_file():
            expanded.append(token)
            continue
        expanded.extend(shlex.split(candidate.read_text(encoding="utf-8", errors="replace")))
    return expanded


def link_tokens(build_dir: pathlib.Path) -> list[str]:
    ninja = shutil.which("ninja")
    if ninja is None:
        fail("ninja is required to inspect the generated link command")
    output = subprocess.check_output(
        [ninja, "-C", str(build_dir), "-t", "commands", "glifistored"], text=True
    )
    links = [line for line in output.splitlines() if " -o glifistored " in f" {line} "]
    if len(links) != 1:
        fail(f"expected one glifistored link command, found {len(links)}")
    return expand_response_files(shlex.split(links[0]), build_dir)


def ninja_file_mentions(build_dir: pathlib.Path, needle: str) -> bool:
    ninja_file = build_dir / "build.ninja"
    if not ninja_file.is_file():
        return False
    return needle in ninja_file.read_text(encoding="utf-8", errors="replace")


def has_linker_option(tokens: list[str], option: str, build_dir: pathlib.Path) -> bool:
    joined = " ".join(tokens)
    if (
        option in tokens
        or f"-Wl,{option}" in tokens
        or f"-Xlinker {option}" in joined
        or f"LINKER:{option}" in tokens
    ):
        return True
    # CMake/Ninja may emit `-Wl,-z -Wl,relro` or `-Xlinker -z -Xlinker relro`.
    if option.startswith("-z,"):
        value = option.split(",", 1)[1]
        compact = joined.replace(" ", "")
        if f"-z,{value}" in joined or f"-z{value}" in compact:
            return True
        if f"-Wl,-z,-Wl,{value}" in compact or f"-Wl,-z,{value}" in joined:
            return True
        if f"-Xlinker-z-Xlinker{value}" in compact:
            return True
        for index, token in enumerate(tokens[:-1]):
            nxt = tokens[index + 1]
            if token in ("-z", f"-Wl,-z") and nxt in (value, f"-Wl,{value}"):
                return True
            if token == "-Xlinker" and nxt == "z" and index + 3 < len(tokens):
                if tokens[index + 2] == "-Xlinker" and tokens[index + 3] == value:
                    return True
        # Last resort: CMake may stash LINKER: options only in generated ninja text.
        if ninja_file_mentions(build_dir, option) or ninja_file_mentions(build_dir, f"-Wl,{option}"):
            return True
        if ninja_file_mentions(build_dir, f"-z {value}") or ninja_file_mentions(build_dir, value):
            # Require a -z neighbour somewhere for the bare value match.
            text = (build_dir / "build.ninja").read_text(encoding="utf-8", errors="replace")
            if f"-z,{value}" in text or f"-z {value}" in text or f"-Wl,-z,{value}" in text:
                return True
    elif option == "-pie":
        if ninja_file_mentions(build_dir, "-pie") or ninja_file_mentions(build_dir, "-Wl,-pie"):
            return True
    return False


def verify_commands(build_dir: pathlib.Path) -> None:
    compile = compile_tokens(build_dir)
    require_token(compile, "-std=c++23", "daemon compile command")
    if any(token.startswith("-std=gnu++") for token in compile):
        fail("daemon compile command enables GNU language extensions")
    require_token(compile, "-Werror", "strict daemon compile command")
    require_token(compile, "-Wpedantic", "strict daemon compile command")
    require_token(compile, "-fstack-protector-strong", "daemon compile command")
    require_token(compile, "-fPIE", "daemon compile command")
    require_token(compile, "-D_FORTIFY_SOURCE=3", "optimized daemon compile command")
    if not any(token.startswith("-O") and token != "-O0" for token in compile):
        fail("fortified daemon command is not optimized")

    link = link_tokens(build_dir)
    for option in ("-pie", "-z,relro", "-z,now"):
        if not has_linker_option(link, option, build_dir):
            preview = " ".join(link)
            if len(preview) > 400:
                preview = preview[:400] + "…"
            fail(f"glifistored link command is missing {option}; link={preview}")


def readelf(binary: pathlib.Path, *options: str) -> str:
    executable = shutil.which("readelf")
    if executable is None:
        fail("readelf is required to inspect the final ELF")
    return subprocess.check_output([executable, *options, str(binary)], text=True)


def verify_elf(binary: pathlib.Path) -> None:
    header = readelf(binary, "-h")
    if "Type:" not in header or "DYN" not in header:
        fail("glifistored is not an ELF position-independent executable")
    if "GNU_RELRO" not in readelf(binary, "-l"):
        fail("glifistored has no PT_GNU_RELRO segment")
    dynamic = readelf(binary, "-d")
    if "BIND_NOW" not in dynamic and not any(
        "FLAGS" in line and "NOW" in line for line in dynamic.splitlines()
    ):
        fail("glifistored does not request immediate dynamic binding")
    if "__stack_chk_fail" not in readelf(binary, "-Ws"):
        fail("glifistored has no observable stack-protector dependency")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("build_dir", type=pathlib.Path, nargs="?")
    parser.add_argument("--binary", type=pathlib.Path)
    args = parser.parse_args()
    if (args.build_dir is None) == (args.binary is None):
        parser.error("provide exactly one build directory or --binary")
    if args.binary is not None:
        binary_input = args.binary.expanduser()
        if binary_input.is_symlink() or not binary_input.is_file():
            fail("distributed ELF binary is missing or unsafe")
        binary = binary_input.resolve()
        if binary.stat().st_mode & 0o111 == 0:
            fail("distributed ELF binary is not executable")
        verify_elf(binary)
        print("hardening verification passed: distributed ELF properties")
        return 0
    build_dir = args.build_dir.resolve()
    verify_commands(build_dir)
    verify_elf(build_dir / "glifistored")
    print("hardening verification passed: ISO C++23, emitted flags, and ELF properties")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError, RuntimeError, ValueError) as error:
        print(f"hardening verification failed: {error}", file=sys.stderr)
        sys.exit(1)
