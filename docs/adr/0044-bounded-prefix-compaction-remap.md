# ADR 0044: Bounded oldest-prefix compaction with commit-time reference remap

- Status: accepted (on maintainer direction, 2026-11-14)
- Date: 2026-11-14
- Deciders: storage and concurrency maintainers
- Applies to: paired runtime durable compaction; persistence v1; wire v2
- Amends: ADR 0015, ADR 0023, ADR 0039
- Supersedes: ADR 0043 (bounded concurrent delta), for the runtime compaction path

## Context

ADR 0015 selects the complete sealed history of one Worker as the unit of compaction, and ADR 0043
reconciles foreground Index changes made during the unlocked build with a bounded journal that is
replayed into a privately built replacement Index. Local measurements recorded under HAZ-041 show
three structural consequences:

1. The journal admits a bounded number of Index publications per transaction (default 4096). A
   transaction whose copy time is `T` aborts whenever the Worker publishes more than that many
   mutations in `T`. Because `T` grows with the Worker's total live bytes, a busy Worker with more
   than roughly `entries × copy_rate / op_rate` live bytes never completes a compaction: 0 of 39
   attempts succeeded with 128 MiB live at about 6000 op/s, while about 1 GiB of copies was
   abandoned and the Worker grew from 128 MiB to 1088 MiB in 25 seconds.
2. The intent gate requires the complete Manifest to equal the Phase A snapshot. Any rotation of any
   Worker during the copy rejects the transaction, so progress degrades with the number of Workers.
3. Normal-mode maintenance skips a Worker whose total sealed live bytes exceed the per-cycle copy
   budget, even when most of its reclaimable space sits in a few old Segments.

The reason ADR 0015 rewrote the whole sealed history was tombstone and TTL closure: dropping a
tombstone from one Segment can resurrect an older value retained in another Segment.

## Decision drivers

- Reclaim progress must not depend on foreground mutation rate.
- No resurrection of deleted or expired values; recovery keeps choosing exactly one of two exact
  Manifest authorities.
- Persistence v1 bytes, wire v2, acknowledgement points and client outcomes stay unchanged.
- Bounded work under the Worker commit gate, with no allocation after the next Manifest is durable.

## Alternatives considered

- **Keep ADR 0043 and raise the journal limits.** Moves the livelock threshold by a constant factor
  (hard ceiling 65536 entries) but keeps the dependence on total live bytes and on rotations.
- **Rebase the ADR 0043 journal across rotations only.** Removes cause 2 but not cause 1.
- **Arbitrary contiguous Segment ranges.** Reaches dead space behind cold live prefixes, but must
  retain every tombstone and every expired put whose key is absent from the Index, because TTL removal
  writes no tombstone. Deferred: it needs its own closure proof and retention accounting.
- **Persistent MVCC or per-key tombstone horizons.** Requires new persisted metadata; outside v1.

## Decision

### 1. Source set is the oldest sealed prefix of one Worker

A transaction selects the first `k` sealed Segments (`1 ≤ k ≤ n`) owned by one Worker, in
Manifest order. Persistence v1 recovery already requires the Worker's Segments in Manifest order to
carry strictly increasing, non-overlapping sequence ranges, so every Record of that Worker outside
the prefix is newer than every Record inside it. `k = n` is exactly the ADR 0015 transaction.
The v2 blob profile (ADR 0042) keeps `k = n`.

Closure argument. For a key `K`, recovery keeps the Record with the highest sequence and then hides
it if it is a tombstone or expired. Let `L` be that Record before the transaction.

- If `L` is outside the prefix, removing prefix Records for `K` cannot change the winner.
- If `L` is inside the prefix and is a visible put, the Index references it; it is copied with its
  sequence and metadata unchanged.
- If `L` is inside the prefix and is a tombstone or expired put, every older Record for `K` is also
  inside the prefix. All of them are dropped, so recovery finds no Record and `K` stays absent.

Outputs reuse the earliest source Segment IDs with incremented generations and take the positions of
the sources in the Manifest, exactly as ADR 0015. Surplus source entries disappear. Retained sealed
entries after the prefix and the active entry are unchanged. The canonical transition is therefore
determined by `(k, m)`, with `m ≤ k` outputs. The transition validator derives both from the exact
old and next Manifests, and recovery uses the derived `k` to name obsolete sources. No codec
changes. An intent written by this decision with `k < n` is rejected by a binary that predates it, so
downgrade across an interrupted transaction is unsupported. A clean shutdown leaves no intent and
remains downgrade-compatible.

### 2. Prefix selection under a copy budget

Phase A still captures an owning snapshot of the Worker Index, then attributes each live reference
to its sealed Segment. For each prefix length `k` the runtime estimates live bytes `L_k` and output
count `ceil(L_k / payload)`. It chooses the `k` that maximizes `k − ceil(L_k / payload)` subject to
`L_k ≤ max_copy_bytes` (no bound when the budget is zero, as in pressure and emergency modes); ties
prefer the smaller `k`. The exact layout is still validated before intent. The Index carries no
expiry, so expired puts still count as live in the estimate. When no prefix promises a gain, the
runtime runs the exact layout of the longest prefix within the budget, which drops expired puts and
reports an exact no-gain otherwise. A plan reclaiming no Segment is `not_beneficial`. If even the
oldest Segment exceeds the budget, the attempt is `not_beneficial` without a scan. When the budget
admits the complete sealed set, `k = n` is a candidate, so this policy never selects less estimated
reclaim than the ADR 0015 policy.

