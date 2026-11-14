#!/usr/bin/env python3
"""Validate GlifiStore assurance artifacts under engineering/.

Checks JSON Schema conformance, unique IDs, referential integrity, critical
requirements without proofs, closed gates without evidence, expired waivers,
and optionally regenerates derived Markdown views.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

try:
    import yaml
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "PyYAML is required. Install with: python3 -m pip install PyYAML jsonschema"
    ) from exc

try:
    import jsonschema
    from jsonschema import Draft202012Validator
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "jsonschema is required. Install with: python3 -m pip install PyYAML jsonschema"
    ) from exc

REPO_ROOT = Path(__file__).resolve().parents[2]
ENG = REPO_ROOT / "engineering"
SCHEMAS = ENG / "schemas"
REQ_DIR = ENG / "requirements"
HAZ_DIR = ENG / "hazards"
GATE_DIR = ENG / "gates"
WAIVER_DIR = ENG / "waivers"
WORKFLOWS = REPO_ROOT / ".github" / "workflows"

CLOSED_GATE_STATES = {
    "PROVATA_IN_CI",
    "PROVATA_SU_HARDWARE",
    "VERIFICATA_INDIPENDENTEMENTE",
    "ACCETTATA_PER_RILASCIO",
}

SATISFIED_REQ_STATES = {"soddisfatto", "provato"}

SECTION_TITLES = {
    "public_contract": "Public contract",
    "durability_recovery": "Durability and recovery",
    "verification": "Verification",
    "operations_security": "Operations and security",
    "distribution_lifecycle": "Distribution and lifecycle",
}

SECTION_ORDER = list(SECTION_TITLES.keys())


class Validator:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warnings: list[str] = []
        self.schemas = {
            "requirement": self._load_schema("requirement.schema.json"),
            "hazard": self._load_schema("hazard.schema.json"),
            "gate": self._load_schema("gate.schema.json"),
            "waiver": self._load_schema("waiver.schema.json"),
        }
        self.requirements: dict[str, dict[str, Any]] = {}
        self.hazards: dict[str, dict[str, Any]] = {}
        self.gates: dict[str, dict[str, Any]] = {}
        self.waivers: dict[str, dict[str, Any]] = {}

    def _load_schema(self, name: str) -> dict[str, Any]:
        path = SCHEMAS / name
        if not path.is_file():
            self.errors.append(f"missing schema: {path.relative_to(REPO_ROOT)}")
            return {}
        return json.loads(path.read_text(encoding="utf-8"))

    def error(self, msg: str) -> None:
        self.errors.append(msg)

    def warn(self, msg: str) -> None:
        self.warnings.append(msg)

    def _validate_schema(self, kind: str, obj: dict[str, Any], where: str) -> None:
        schema = self.schemas.get(kind) or {}
        if not schema:
            return
        validator = Draft202012Validator(schema)
        for err in sorted(validator.iter_errors(obj), key=lambda e: list(e.path)):
            path = "/".join(str(p) for p in err.path)
            self.error(f"{where}: schema ({kind}) {path}: {err.message}")

    def _load_yaml_docs(self, directory: Path, key: str) -> list[tuple[Path, list[dict[str, Any]]]]:
        if not directory.is_dir():
            self.error(f"missing directory: {directory.relative_to(REPO_ROOT)}")
            return []
        out: list[tuple[Path, list[dict[str, Any]]]] = []
        for path in sorted(directory.glob("*.yaml")):
            data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
            items = data.get(key)
            if not isinstance(items, list):
                self.error(f"{path.relative_to(REPO_ROOT)}: expected top-level '{key}' list")
                continue
            out.append((path, items))
        return out

    def _path_exists(self, ref: str) -> bool:
        if not ref or ref.startswith(("http://", "https://")):
            return True
        # workflow short name
        if ref.endswith(".yml") and not ref.startswith(".") and "/" not in ref:
            return (WORKFLOWS / ref).is_file()
        candidate = REPO_ROOT / ref
        return candidate.exists()

    def _check_refs(self, refs: list[str], where: str, *, allow_workflow_keys: bool = False) -> None:
        for ref in refs:
            if allow_workflow_keys and ref.startswith("workflow:"):
                wf = ref.split(":", 1)[1]
                if not (WORKFLOWS / wf).is_file() and not (WORKFLOWS / f"{wf}.yml").is_file():
                    # tolerate bare name without suffix
                    if not (WORKFLOWS / wf).is_file():
                        self.error(f"{where}: missing workflow '{wf}'")
                continue
            if not self._path_exists(ref):
                self.error(f"{where}: missing path '{ref}'")

    def _check_evidence(self, evidenze: list[dict[str, Any]], where: str) -> bool:
        """Return True if at least one usable evidence pointer exists."""
        if not evidenze:
            return False
        ok = False
        for ev in evidenze:
            if not isinstance(ev, dict):
                self.error(f"{where}: evidence entry must be object")
                continue
            if "workflow" in ev:
                wf = ev["workflow"]
                path = WORKFLOWS / wf
                if not path.is_file() and not wf.endswith(".yml"):
                    path = WORKFLOWS / f"{wf}.yml"
                if not path.is_file():
                    self.error(f"{where}: missing workflow evidence '{wf}'")
                else:
                    ok = True
            if "path" in ev:
                if not self._path_exists(ev["path"]):
                    self.error(f"{where}: missing evidence path '{ev['path']}'")
                else:
                    ok = True
            if "note" in ev and "workflow" not in ev and "path" not in ev:
                # note-only is documentation, not evidence for closed gates
                pass
        return ok

    def load_all(self) -> None:
        required_req_files = [
            "core.yaml",
            "persistence.yaml",
            "recovery.yaml",
            "concurrency.yaml",
            "protocol.yaml",
            "security.yaml",
            "operations.yaml",
            "compatibility.yaml",
            "performance.yaml",
        ]
        for name in required_req_files:
            if not (REQ_DIR / name).is_file():
                self.error(f"missing required requirements file: engineering/requirements/{name}")

        for path, items in self._load_yaml_docs(REQ_DIR, "requirements"):
            for item in items:
                if not isinstance(item, dict):
                    self.error(f"{path.name}: requirement entry must be object")
                    continue
                rid = item.get("id", "<missing>")
                where = f"{path.relative_to(REPO_ROOT)}:{rid}"
                self._validate_schema("requirement", item, where)
                if rid in self.requirements:
                    self.error(f"duplicate requirement id {rid}")
                self.requirements[rid] = item

        for path, items in self._load_yaml_docs(HAZ_DIR, "hazards"):
            for item in items:
                if not isinstance(item, dict):
                    self.error(f"{path.name}: hazard entry must be object")
                    continue
                hid = item.get("id", "<missing>")
                where = f"{path.relative_to(REPO_ROOT)}:{hid}"
                self._validate_schema("hazard", item, where)
                if hid in self.hazards:
                    self.error(f"duplicate hazard id {hid}")
                self.hazards[hid] = item

        for path, items in self._load_yaml_docs(GATE_DIR, "gates"):
            for item in items:
                if not isinstance(item, dict):
                    self.error(f"{path.name}: gate entry must be object")
                    continue
                gid = item.get("id", "<missing>")
                where = f"{path.relative_to(REPO_ROOT)}:{gid}"
                self._validate_schema("gate", item, where)
                if gid in self.gates:
                    self.error(f"duplicate gate id {gid}")
                self.gates[gid] = item

        if WAIVER_DIR.is_dir():
            for path, items in self._load_yaml_docs(WAIVER_DIR, "waivers"):
                for item in items:
                    if not isinstance(item, dict):
                        self.error(f"{path.name}: waiver entry must be object")
                        continue
                    wid = item.get("id", "<missing>")
                    where = f"{path.relative_to(REPO_ROOT)}:{wid}"
                    self._validate_schema("waiver", item, where)
                    if wid in self.waivers:
                        self.error(f"duplicate waiver id {wid}")
                    self.waivers[wid] = item

    def check_semantic(self) -> None:
        today = dt.date.today()

        # Normative specs should be referenced by at least one requirement (soft warn if orphan)
        spec_dir = REPO_ROOT / "docs" / "spec"
        if spec_dir.is_dir():
            referenced = set()
            for req in self.requirements.values():
                for a in req.get("autorita", []):
                    referenced.add(a)
            for spec in sorted(spec_dir.glob("*.md")):
                rel = str(spec.relative_to(REPO_ROOT))
                if rel.endswith("README.md"):
                    continue
                if rel not in referenced:
                    self.warn(f"normative spec not referenced by any requirement: {rel}")

        for rid, req in self.requirements.items():
            where = f"requirement {rid}"
            self._check_refs(req.get("autorita", []), f"{where} autorita")
            self._check_refs(req.get("implementazione", []), f"{where} implementazione")
            self._check_refs(req.get("prove", []), f"{where} prove")
            self._check_evidence(req.get("evidenze", []), f"{where} evidenze")

            if req.get("criticita") == "critica" and not req.get("prove"):
                self.error(f"{where}: critical requirement has no proofs")

            if req.get("stato") in SATISFIED_REQ_STATES:
                if not req.get("prove"):
                    self.error(f"{where}: stato={req['stato']} but prove is empty")
                if not self._check_evidence(req.get("evidenze", []), f"{where} evidenze"):
                    # note-only evidence is insufficient for satisfied/proven
                    has_pointer = any(
                        isinstance(e, dict) and ("workflow" in e or "path" in e)
                        for e in req.get("evidenze", [])
                    )
                    if not has_pointer:
                        self.error(f"{where}: stato={req['stato']} without workflow/path evidence")

            for hid in req.get("hazards", []) or []:
                if hid not in self.hazards:
                    self.error(f"{where}: unknown hazard {hid}")
            for gid in req.get("gates", []) or []:
                if gid not in self.gates:
                    self.error(f"{where}: unknown gate {gid}")

        for hid, haz in self.hazards.items():
            where = f"hazard {hid}"
            self._check_refs(haz.get("prove", []), f"{where} prove")
            for rid in haz.get("requisiti", []):
                if rid not in self.requirements:
                    self.error(f"{where}: unknown requirement {rid}")

        brief_events = {
            "Conferma falsa di durabilità",
            "Perdita silenziosa",
            "Lettura obsoleta come corrente",
            "Resurrezione di una cancellazione",
            "Doppia applicazione",
            "Recupero non deterministico",
            "Perdita di una mutazione confermata",
            "Uso dopo liberazione",
            "Recupero prematuro di segmenti",
            "Scrittura nella partizione errata",
            "Divergenza fra instradamento e manifesto",
            "Compattazione che elimina dati visibili",
            "Rotazione incompleta",
            "Corruzione del manifesto",
            "Corruzione delle caselle di conferma",
            "Esaurimento dello spazio",
            "Quota esaurita",
            "Errori di scrittura differita",
            "Errori di sincronizzazione",
            "Fallimento durante la chiusura",
            "Arresto durante il backup",
            "Ripristino incompatibile",
            "Errore di autorizzazione",
            "Aggiramento dei limiti",
            "Saturazione delle code",
            "Starvation",
            "Blocco permanente",
            "Errore nella pubblicazione delle generazioni",
        }
        present_events = {h.get("evento_pericoloso") for h in self.hazards.values()}
        missing = sorted(brief_events - present_events)
        if missing:
            self.error("hazard register missing brief §7 events: " + "; ".join(missing))

        for gid, gate in self.gates.items():
            where = f"gate {gid}"
            self._check_refs(gate.get("prove", []), f"{where} prove")
            for rid in gate.get("requisiti", []):
                if rid not in self.requirements:
                    self.error(f"{where}: unknown requirement {rid}")
            has_ev = self._check_evidence(gate.get("evidenze", []), f"{where} evidenze")
            state = gate.get("stato")
            if state in CLOSED_GATE_STATES:
                if not gate.get("requisiti"):
                    self.error(f"{where}: stato={state} without linked requirements")
                if not has_ev:
                    self.error(f"{where}: stato={state} without existing workflow/path evidence")
                if not gate.get("prove"):
                    self.error(f"{where}: stato={state} without proofs")

        for wid, wav in self.waivers.items():
            where = f"waiver {wid}"
            try:
                exp = dt.date.fromisoformat(wav["scadenza"])
            except Exception:
                self.error(f"{where}: invalid scadenza")
                continue
            if exp < today or wav.get("stato") == "scaduta":
                if wav.get("stato") != "revocata":
                    self.error(f"{where}: expired waiver (scadenza={wav['scadenza']})")
            for rid in wav.get("requisiti", []) or []:
                if rid not in self.requirements:
                    self.error(f"{where}: unknown requirement {rid}")
            for gid in wav.get("gates", []) or []:
                if gid not in self.gates:
                    self.error(f"{where}: unknown gate {gid}")

        # Category coverage
        cats = {r.get("categoria") for r in self.requirements.values()}
        expected = {
            "core",
            "persistence",
            "recovery",
            "concurrency",
            "protocol",
            "security",
            "operations",
            "compatibility",
            "performance",
        }
        missing_cats = sorted(expected - cats)
        if missing_cats:
            self.error("missing requirement categories: " + ", ".join(missing_cats))

    def generate_production_readiness(self) -> str:
        lines: list[str] = []
        lines.append("<!-- GENERATED FILE. Do not edit by hand.")
        lines.append("     Authority: engineering/gates/*.yaml")
        lines.append("     Regenerate: python3 engineering/tools/validate_assurance.py --write-generated")
        lines.append("-->")
        lines.append("")
        lines.append("# Production readiness")
        lines.append("")
        lines.append("> **Derived view.** Machine-readable authority lives under")
        lines.append("> [`engineering/gates/`](../engineering/gates/). GlifiStore remains an")
        lines.append("> **architectural prototype**. A release level advances only when every")
        lines.append("> mandatory gate below has automated evidence. A design document or")
        lines.append("> implementation alone does not close a gate.")
        lines.append("")
        lines.append("## Daemon runtime boundary (0.1.0)")
        lines.append("")
        lines.append("`glifistored` runs only the paired Reader–Writer model")
        lines.append("([ADR paired shards](adr/paired-reader-writer-shards.md),")
        lines.append("[server model](architecture/server-model.md)): one ShardPair (Reader + serial")
        lines.append("Writer + SPSC lanes) per owner id. There is no dual-select daemon runtime.")
        lines.append("The former volatile prototype is removed from the tree; only the separate ADR 0036 generation laboratory remains under `src/experimental/`.")
        lines.append("")
        lines.append("## Release levels")
        lines.append("")
        lines.append("- **Prototype:** architecture and performance exploration; no compatibility or durability promise.")
        lines.append("- **Alpha:** public API and formats are versioned; destructive changes remain possible.")
        lines.append("- **Beta:** durability, recovery, upgrade, security, and operational contracts are feature-complete.")
        lines.append("- **Release candidate:** only correctness, compatibility, security, and operability fixes are accepted.")
        lines.append("- **Stable:** supported upgrade paths, published artifacts, and an explicit support lifetime exist.")
        lines.append("")
        lines.append("## Mandatory gates")
        lines.append("")
        lines.append("| State | Meaning |")
        lines.append("| --- | --- |")
        lines.append("| `NON_INIZIATA` … `IMPLEMENTATA` | Work incomplete or not yet proven |")
        lines.append("| `PROVATA_LOCALMENTE` | Proven outside mandatory CI evidence |")
        lines.append("| `PROVATA_IN_CI` | CI evidence path exists and is linked |")
        lines.append("| `PROVATA_SU_HARDWARE` / `VERIFICATA_INDIPENDENTEMENTE` / `ACCETTATA_PER_RILASCIO` | Higher claim levels |")
        lines.append("")

        by_section: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for gate in self.gates.values():
            by_section[gate["sezione"]].append(gate)

        for section in SECTION_ORDER:
            gates = sorted(by_section.get(section, []), key=lambda g: g["id"])
            if not gates:
                continue
            lines.append(f"### {SECTION_TITLES[section]}")
            lines.append("")
            for gate in gates:
                mark = "x" if gate["stato"] in CLOSED_GATE_STATES else " "
                # Keep checkbox semantics for humans; state is authoritative.
                title = gate["titolo"]
                narrative = (gate.get("narrative") or "").strip()
                lines.append(f"- [{mark}] **{gate['id']}** — {title}")
                lines.append(f"  State: `{gate['stato']}` · Release target: `{gate['livello_rilascio']}`")
                if gate.get("requisiti"):
                    lines.append("  Requirements: " + ", ".join(f"`{r}`" for r in gate["requisiti"]))
                lines.append(f"  Residual risk: {gate['rischio_residuo']}")
                if narrative:
                    lines.append(f"  {narrative}")
                lines.append("")

        lines.append("## Change discipline")
        lines.append("")
        lines.append("Any change to routing, hashing, persisted bytes, protocol framing, acknowledgement")
        lines.append("semantics, or reclamation requires an ADR and new compatibility or recovery")
        lines.append("evidence. Performance changes must preserve all safety and durability gates;")
        lines.append("benchmark improvement is never evidence of correctness.")
        lines.append("")
        lines.append("Assurance catalog: [`engineering/`](../engineering/) · Baseline:")
        lines.append("[`docs/assurance/engineering-baseline.md`](assurance/engineering-baseline.md) ·")
        lines.append("Agent rules: [`AGENTS.md`](../AGENTS.md).")
        lines.append("")
        return "\n".join(lines)

    def generate_hazards_md(self) -> str:
        lines = [
            "<!-- GENERATED FILE. Authority: engineering/hazards/*.yaml -->",
            "",
            "# Hazard register",
            "",
            "Generated from `engineering/hazards/`. GlifiStore is an architectural prototype;",
            "accepted residual risks do not imply production readiness.",
            "",
            "| ID | Event | Severity | Probability | Detectability | State | Requirements |",
            "| --- | --- | --- | --- | --- | --- | --- |",
        ]
        for hid in sorted(self.hazards):
            h = self.hazards[hid]
            reqs = ", ".join(f"`{r}`" for r in h.get("requisiti", []))
            lines.append(
                f"| `{hid}` | {h['evento_pericoloso']} | {h['severita']} | {h['probabilita']} | "
                f"{h['rilevabilita']} | {h['stato']} | {reqs} |"
            )
        lines.append("")
        return "\n".join(lines)

    def generate_gates_md(self) -> str:
        lines = [
            "<!-- GENERATED FILE. Authority: engineering/gates/*.yaml -->",
            "",
            "# Quality gates",
            "",
            "Generated from `engineering/gates/`. See also the derived",
            "[production-readiness](../production-readiness.md) view.",
            "",
            "| ID | Section | State | Release | Requirements |",
            "| --- | --- | --- | --- | --- |",
        ]
        for gid in sorted(self.gates):
            g = self.gates[gid]
            reqs = ", ".join(f"`{r}`" for r in g.get("requisiti", []))
            lines.append(
                f"| `{gid}` | {g['sezione']} | `{g['stato']}` | `{g['livello_rilascio']}` | {reqs} |"
            )
        lines.append("")
        return "\n".join(lines)

    def write_generated(self) -> None:
        assurance = REPO_ROOT / "docs" / "assurance"
        assurance.mkdir(parents=True, exist_ok=True)
        (REPO_ROOT / "docs" / "production-readiness.md").write_text(
            self.generate_production_readiness(), encoding="utf-8"
        )
        (assurance / "gates.md").write_text(self.generate_gates_md(), encoding="utf-8")
        (assurance / "hazards.md").write_text(self.generate_hazards_md(), encoding="utf-8")

    def check_generated_fresh(self) -> None:
        expected = {
            REPO_ROOT / "docs" / "production-readiness.md": self.generate_production_readiness(),
            REPO_ROOT / "docs" / "assurance" / "gates.md": self.generate_gates_md(),
            REPO_ROOT / "docs" / "assurance" / "hazards.md": self.generate_hazards_md(),
        }
        for path, content in expected.items():
            if not path.is_file():
                self.error(f"generated file missing: {path.relative_to(REPO_ROOT)} (run --write-generated)")
                continue
            current = path.read_text(encoding="utf-8")
            if current != content:
                self.error(
                    f"generated file stale: {path.relative_to(REPO_ROOT)} "
                    "(run: python3 engineering/tools/validate_assurance.py --write-generated)"
                )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--write-generated",
        action="store_true",
        help="Regenerate docs/production-readiness.md and docs/assurance/{gates,hazards}.md",
    )
    parser.add_argument(
        "--skip-generated-check",
        action="store_true",
        help="Do not fail when generated Markdown is missing/stale",
    )
    args = parser.parse_args(argv)

    v = Validator()
    v.load_all()
    v.check_semantic()

    if args.write_generated:
        if v.errors:
            print("Refusing to write generated files while validation has errors:", file=sys.stderr)
        else:
            v.write_generated()
            print("Wrote docs/production-readiness.md, docs/assurance/gates.md, docs/assurance/hazards.md")

    if not args.skip_generated_check and not args.write_generated:
        v.check_generated_fresh()
    elif args.write_generated and not v.errors:
        # After write, ensure consistency
        v.check_generated_fresh()

    for w in v.warnings:
        print(f"WARNING: {w}", file=sys.stderr)
    for e in v.errors:
        print(f"ERROR: {e}", file=sys.stderr)

    if v.errors:
        print(f"Assurance validation FAILED ({len(v.errors)} error(s))", file=sys.stderr)
        return 1

    print(
        f"Assurance validation OK "
        f"({len(v.requirements)} requirements, {len(v.hazards)} hazards, "
        f"{len(v.gates)} gates, {len(v.waivers)} waivers; {len(v.warnings)} warning(s))"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
