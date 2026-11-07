# ADR 0042: Large Blob Storage v1 with a versioned Store boundary

- Status: accepted
- Date: 2026-11-07
- Deciders: storage, concurrency, API and release maintainers
- Applies to: opt-in blob-capable embedded Store; Blob File v1; persistence family v2
- Amends: ADR 0008/0010/0011 persistence and acknowledgement
  contracts, ADR 0009 read ownership, and ADR 0015/0031/0032 compaction and generation lifetimes
- Supersedes: none
- Preserves: ADR 0002 Segment geometry; current persistence v1 and wire v2 Stores

## Context

The requested maximum is 67,108,864 **value bytes**, independently of key and metadata bytes.
Normal Records are limited to 1,048,576 **encoded bytes**, including the 56-byte header, key,
value and alignment. Therefore a value of exactly 1 MiB cannot be inline with a nonempty key.
Enlarging Records or crossing Segment boundaries would weaken an existing invariant.

Large payloads also cannot be retained in Writer queues, immutable generation copies, or a
64 MiB network frame without changing admission and lifetime contracts. Filesystem rename alone
does not establish the durable dependency between a descriptor and its external payload.

Accepted by the project owner on 2026-11-07 for implementation. Acceptance is not proof of an
implemented feature or E3/E4 durability; existing v1 contracts and the release-claim ceiling remain.

## Decision drivers

- Preserve 64 MiB append-only Segments and the 1 MiB physical Record limit.
- Retain one logical key space and Writer-ordered replacement, erase and TTL semantics.
- Keep payload I/O and hashing outside Readers and Writers, with bounded credits and queues.
- Make every recoverable BlobRef depend on an already durable immutable payload.
- Make old-engine refusal explicit, including for an empty blob-capable Store.
- Integrate GC, backup and recovery with Manifest authority rather than a second database.

## Alternatives considered

1. **Increase the normal Record/Segment limit.** Rejected: changes the small-record path and the
   existing physical-format contract.
2. **Split payloads into ordinary user-visible keys.** Rejected: introduces key collisions,
   partial visibility and a separate multi-key atomicity problem.
3. **Recognize a value prefix or reuse a v1 reserved type/flag.** Rejected: opaque application
   values must remain unambiguous, and old readers must refuse the Store before recovery.
4. **Pack many blobs into mutable containers.** Deferred: adds relocation, fragmentation and
   pack-level recovery to the first milestone.
5. **Immutable file per blob plus a typed descriptor.** Selected: bounded sequential transfer,
   simple identity checks, independent chunk verification and conservative reachability GC.

## Decision

The target contract is [Large Blob Storage v1](../spec/large-blob-storage-v1.md).

1. Introduce an explicitly selected **persistence family v2**, with Manifest/header/Record/intent
   version changes specified there. **Blob File v1 is not persistence v1.** Legacy v1 Stores
   retain their bytes and limits; there is no automatic or in-place upgrade.
2. Keep each Segment exactly 64 MiB and each normal encoded Record at most 1 MiB. A new typed
   `blob_ref` Record carries an 80-byte descriptor; arbitrary application bytes are never
   interpreted as a descriptor.
3. Choose inline placement by the complete encoded extent, not only value length. Blob-capable
   Stores have an explicit key-size bound sufficient to encode a BlobRef for every accepted key.
4. Stage, hash, verify, synchronize and install an immutable blob using bounded I/O workers.
   Only then submit its descriptor to the owner Writer. The Writer orders logical mutations;
   beginning an upload does not reserve a key position or block later mutations.
5. In **all durable modes**, synchronize blob contents and the installed directory entry before
   descriptor admission. `durable_periodic` may defer the descriptor's durable boundary, but
   never its payload dependency. Strict sync/group ACK points remain after their existing
   descriptor durability and Reader-visibility boundaries.
6. Retain blobs referenced by any recoverable committed Record, publication in flight, Reader
   lease or backup lease. Overwrite/erase/TTL alone do not authorize file deletion. Compaction
   removes obsolete descriptor roots only after its Manifest transition is durable.
