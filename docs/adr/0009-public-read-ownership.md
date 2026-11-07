# ADR 0009: Owned and pinned public reads

- Status: accepted
- Date: 2026-07-14
- Amended by: [0032](0032-paired-concurrency-embedded-store.md) (paired concurrency for ordinary
  GET; owning return value unchanged)

The alpha C++ API does not expose an unpinned `RecordView`, `Worker`, `Index`, `Segment`, Segment
manager, mutable Segment bytes, or caller-supplied routing hash as supported public surface.
Implementation headers may exist while the prototype is being migrated, but installing a header
does not by itself make that header a compatibility commitment.

The default public read returns an owning value copy. Under paired concurrency (ADR 0032) the Store
may satisfy that copy from an immutable published `ReadGeneration`, but the public result remains
an `OwnedValue` independent of later Store operations. A future zero-copy public read may return a
move-only pinned handle whose immutable spans remain valid for the handle lifetime. The handle
keeps the underlying Segment generation resident even if the key is replaced, vacuum publishes a
new Segment set, or the Store object closes. Reclamation, unmapping, and Segment reuse must wait for
all pins to be released.

Internal server executors may use non-owning views only while they hold an explicit execution or
Segment lifetime guarantee and finish response encoding before that guarantee ends. Such paths are
not public API and require dedicated lifetime tests.

This design makes safe ownership the default while retaining a defined route to zero-copy reads.
It also removes public access paths that currently bypass Worker synchronization or permit Segment
corruption. The detailed target contract and migration rules are in
[`public-api-contract.md`](../architecture/public-api-contract.md).

## Blob-profile amendment (2026-11-07)

[ADR 0042](0042-large-blob-storage.md) amends this decision for the explicit v2 blob profile.
Owning GET keeps its existing lifetime, including materialized blob values. New streaming handles are Store-scoped and are revoked safely by close.
Acceptance of that ADR does not close its implementation/evidence gate or establish E3/E4 certification.
