# Mutation lifecycle structural refactor — performance posture

Date: 2026-08-26  
Scope: phases 3–7 behavior-neutral extraction (`fail_closed_state`, `mutation_execution`,
`mutation_batch`, `publication_coordinator`, `lane_state` nesting, `connection_lifecycle`)

## Verdict

**Neutral by construction.** Extractions are pure helpers / by-value aggregates and centralize
predicates already executed on the Writer/Reactor hot paths. No change to acknowledgement points,
publication release/acquire, batch caps (≤32), or wire mapping.

## Baseline (pre-refactor lab)

Recorded under `benchmarks/results/local-macos-2026-08-26-head-94f1307/`:

- PUT end-to-end dominated by Writer ack/handoff (~67%); publication ~14%
- `put_batch` ≤32 amortizes publication (+48% vs single PUT)

Structural phases intentionally do not claim a new median A/B campaign. Re-run the interlaced
`store-put` / `store-get` / `store-put-batch` / `index-find-hit` / TCP RAW suite (≥9 samples) before
any performance ADR or production claim; classify improvement / neutral / regression against that
baseline directory.

## Gate litmus (this change)

- unit: mutation_*, completion_policy, connection_lifecycle, property OVERLOADED
- integration: full `paired` filter (57) + selected reactor OVERLOADED / half-close