The maintenance controller (ADR 0023) no longer skips a durable Worker whose total live bytes exceed
`max_copy_bytes_per_cycle` (`copy_budget` is no longer produced). It passes the budget to the
runtime, and the rate-window check compares the budget-bounded copy instead of the Worker total.

### 3. Commit-time conditional remap replaces the delta journal

The builder no longer constructs a replacement Index. For every copied Record it returns the pair
`(source reference, output reference)` with its key; for every expired source put it drops, it
returns the source reference. After the next Manifest is durable, with the Worker commit gate armed
and Writers drained, the runtime applies under the Worker mutex:

- for a copied Record, if the Index entry for its key still equals the source reference, replace it
  with the output reference (and retarget a matching hot-cache row);
- for an expired drop, if the Index entry still equals the source reference, erase it without
  compaction and subtract its live bytes;
- otherwise leave the entry alone: a foreground mutation superseded it during the build, and the
  output copy is ordinary dead space reclaimed by a later transaction.

Each step is an in-place lookup and slot update: no allocation, no failure path, `O(copied Records)`
work. Live byte counters are unchanged by a remap because source and output are both sealed.
Foreground mutations never record anything for compaction, so no journal can overflow. No Index
reference can point into the prefix afterwards: the snapshot required no pending group publication,
and every later publication targets the active Segment.

### 4. Rebase across rotations at the intent boundary

When the builder requests the publication lease, the runtime supplies the current authoritative
Manifest. The transaction proceeds if every source still appears with the same identity as the
Worker's first `k` sealed Segments. Rotations only append newer Segments, so the plan is recomputed
against that Manifest with the same `(k, m)`. The intent still embeds exactly that old Manifest and
its next Manifest, so recovery still sees only two authorities. The lease excludes rotation and
other compaction until cleanup, and Phase C validates against the rebased old Manifest.

### Unchanged

Write/sync/rename ordering, intent and Manifest codecs, acknowledgement points, Reader pins and
descriptor lifetime, foreground `sequence_conflict` outcomes while the gate is armed, and fail-closed
behavior after an ambiguous persistent failure.

## Consequences

- Reclaim progress no longer depends on the foreground mutation rate, and unrelated rotations no
  longer abort a staged transaction.
- Copy work per transaction is bounded by the selected prefix rather than the Worker's total live
  bytes. The budget no longer prevents compaction of large Workers.
- Records superseded during the build are written once more as dead bytes, which raises physical
  write amplification slightly under heavy overwrite.
- Cold live data at the start of the history must be copied before newer dead space can be reclaimed
  (prefix head-of-line). Range compaction (deferred) would remove that cost.
- The Phase A Index enumeration is still `O(N)` under synchronization (unchanged residual).
- ADR 0043 resource limits `max_compaction_delta_entries` and `max_compaction_delta_key_bytes` stay
  accepted and validated for source and configuration compatibility, but no longer bound anything.
  Delta counters in telemetry remain present and report zero.
- Pre-existing and unchanged: in periodic or group durability modes a sealed Record may be superseded
  by a newer Record that is published but not yet synchronized. Dropping the sealed Record before that
  sync allows a crash to lose both. Whole-Worker compaction has the same exposure; it is recorded as a
  residual risk, not introduced here.

## Compatibility and migration

Persistence v1 Record, Segment, Manifest and intent bytes are unchanged; wire v2 is unchanged. Any
existing Store opens unchanged, and whole-sealed-set intents written by earlier binaries recover
unchanged (`k = n`). Public C++ result fields gain prefix and remap counters; the stable C ABI is
unchanged.

## Verification

- Transition validator: canonical prefix transitions accepted for every `(k, m)`; reordered, gapped,
  non-prefix and wrong-generation transitions rejected; whole-set fixtures unchanged.
- Recovery with an intent for `k < n`, under both authorities, including partial retirement and
  rollback.
- Runtime: prefix compaction preserves a sequential model across PUT/overwrite/ERASE/TTL, including
  tombstones and expired puts inside and after the prefix; reopen agrees with the model.
- Sustained same-Worker overwrite during compaction completes the transaction, where ADR 0043
  aborted on journal overflow.
- A rotation of another Worker during the build no longer aborts the transaction.
- Existing crash, fault, allocation and sanitizer suites keep passing.
- The decision is stated as definitions and theorems in the
  [durable compaction model](../architecture/durable-compaction-model.md). The items above are
  checked by exhaustion over a finite scope and the runtime is compared with the model over the same
  scope.

An exploratory local measurement (HAZ-041, 2026-11-14, Debug, one Worker, 128 MiB live) completed
8 of 12 transactions at about 6,000 op/s with no abandoned copy and a bounded Segment count, where
ADR 0043 completed 0 of 39. It is not a retained campaign. The multi-output crash fixtures keep more
than one output of live Records in the first two Segments so that the complete sealed set remains
the strictly better prefix and the existing checkpoints stay meaningful.

Local evidence is architectural-prototype evidence and does not promote a gate or certify E3/E4.

## References

- [ADR 0015: Whole-Worker compaction](0015-whole-worker-compaction.md)
- [ADR 0039: Pre-intent staging](0039-pre-intent-staged-compaction-output.md)
- ADR 0043: Bounded compaction delta (removed from the tree; available in Git history)
- [Durable compaction](../architecture/durable-compaction.md)
- [Durable compaction model](../architecture/durable-compaction-model.md)
- [Recovery state matrix](../spec/recovery-state-matrix-v1.md)