7. A streaming commit is terminal and cached on its embedded handle. A timeout after logical
   admission is indeterminate and cannot cancel Writer work. Persistent retry resolution and
   network opcodes require a later accepted contract before wire/SDK implementation.
8. The first milestone is embedded, durable, streaming PUT/GET with recovery and GC proofs.
   Existing daemon, C ABI v1, batch APIs, volatile mode and SDKs keep their current capabilities
   until their separate integration gates are satisfied. Unsupported entry points fail explicitly.

## Consequences

Small values avoid blob allocation, hashing and I/O dispatch. Large values add files, directory
syncs, inode pressure, recovery verification and explicit streaming lifetimes. Periodic mode pays
strict payload installation cost even though its logical mutation ACK is weaker; this is an
intentional first-version simplicity tradeoff, to be measured rather than hidden.

Conservative GC can retain obsolete blob bytes until Segment compaction removes historical
descriptors. Space quotas therefore account for staging, installed orphans and retained history,
not only currently visible values. Descriptor-only compaction may be useful even without a
Segment-count gain, requiring an explicit v2 maintenance policy and bounded copy budgets.

An owning `Store::get` may materialize 64 MiB; its allocation must use a separate bounded
materialization budget. The proposed 4 MiB streaming budget cannot be advertised for owning GET.

## Compatibility and migration

- The engine reads legacy persistence v1 unchanged and new opt-in v2 Stores separately.
- Old engines must reject Manifest v2 before examining Records or cleaning the namespace.
- V2 uses Record v2 and a versioned Segment header; merely adding a `ValueType` enumerator is
  insufficient. Compaction/bootstrap intents must also carry the v2 required-version boundary.
- Migration, if later implemented, is an explicit offline copy into a new Store. The source is
  preserved until destination verification; downgrade is possible only if all values and keys fit
  the legacy contract. No migration or downgrade is implied by this proposal.
- Backup must include the descriptor closure and verify blob data; legacy backup/repair/migrate
  tools must refuse v2 until they understand it.
- No wire v2 opcode or C ABI symbol is allocated by this ADR. Capability negotiation, transfer
  authentication, flow control, retry resolution and old-client behavior remain separate gates.

## Verification

The specification's acceptance matrix is the implementation checklist. It requires independent
codecs/fixtures, old-engine refusal, 64 MiB streaming, deterministic fault and crash boundaries,
same-key commit histories, Reader/GC/backup races, bounded overload and controlled inline A/B.
Retained native rows are required for macOS, Linux, FreeBSD and OpenBSD. E2 process-kill proof
does not constitute filesystem/device power-loss certification.

Tracking: [requirements](../../engineering/requirements/large-blobs.yaml),
[hazards](../../engineering/hazards/large-blobs.yaml),
[open gates](../../engineering/gates/large-blobs.yaml). Linked local evidence covers only its
recorded subset; the complete acceptance matrix and platform gates remain open.

## Lifecycle correction, 2026-11-07

The implementation must charge the peak staging/final extent, remove only staging files it
exclusively created, and release copied keys and payload buffers before returning terminal
transfer credits. Failed writes, including exceptions from I/O dispatch, remain terminal and
attempt pre-admission cleanup; failed cleanup preserves orphan accounting. Store close aborts
unfinished uploads before stopping their I/O workers. Configured syscall fault hooks also apply
to synchronization of the blob parent directory. These corrections implement the specified
lifecycle without moving descriptor admission or acknowledgement boundaries.

The [local lifecycle campaign](../../engineering/evidence/blob-lifecycle-local-2026-11-07.md)
retains targeted regression evidence. It does not close the embedded or durability gates.

## References

- [Persistence v1](../spec/persistence-v1.md)
- [Segment and Record format](../architecture/segment-format.md)
- [Manifest format](../architecture/manifest-format.md)
- [Paired embedded concurrency](0032-paired-concurrency-embedded-store.md)
- [Backup snapshot boundary](../spec/backup-restore-v1.md)
- [Platform durability evidence](../architecture/platform-durability-evidence.md)
