# ADR 0008: Alpha persistence and durability contract

- Status: accepted
- Date: 2026-07-14

The alpha storage engine supports three explicit modes: `volatile`, `durable_sync`, and
`durable_periodic`. Volatile mode acknowledges after in-memory Index publication and makes no
restart guarantee. Durable-sync mode requires a data directory and acknowledges a mutation only
after its Record bytes and a newer, CRC-protected Segment commit slot have both reached the
configured synchronization boundary. Durable-periodic mode is specified in
[ADR 0010](0010-durable-periodic-policy.md); it acknowledges after in-memory publication and
defers Segment synchronization to a configurable periodic flusher.

Each persistent Segment header contains two alternating commit slots. A valid slot identifies a
monotonic commit generation, the committed byte extent, Record count, and last sequence. Recovery
selects the highest valid slot and ignores all bytes beyond its committed extent. This avoids
guessing whether an invalid trailing Record is corruption or a torn write.

The durable commit point is the successful synchronization of the new commit slot. Before that
point, a failed operation must not be recovered as committed. After that point, the runtime must
publish the corresponding in-memory state or fail closed; it must not report a recoverable failure
that could later reappear as a successful mutation. Loss of the client connection after the commit
point but before receipt of the response leaves the outcome indeterminate to that client.

A versioned manifest records Store identity, format versions, the Segment catalog, active Segment
ownership, routing hash identifier, Worker count, and routing epoch. Worker auto-sizing applies
only when a durable Store is first created. Reopening uses the persisted Worker count. A requested
hash, Worker count, or routing configuration mismatch is rejected until an explicit offline
migration exists ([ADR 0024](0024-offline-worker-migration.md)).

Manifest replacement uses write-temporary, synchronize-file, atomic rename, and
synchronize-directory ordering. Segment creation is synchronized and made visible in the manifest
before the Segment accepts committed Records. Unknown files are never silently adopted. Index
checkpoints may accelerate startup but remain disposable derived state; Segments and the manifest
are the recovery authority.

The detailed normative behavior and crash-state rules are in
[`durability-recovery.md`](../architecture/durability-recovery.md). The exact v1 byte layouts are in
the [manifest format](../architecture/manifest-format.md) and
[Segment format](../architecture/segment-format.md) specifications.

## Blob-profile amendment (2026-11-07)

[ADR 0042](0042-large-blob-storage.md) amends this decision for the explicit v2 blob profile.
The opt-in v2 profile installs and synchronizes an immutable blob before admitting its typed descriptor; legacy v1 acknowledgement points remain unchanged.
Acceptance of that ADR does not close its implementation/evidence gate or establish E3/E4 certification.
