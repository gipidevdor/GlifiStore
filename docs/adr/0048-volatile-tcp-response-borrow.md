# ADR 0048: Encode small volatile TCP values within the Reader generation lease

- Status: accepted (on maintainer direction, 2026-11-21)
- Date: 2026-11-21
- Deciders: maintainer direction to correct the measured hot-shard path; executor Codex
- Applies to: volatile paired read generations and the daemon's contiguous GET output
- Amends: [0031](paired-reader-writer-shards.md) (internal read borrow)
- Supersedes: none

## Context

HAZ-045 records a structural capacity limit of fixed-owner shards. The finite local campaign
exposed sensitivity to concentrating clients on one Reader. It did not prove saturation or
sibling isolation. Inspection found avoidable work on each volatile TCP GET: the generation
materializes an `OwnedValue`, then the Reactor copies those bytes into its contiguous output.
Values above the inline buffer allocate even when the response immediately copies them again.

## Decision

Add a validated volatile Record view whose spans borrow the exact immutable generation. The
caller must retain that generation's owning reference or Reader lease for every access. This
operation keeps delta-before-base selection, exact Segment-generation pins, bounds, CRC32C,
Record identity, key and expiry checks. Durable generations still require their preparation path.
The existing owning `get` builds an independent value from this view and keeps its lifetime contract.

For a volatile daemon response below the existing pipelined-scatter threshold, the Reader encodes
the view directly into its bounded contiguous output before returning from `execute_local`.
No borrowed span is stored in a connection, completion, held-response slot or output lease. The
Reader cannot adopt another generation or publish quiescence within that synchronous call. The
queued output owns all bytes before any later event-loop adoption, write, overwrite or close.
The removed intermediate allocation can no longer fail; output admission/allocation keeps the
existing per-connection failure handling. No mutation is retried or reclassified.
Values at or above the scatter threshold keep their independently owned output lease. Durable
resident and cold reads retain the current owning path and ordered response windows.

## Compatibility and limits

No acknowledgement, visibility barrier, routing, retry, persistence or wire byte changes are made.
Socket writes may still complete later; their input is the existing owning output buffer. Public
Store and client GET results remain owning. This is a reduction in Reader work, not redistribution:
HAZ-045 and HAZ-046 stay open, and a serial Writer or single hot key remains a capacity constraint.

## Verification

Native tests compare borrowed bytes and metadata with owning reads in base and delta generations,
including overwrite, tombstones, TTL and exact generation lifetime. TCP tests retain validation of
pipeline order and bytes across publication and output backpressure. A local Release comparison
must preserve both binary fingerprints, source fingerprints, raw validated profiles and run order, including
an unchanged-binary control. It remains exploratory, without a hardware acceptance or gate closure.
