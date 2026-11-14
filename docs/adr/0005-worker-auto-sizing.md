# ADR 0005: Automatic Worker sizing

- Status: accepted
- Date: 2026-07-11
- Amended by: [0024](0024-offline-worker-migration.md) (offline Worker-count migration);
  [0031](paired-reader-writer-shards.md) / [0032](0032-paired-concurrency-embedded-store.md)
  (paired runtime; Worker count remains the ownership identity)

GlifiStore detects usable physical CPU and memory topology at startup, applies reserved-core,
maximum-Worker, memory-per-Worker, and explicit-override policies, and fixes the Worker count for
the process lifetime. The Store remains one logical key-space; physical Index partitions and
Segment assignment are internal.

For a durable Store, this selection occurs only at creation. Reopen uses the Worker count persisted
in the manifest; changing it requires an explicit migration because modulo routing would otherwise
change key ownership. Offline migration is specified in
[ADR 0024](0024-offline-worker-migration.md).
