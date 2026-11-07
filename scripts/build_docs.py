#!/usr/bin/env python3
"""Build Doxygen Pages from headers and a disposable copy of the existing guides."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import quote, unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
SITE = ROOT / "docs/doxygen"
INLINE_LINK = re.compile(r"!?\[[^\]]*\]\(\s*(?P<target><[^>\n]+>|[^\s)]+)")
REFERENCE_LINK = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*(?P<target><[^>\n]+>|\S+)")
FENCE = re.compile(r"^\s{0,3}(?P<marker>`{3,}|~{3,})")


def stage_guides(destination: Path, source_url: str, revision: str) -> None:
    sources = sorted(
        path for path in (ROOT / "docs").rglob("*.md") if SITE not in path.parents
    )
    sources.append(SITE / "index.md")
    staged = {path: destination / path.relative_to(ROOT) for path in sources}
    staged[SITE / "index.md"] = destination / "glifistore.md"

    def rewrite_target(match: re.Match[str], source: Path) -> str:
        target = match.group("target").strip("<>")
        url = urlsplit(target)
        if url.scheme or url.netloc or not url.path:
            return match.group(0)
        resolved = (source.parent / unquote(url.path)).resolve()
        try:
            relative = resolved.relative_to(ROOT)
        except ValueError:
            return match.group(0)
        suffix = (f"?{url.query}" if url.query else "") + (
            f"#{url.fragment}" if url.fragment else ""
        )
        if resolved in staged:
            replacement = staged[resolved].as_posix() + suffix
        elif (
            relative.parts
            and relative.parts[0] == "include"
            and resolved.suffix in {".h", ".hpp"}
        ):
            replacement = resolved.as_posix() + suffix
        else:
            kind = "tree" if resolved.is_dir() else "blob"
            if match.group(0).startswith("!["):
                kind = "raw"
            replacement = f"{source_url}/{kind}/{quote(revision, safe='')}/{quote(relative.as_posix())}{suffix}"
        start, end = match.span("target")
        offset = match.start()
        return (
            match.group(0)[: start - offset]
            + f"<{replacement}>"
            + match.group(0)[end - offset :]
        )

    # Leave examples alone: their Markdown links are content, not navigation.
    for source, target in staged.items():
        lines: list[str] = []
        fence_marker = ""
        fence_length = 0
        for line in source.read_text(encoding="utf-8").splitlines(keepends=True):
            fence = FENCE.match(line)
            if fence:
                marker = fence.group("marker")
                if not fence_marker:
                    fence_marker, fence_length = marker[0], len(marker)
                elif marker[0] == fence_marker and len(marker) >= fence_length:
                    fence_marker = ""
            elif not fence_marker:
                line = INLINE_LINK.sub(
                    lambda match, source=source: rewrite_target(match, source), line
                )
                line = REFERENCE_LINK.sub(
                    lambda match, source=source: rewrite_target(match, source), line
                )
            lines.append(line)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("".join(lines), encoding="utf-8")


class LocalLinks(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.targets: list[str] = []
        self.anchors: set[str] = set()

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        for key, value in attrs:
            if key in {"href", "src"} and value:
                self.targets.append(value)
            if key in {"id", "name"} and value:
                self.anchors.add(value)


def validate_site(html: Path) -> None:
    """Reject missing generated pages/assets, including untranslated Markdown links."""
    errors: set[str] = set()
    parsed: dict[Path, LocalLinks] = {}
    for page in sorted(html.rglob("*.html")):
        parser = LocalLinks()
        parser.feed(page.read_text(encoding="utf-8"))
        parsed[page] = parser
    for page, parser in parsed.items():
        for target in parser.targets:
            url = urlsplit(target)
            if url.scheme or url.netloc:
                continue
            resolved = (page.parent / unquote(url.path)).resolve() if url.path else page
            if not resolved.is_relative_to(html) or not resolved.exists():
                errors.add(f"{page.relative_to(html)}: {target}")
            elif (
                url.fragment
                and resolved in parsed
                and unquote(url.fragment) not in parsed[resolved].anchors
            ):
                # Doxygen's alphabet indexes can emit jump links for letters
                # without a section, and encode '~' differently in section IDs.
                # These generated index shortcuts still target a valid page.
                generated_shortcut = url.fragment.startswith(
                    "index_"
                ) and resolved.name.startswith(("functions", "globals"))
                if not generated_shortcut:
                    errors.add(f"{page.relative_to(html)}: missing anchor: {target}")
    if errors:
        raise RuntimeError("Broken generated links:\n" + "\n".join(sorted(errors)))
    print(
        f"Validated local pages, assets, and fragment links in {len(parsed)} HTML files."
    )


def build(output: Path, doxygen: str, source_url: str, revision: str) -> None:
    version_text = subprocess.check_output([doxygen, "--version"], text=True).strip()
    version = tuple(int(part) for part in version_text.split()[0].split(".")[:3])
    if version < (1, 18, 0):
        raise RuntimeError(f"Doxygen >= 1.18.0 required; found {version_text}")
    output.mkdir(parents=True, exist_ok=True)
    inputs = output / "input"
    # Only delete our disposable input tree; keep CMake's build files intact.
    if inputs.exists():
        shutil.rmtree(inputs)
    if (output / "html").exists():
        shutil.rmtree(output / "html")
    stage_guides(inputs, source_url.rstrip("/"), revision)

    config = (SITE / "Doxyfile.in").read_text(encoding="utf-8")
    for key, value in {
        "ROOT": ROOT.as_posix(),
        "OUTPUT": output.as_posix(),
        "INPUT": inputs.as_posix(),
        "THEME": (SITE / "theme").as_posix(),
        "VERSION": (ROOT / "VERSION").read_text(encoding="utf-8").strip(),
    }.items():
        config = config.replace(f"@{key}@", value)
    doxyfile = output / "Doxyfile"
    doxyfile.write_text(config, encoding="utf-8")

    header = output / "header.html"
    subprocess.run(
        [
            doxygen,
            "-w",
            "html",
            str(header),
            str(output / "footer.html"),
            str(output / "default.css"),
            str(doxyfile),
        ],
        check=True,
        cwd=ROOT,
    )
    extensions = """
<script src="$relpath^doxygen-awesome-darkmode-toggle.js"></script>
<script src="$relpath^doxygen-awesome-fragment-copy-button.js"></script>
<script>
DoxygenAwesomeDarkModeToggle.init();
DoxygenAwesomeFragmentCopyButton.init();
</script>
"""
    header.write_text(
        header.read_text(encoding="utf-8").replace("</head>", extensions + "</head>"),
        encoding="utf-8",
    )
    try:
        subprocess.run([doxygen, str(doxyfile)], check=True, cwd=ROOT)
    finally:
        warnings = output / "warnings.log"
        if warnings.exists() and warnings.stat().st_size:
            print(warnings.read_text(encoding="utf-8"), end="")
    html = output / "html"
    (html / ".nojekyll").touch()
    shutil.copyfile(SITE / "theme/LICENSE", html / "doxygen-awesome-LICENSE.txt")
    validate_site(html)
    print(f"Doxygen {version_text}: {html / 'index.html'}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/docs")
    parser.add_argument("--doxygen", default="doxygen")
    parser.add_argument(
        "--source-url", default="https://github.com/gipidevdor/GlifiStore"
    )
    parser.add_argument("--revision", default="main")
    args = parser.parse_args()
    build(args.output.resolve(), args.doxygen, args.source_url, args.revision)


if __name__ == "__main__":
    main()
