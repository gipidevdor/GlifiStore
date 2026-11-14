# Lean memory algorithm

Status: descriptive of the design in [ADR 0044](../adr/0044-bounded-prefix-compaction-remap.md),
[ADR 0045](../adr/0045-memory-resident-durable-reads.md) and
[ADR 0046](../adr/0046-unified-memory-reserve.md) (0045 and 0046 proposed)
Applies to: durable Stores; persistence v1
Owner: storage-engine maintainers
Last reviewed: 2026-11-14

GlifiStore keeps one rule for memory and storage:

> **The log is the only truth. RAM is derived, governed by one reserve. Compaction makes it dense.**

## Three structures, one direction of authority

```text
            authority                       derived, rebuildable
  ┌──────────────────────────┐     ┌────────────────────────────────────┐
  │ Manifest + Segment files │ ──▶ │ Index (key → RecordRef)  mandatory │
  │ append-only, CRC-checked │     │ Segment images (bytes)   optional  │
  └──────────────────────────┘     └────────────────────────────────────┘
```

- A write appends one Record to the active Segment, mirrors the same bytes into the active image
  (when resident), then publishes the new reference in the Index. Acknowledgement points are those of
  the durability mode; RAM never decides what is durable.
- A read finds the reference in the Index and decodes the Record from the image when resident,
  otherwise from the file. Both paths verify the same checksum, identity, key and expiry. A resident
  read completes on the calling thread; only a cold read takes a disk-read lane.
- Recovery rebuilds the Index from the files. Images and spill files are never read back.

## One reserve, two priorities

`memory_reserve_bytes` is the RAM the Store keeps resident for derived state.

1. **Index first, never refused.** The Worker Index and the paired Reader index are charged alike.
   When they outgrow the reserve, new arrays are mapped from an unlinked spill file (or left to
   swap), so the operating system pages them instead of the Store refusing writes.
2. **Images in what remains.** `image budget = reserve − Index`. Admission is a single comparison.

Nothing else competes for the reserve. The paired runtime does not allocate the ADR 0017 hot cache.

## One eviction rule

At a Worker's rotation, before any persistent write, drop that Worker's **oldest** sealed images
until the next active image fits. Dropping republishes the Segment generation without its image;
Readers that already hold it finish on the old bytes, and the existing generation reclamation
returns the pages. No new lifetime protocol exists.

## Compaction closes the loop

Compaction (ADR 0044) copies the live Records of a Worker's oldest sealed prefix, remaps their
references at commit, and retires the sources. Its outputs are mirrored while they are staged and
may borrow the image bytes of the sources they replace. The result:

- outputs hold only live data, so every resident byte serves a read;
- outputs are the newest images, so the oldest-first rule keeps them longest;
- retired sources release their charge at commit.

Over time the reserve converges to the live working set: dead bytes leave RAM through compaction,
cold live bytes leave through eviction, and the Index stays complete whatever happens to RAM.

## What each knob does

| Setting | Effect |
| --- | --- |
| `memory_reserve_bytes` | How much derived state stays resident; 0 means Index only (paged), no images |
| `index_spill_directory` | Where Index pages go beyond the reserve; empty means swap |
| `resident_reads_enabled` | Kill switch for images; reads fall back to the file path |

## Local measurement

One local run, not release evidence: macOS arm64, Release build, one Worker, 100 000 keys of
16 bytes, `durable_periodic`, 7 measured repeats after 2 warm-ups, median ns per GET.

```bash
glifistore_benchmarks --filter store-durable-periodic-get --ops 100000 --value-size N [--random] [--cold-reads]
```

| Value | Access | Resident | Cold | Speed-up |
| --- | --- | ---: | ---: | ---: |
| 64 B | sequential | 399 | 1 065 | 2.7× |
| 1 KiB | sequential | 640 | 1 562 | 2.4× |
| 4 KiB | sequential | 1 601 | 2 910 | 1.8× |
| 64 B | random | 498 | 1 292 | 2.6× |
| 1 KiB | random | 840 | 1 765 | 2.1× |
| 4 KiB | random | 2 132 | 3 089 | 1.4× |

The cold path here is served from the page cache, so the gap is the syscall and scratch copy alone;
a cold read that reaches the device costs far more. The remaining resident cost grows with the value:
CRC32C verification and the copy into the response.

## Limits of this design

- A resident read cannot observe on-disk damage that happens after load (HAZ-042); recovery,
  compaction, backup verification and cold reads still detect it.
- Spill files can fault on a full or copy-on-write filesystem (HAZ-043).
- During a Reader rebuild two bases coexist and both are charged.
- Image granularity is one Segment; a partially live sealed Segment is resident in full until
  compaction rewrites it.
