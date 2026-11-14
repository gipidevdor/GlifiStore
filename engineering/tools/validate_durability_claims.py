#!/usr/bin/env python3
"""Fail closed on dishonest durability certification claims.

Rules:
1. Hosted CI / in-repo rehearsal artifacts must keep e3_certified=no and e4_certified=no.
2. Any evidence or claim that sets e3_certified=yes (or e4_certified=yes) must reference a
   retained pinned campaign directory that includes campaign-pin.txt, campaign-provenance.txt,
   campaign-summary.md, and a SHA-256 checksum manifest.
3. Requirements may claim at most E2 unless a pinned campaign evidence path is linked.

See docs/architecture/platform-durability-evidence.md and GS-PERSIST-FAULT-001.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "PyYAML is required. Install with: python3 -m pip install PyYAML"
    ) from exc

REPO_ROOT = Path(__file__).resolve().parents[2]
ENG = REPO_ROOT / "engineering"
REQ_DIR = ENG / "requirements"
CLAIMS_DIR = ENG / "claims"
EVIDENCE_DIR = ENG / "evidence"

CERT_YES = re.compile(r"(?im)^(?:e3_certified|e4_certified)\s*=\s*yes\s*$")
CERT_YES_INLINE = re.compile(r"(?i)\b(?:e3_certified|e4_certified)\s*=\s*yes\b")
E3_CERTIFIED_YES_MD = re.compile(r"(?i)E3\s+certified:\s*`?yes`?")
E4_CERTIFIED_YES_MD = re.compile(r"(?i)E4\s+certified:\s*`?yes`?")
CLAIM_E3_OR_HIGHER = re.compile(r"(?i)\bE[34](?:\b|[^0-9a-z]|$)")

PINNED_REQUIRED = (
    "campaign-pin.txt",
    "campaign-provenance.txt",
    "campaign-summary.md",
)


class DurabilityClaimValidator:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warnings: list[str] = []

    def error(self, msg: str) -> None:
        self.errors.append(msg)

    def warn(self, msg: str) -> None:
        self.warnings.append(msg)

    def _is_text(self, path: Path) -> bool:
        if path.suffix.lower() in {
            ".png",
            ".jpg",
            ".jpeg",
            ".gif",
            ".webp",
            ".pdf",
            ".zip",
            ".tar",
            ".gz",
            ".tgz",
            ".xz",
            ".bin",
            ".so",
            ".dylib",
            ".a",
            ".o",
        }:
            return False
        try:
            sample = path.read_bytes()[:512]
        except OSError:
            return False
        if b"\0" in sample:
            return False
        return True

    def _scan_file_for_cert_yes(self, path: Path) -> list[str]:
        if not self._is_text(path):
            return []
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            self.error(f"{path.relative_to(REPO_ROOT)}: unreadable ({exc})")
            return []
        hits: list[str] = []
        if CERT_YES.search(text) or CERT_YES_INLINE.search(text):
            hits.append("e*_certified=yes")
        if E3_CERTIFIED_YES_MD.search(text):
            hits.append("E3 certified: yes")
        if E4_CERTIFIED_YES_MD.search(text):
            hits.append("E4 certified: yes")
        return hits

    def _validate_pinned_campaign(self, campaign_dir: Path, where: str) -> None:
        if not campaign_dir.is_dir():
            self.error(f"{where}: pinned campaign path missing: {campaign_dir}")
            return
        for name in PINNED_REQUIRED:
            if not (campaign_dir / name).is_file():
                self.error(f"{where}: pinned campaign missing {name} under {campaign_dir}")
        pin = campaign_dir / "campaign-pin.txt"
        if pin.is_file():
            pin_text = pin.read_text(encoding="utf-8", errors="replace")
            if "pin_label=" not in pin_text and "source_commit=" not in pin_text:
                self.error(f"{where}: campaign-pin.txt lacks pin_label=/source_commit=")
        # Checksums: accept either checksums.sha256 or sha256sums.txt
        if not any((campaign_dir / n).is_file() for n in ("checksums.sha256", "sha256sums.txt", "SHA256SUMS")):
            self.error(f"{where}: pinned campaign missing SHA-256 manifest")
        prov = campaign_dir / "campaign-provenance.txt"
        if prov.is_file():
            prov_text = prov.read_text(encoding="utf-8", errors="replace")
            if not re.search(r"(?m)^e3_certified=yes$", prov_text):
                self.error(
                    f"{where}: provenance must set e3_certified=yes when claiming certification"
                )
            if re.search(r"(?m)^source_dirty=yes$", prov_text):
                self.error(f"{where}: certified campaign must have source_dirty=no")

    def scan_tree(self, root: Path, *, allow_certified_with_pin: bool = False) -> None:
        if not root.exists():
            return
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            if path.name == ".gitkeep":
                continue
            # Skip schema/tool sources that mention the forbidden pattern as documentation.
            rel = str(path.relative_to(REPO_ROOT))
            if rel.startswith("engineering/tools/"):
                continue
            if rel.startswith("engineering/schemas/"):
                continue
            hits = self._scan_file_for_cert_yes(path)
            if not hits:
                continue
            if allow_certified_with_pin and path.parent.name:
                # Claims may point at a sibling campaign dir via YAML; raw yes in evidence
                # without pin metadata is still forbidden unless this path is inside a
                # directory that already satisfies the pin contract.
                parent = path.parent
                if all((parent / n).is_file() for n in PINNED_REQUIRED):
                    self._validate_pinned_campaign(parent, rel)
                    continue
            self.error(
                f"{rel}: durability certification claim ({', '.join(hits)}) without "
                "retained pinned campaign (campaign-pin.txt + provenance + checksums)"
            )

    def check_requirements(self) -> None:
        if not REQ_DIR.is_dir():
            self.error("missing engineering/requirements/")
            return
        for path in sorted(REQ_DIR.glob("*.yaml")):
            data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
            for req in data.get("requirements") or []:
                if not isinstance(req, dict):
                    continue
                rid = req.get("id", "<missing>")
                claim = str(req.get("claim_massimo") or "")
                if not CLAIM_E3_OR_HIGHER.search(claim):
                    continue
                # E3/E4 in claim_massimo requires pinned campaign evidence path.
                evidenze = req.get("evidenze") or []
                pinned = False
                for ev in evidenze:
                    if not isinstance(ev, dict):
                        continue
                    camp = ev.get("campaign_path") or ev.get("path")
                    if not camp:
                        continue
                    camp_path = REPO_ROOT / camp if not Path(camp).is_absolute() else Path(camp)
                    if camp_path.is_dir() and all(
                        (camp_path / n).is_file() for n in PINNED_REQUIRED
                    ):
                        self._validate_pinned_campaign(camp_path, f"{rid} evidenze")
                        pinned = True
                if not pinned:
                    self.error(
                        f"requirement {rid}: claim_massimo={claim!r} requires a retained "
                        "pinned campaign evidence path (campaign_path/path with pin files)"
                    )

    def check_claims_yaml(self) -> None:
        if not CLAIMS_DIR.is_dir():
            return
        for path in sorted(CLAIMS_DIR.glob("*.yaml")):
            data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
            claims = data.get("claims") if isinstance(data, dict) else None
            if claims is None and isinstance(data, dict) and "e3_certified" in data:
                claims = [data]
            if not isinstance(claims, list):
                continue
            for claim in claims:
                if not isinstance(claim, dict):
                    continue
                cid = claim.get("id", path.name)
                if str(claim.get("e3_certified", "no")).lower() != "yes" and str(
                    claim.get("e4_certified", "no")
                ).lower() != "yes":
                    continue
                camp = claim.get("campaign_path")
                if not camp:
                    self.error(f"claim {cid}: e3/e4_certified=yes without campaign_path")
                    continue
                camp_path = REPO_ROOT / camp if not Path(camp).is_absolute() else Path(camp)
                self._validate_pinned_campaign(camp_path, f"claim {cid}")

    def run(self) -> int:
        # Evidence tree: any e3_certified=yes must sit inside a pinned campaign dir.
        self.scan_tree(EVIDENCE_DIR, allow_certified_with_pin=True)
        self.scan_tree(CLAIMS_DIR, allow_certified_with_pin=True)
        self.check_claims_yaml()
        self.check_requirements()

        for w in self.warnings:
            print(f"WARNING: {w}", file=sys.stderr)
        for e in self.errors:
            print(f"ERROR: {e}", file=sys.stderr)
        if self.errors:
            print(
                f"Durability claim validation FAILED ({len(self.errors)} error(s))",
                file=sys.stderr,
            )
            return 1
        print("Durability claim validation OK (E0–E2 honesty; no unpinned E3/E4)")
        return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args(argv)
    return DurabilityClaimValidator().run()


if __name__ == "__main__":
    raise SystemExit(main())
