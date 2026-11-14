# ADR 0006: Key routing hash

- Status: accepted
- Date: 2026-07-11
- Amended by: [0024](0024-offline-worker-migration.md) (offline Worker-count migration);
  [0030](0030-keyed-worker-routing.md) (optional SipHash-2-4 keyed routing + Manifest seed)

Worker routing uses a deterministic 64-bit hash over the full logical key bytes. The selected
Worker is `hash(key) % worker_count`, where `worker_count` is fixed for the process lifetime.

Routing algorithm **v1** is FNV-1a 64-bit. Routing algorithm **v2** (`siphash24-v1`, ADR 0030) uses
SipHash-2-4 with a Manifest-persisted seed. Default Stores keep FNV.

The hash is also stored in each Record as metadata for recovery ownership checks. Changing the
routing hash function or seed changes Worker ownership and must fail closed on reopen mismatch.

Durable Store creation persists the routing algorithm identifier, Worker hash seed (zero for FNV),
Worker count, and routing epoch. Reopen rejects incompatible requested values instead of silently
repartitioning existing Records.
