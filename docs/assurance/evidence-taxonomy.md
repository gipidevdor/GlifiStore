# Evidence taxonomy

**Normative** · evidence paths in requirements, gates, benchmarks and PRs · reviewed 2026-08-27

How much a result may support, according to where it was produced. This policy is maintained by
hand, not generated. A label classifies evidence; it never closes a gate by itself. A gate closes
only when requirement, specification or ADR, implementation, proofs, retained evidence and residual
risk all align ([AGENTS.md](../../AGENTS.md)). The claim ceiling stays architectural prototype.

## Labels

| Label | Meaning | Typical surfaces | May support |
| --- | --- | --- | --- |
| `local` | Developer or lab machine; environment not release-controlled | macOS laptop benches, unpinned cores, ad-hoc scripts under `benchmarks/results/local-*` | Debugging, harness bring-up, non-scaling relative signals |
| `CI` | Hosted or self-hosted workflow with pinned actions and scripts | `.github/workflows/*`, sanitizer/crash/fuzz smoke, required bounded formal TLC models | Regression and correctness gates (`PROVATA_IN_CI` / local CI proofs) |
| `hardware` | Labeled hard-pinned performance or physical durability lab | Runner `glifistore-linux-perf`, physical E3/E4 campaigns | Absolute throughput/p99 budgets, multi-core scaling claims, durability certification rows |
| `release` | Tagged artifact plus retained supply-chain and compatibility evidence | Signed release assets, SBOM/SLSA, N↔N−1 fixtures, install-consumer on a tag | Distribution and compatibility claims at the gate's release level |

An evidence path carries only one primary label in gate and requirement YAML. Do not mix `local`
medians into a `hardware` claim by rewording the README.

## Bans

1. **Do not promote macOS unpinned rows to scaling or absolute performance claims.** macOS and
   other unpinned benches are `local` only. Scaling (1/2/4/8 affine pairs, ≥80% physical-core
   efficiency, no migration) and absolute budgets require the Linux hard-pinned path on runner
   label `glifistore-linux-perf` with a `pass-candidate` manifest
   ([performance budgets](performance-budgets.md),
   [paired shards Linux P1 harness](../benchmarks/paired-shards-linux-p1.md)).
2. Do not cite `src/experimental/` as daemon or install proof. Experimental code may appear in lab,
   tests and benchmarks only, until an ADR promotes it into the official paired runtime and install
   graph; `glifistored` and the installed libraries stay free of experimental objects.
3. Do not close a gate with narrative alone. Design text or code presence without linked proof and
   evidence paths is not closure.
4. Rehearsal is not certification. The E3 script rehearsal remains honesty scaffolding until
   physical campaign evidence exists.

## Correctness gates and performance gates

Correctness, portability and distribution gates (`CI`, `Assurance`, FreeBSD/OpenBSD,
`install-consumer`, release sealing and evidence producers) close or advance independently of
absolute performance budgets and hard-pinned scaling rows. Landing release-sealing scaffolding must
not require `hardware` evidence from runner `glifistore-linux-perf`, and release sealing must not
absorb absolute performance claims from macOS `local` benches. Hardware work toward
`ACCETTATA_PER_RILASCIO` does not reopen the
[release sealing residuals](../distribution/wave5-l7-residuals.md).

## Pull request checklist

- [ ] Label stated (`local`, `CI`, `hardware` or `release`) next to any new bench or campaign path
- [ ] Scaling or absolute numbers → `hardware` + `glifistore-linux-perf` (or leave advisory)
- [ ] Experimental paths not wired into install or `glifistored`
- [ ] Requirement and gate `prove` / `evidenze` updated when behavior or proof moves

## Related

- [Performance budgets](performance-budgets.md): absolute placeholders stay
  `specified_waiting_for_runner`
- [Release checklist](release-checklist.md)
- [Production readiness](../production-readiness.md), the generated gate view
