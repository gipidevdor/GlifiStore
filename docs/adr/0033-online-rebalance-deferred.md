# ADR 0033: Online Worker rebalance — required design before implementation (deferred)

- Status: accepted
- Date: 2026-08-01
- Deciders: architects / persistence / networking maintainers
- Applies to: Worker ownership, wire `routing_epoch`, client cutover; not implemented in 0.1.x
- Amends: [0024](0024-offline-worker-migration.md) (records the deferred online path’s hard requirements)
- Supersedes: none
- Depends on: [0006](0006-key-routing-hash.md), [0012](0012-worker-affine-reactors.md),
  [0013](0013-native-wire-protocol-v2.md), [0030](0030-keyed-worker-routing.md),
  [0031](paired-reader-writer-shards.md)

## Context

Persistence v1 and wire v2 fix ownership as `hash(key) % worker_count` (or keyed SipHash under
ADR 0030) with a session `routing_epoch`. Clients treat `WRONG_OWNER` and epoch/count mismatch as
unhealthy and open a **new** client; protocol v2 defines no online rebalance. Offline Worker-count
change is already productized via `glifistore_migrate_store` (ADR 0024).

Product pressure exists to grow/shrink shard pairs without offline copy. Implementing that without a
frozen failure model would risk dual-ownership windows, silent reopen repartition, and client
retry duplication. This ADR freezes the **design constraints** that any future online rebalance
must satisfy and deliberately defers implementation past 0.1.x / persistence v1 single-node scope.

## Decision drivers

- Exact-key ownership without peer-fallback GET/PUT.
- Fail-closed reopen and recovery (no silent Manifest Worker-count rewrite).
- Clear client cutover: no automatic mutation retry across epochs.
- Bounded dual-ownership windows with explicit draining and authority publication.
- Compatibility with paired Reader/Writer shard pairs (ADR 0031/0032).

## Alternatives considered

1. **Implicit open-time rewrite when Worker count differs.** Rejected (ADR 0024): unauditable
   ownership change during recovery.
2. **Client-only sticky routing without epoch bump.** Rejected: servers and clients diverge;
   `WRONG_OWNER` storms.
3. **Immediate dual-write to old and new owners.** Rejected as default: doubles durability cost and
   complicates crash recovery without a versioned ownership table.
4. **Ship online reshard in 0.1.x without a routing-slot table.** Rejected: insufficient failure
   model.
5. **Keep offline migrate only until a later major.** Accepted for 0.1.x; online path remains a
   separate project once this ADR’s requirements are implemented and verified.

## Decision

### A. 0.1.x behavior (unchanged)

1. Reopen requires matching persisted Worker/shard-pair count and routing metadata.
2. Worker-count changes use offline migrate into a **new** data directory (ADR 0024).
3. Wire v2 clients must not implement online rebalance; epoch/count mismatch → unhealthy client.

### B. Required design for any future online rebalance (not implemented)

Any online reshard project **must** specify and prove all of the following before claiming support:

1. **Routing-slot / ownership table**  
   Stable mapping from key → owner that can change without relying solely on
   `hash % worker_count` of the old count. The table (or epoch-scoped function) is durable authority
   or is derived from durable authority with crash proofs.

2. **Epoch transition protocol**  
   - Monotonic `routing_epoch` (or successor field) published to clients.  
   - Distinct phases: `quiesce` → `dual_ownership_or_forward` → `cutover` → `retire`.  
   - At most one authoritative writer per key at any recovery-visible time, **or** an explicit
     dual-write algorithm with conflict resolution and durable proof.

3. **Client propagation**  
   - How clients learn the new epoch (INIT/response field, control opcode, or forced reconnect).  
   - `WRONG_OWNER` during transition: whether to refresh routing or fail closed.  
   - Mutation `bytes_sent > 0` across cutover remains `indeterminate` until reconcile (client
     semantics v1).

4. **Paired shard topology**  
   Reader/Writer pair count changes must define handoff of sockets, SPSC lanes, and
   `ReadGeneration` publication without breaking drain-before-close.

5. **Persistence interaction**  
   Segment ownership, compaction, and Manifest updates during transition must appear in the
   recovery state-transition matrix with crash/I/O fault evidence.

6. **Verification bar**  
   Linearizability (or a documented weaker model) under concurrent GET/PUT during cutover;
   process-kill and fault-injection matrices; explicit non-support for cross-epoch automatic
   mutation retry.

### C. Non-goals until the above lands

- Live Worker-count change on an open Store in 0.1.x.
- Wire v2 “transparent” rebalance without epoch bump.
- Treating offline migrate as online reshard.

## Consequences

**Positive:** Online reshard cannot be half-implemented; offline path remains the honest operator
tool; client SDKs keep a simple unhealthy-on-mismatch rule.

**Negative:** Horizontal scale-out of shard pairs still requires offline migrate or multiple Stores;
operators must plan capacity with fixed pair counts per data directory.

**Deferred work:** Protocol revision (likely v3 or INIT extensions), durable ownership table,
daemon orchestrator, and client auto-refresh — tracked outside persistence v1 alpha.

## Compatibility and migration

- No wire or Manifest change in this ADR.
- Does not amend offline migrate behavior.
- Future implementation that changes ownership rules requires a new ADR, fixture drops, and
  N↔N-1 matrix rows before release claims.

## Verification

For this ADR (design-only): documentation review; links from version lifecycle, compatibility
manual, and client conformance guide.

For a future implementation ADR: crash/fault matrices, client interop across epochs, and
performance evidence on labeled hardware — not satisfied by this document.

## References

- [ADR 0024 offline Worker migration](0024-offline-worker-migration.md)
- [Version lifecycle](../architecture/version-lifecycle.md)
- [Compatibility and migration manual](../operations/compatibility-and-migration.md)
- [TCP client conformance v1](../spec/tcp-client-conformance-v1.md)
- [Wire protocol v2](../spec/wire-protocol-v2.md)
- [Client semantics v1](../spec/client-semantics-v1.md)
