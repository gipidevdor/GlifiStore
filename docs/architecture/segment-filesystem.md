# Durable Segment files

**Descriptive** · persistence v1 Segment files

The Segment-file layer behind end-to-end `durable_sync` creation, recovery and mutation: names,
creation and preallocation, the append and commit protocol, scans and reads. Process-kill evidence
remains a certification gate.

## Names and creation

A Segment identity maps to one canonical lowercase filename:

```text
segment-<16 hexadecimal Segment-ID digits>-<8 hexadecimal generation digits>.glifi
```

Creation uses the recognizable temporary `.<final-name>.tmp`, private mode `0600`, `O_EXCL`,
`O_NOFOLLOW` and directory-relative operations. The filename is only a namespace key: opening also
decodes the immutable header and requires the complete expected Store ID, Segment ID, generation and
owner Worker.

The creation state machine:

```text
encode initial header (slot 0 generation 1, empty active extent)
  -> reject an existing final identity
  -> create private temporary
  -> physically reserve exactly 64 MiB
  -> write the 4 KiB header
  -> synchronize the complete file
  -> rename to the final name
  -> synchronize the data directory
```

An injected or real failure before `renameat` is `not_published` and removes the temporary name. A
failure after `renameat` is attempted, or during the directory synchronization that follows, is
`indeterminate` and poisons the `DataDirectory` instance. This matches Manifest publication: atomic
visibility is not evidence that a name survived a machine failure.

## Platform allocation

A file is not accepted after a sparse `ftruncate` alone. The allocation path is chosen at compile
time:

| Platform | Allocation path | Reason |
|---|---|---|
| Linux | direct `fallocate(fd, 0, 0, 64 MiB)` syscall | native optimal allocation without the unsafe userspace-emulation fallback described for glibc `posix_fallocate` |
| macOS | `F_PREALLOCATE` with contiguous/all-or-nothing first, then non-contiguous/all-or-nothing; exact `ftruncate` afterward | reserves backing store while preferring low fragmentation; preallocation alone does not set logical EOF |
| FreeBSD | native `posix_fallocate` syscall | the OS guarantees the requested range is allocated or returns an error number |
| OpenBSD | eager sequential zero writes in bounded 256 KiB chunks | OpenBSD exposes no documented reservation syscall; this avoids approving a merely sparse file |

Linux documents that a successful default `fallocate` prevents later writes in the range from
failing for lack of disk space, and FreeBSD documents the same guarantee for `posix_fallocate`.
Apple documents `F_PREALLOCATE` as reserving backing store and recommends the contiguous, then
non-contiguous, fallback pattern. OpenBSD's eager path is a separate case and still requires native
filesystem crash and disk-full certification.

## Append and commit protocol

`append` accepts only a complete canonical Record that passes structural and CRC32C validation. Its
sequence must be non-zero and strictly greater than the last committed sequence. Checked arithmetic
rejects generation or count overflow and capacity exhaustion before any I/O.

```text
pwrite complete Record at selected committed_end
  -> data synchronization
  -> encode the next generation into the other 128-byte slot
  -> pwrite the complete slot
  -> data synchronization
  -> update the in-memory selected slot
```

No header read-modify-write occurs. Slot writes are fixed-size and alternate between offsets 128 and
256, which preserves the previous valid recovery boundary. `seal` uses the same alternate-slot
protocol without changing the Record extent.

The outcome classification is deliberately asymmetric:

| Failure boundary | Outcome | Meaning |
|---|---|---|
| argument, Record write, or Record-data sync | `not_committed` | the selected slot still defines the old extent; any tail is ignored |
| before-hook throw/Status ahead of the matching write/sync | `not_committed` | hook runs before durable bytes or slot authority change |
| before attempting the slot write (fault seam) | `not_committed` | Record bytes may be durable, but remain outside recovery authority |
| slot write attempted or following sync failed | `indeterminate` | either old or new valid slot may win after restart; every handle sharing the data directory is poisoned |
| slot sync completed | `committed` | the new boundary is recovery authority |

After `indeterminate`, the shared `DataDirectory` health state fails closed across Segment and
Manifest operations. Callers must stop mutating, close and recover; they must not infer success from
bytes visible through the page cache.

The shared state is also poisoned before a `DataDirectory` releases or replaces its process lock, so
a Segment handle cannot outlive the lock owner and keep mutating storage.

## Open, scan and read

Open rejects non-private files, symbolic links, extra hard links, incorrect ownership, any size
other than 64 MiB, a mismatched immutable identity and headers without an unambiguous valid commit
slot.

A recovery scan reads exactly `[4096, committed_end)` in one bounded, contiguous positional read.
Memory is therefore `O(committed bytes)` with a hard 64 MiB Segment bound, and the syscall count
does not depend on the Record count. The decoder advances by each canonical encoded extent and
validates checksum, alignment, strict sequence ordering, Record count and the first and last commit
sequences. It never reads or interprets a crash tail. The returned `RecordRef` list is
`O(record count)` and can feed a deterministic Index rebuild.

The recovery orchestrator uses the lower-level visitor form of this scan: it consumes each decoded
`RecordView` before the bounded buffer is released, which avoids an extra read and CRC pass per
Record. See the [durable recovery implementation](recovery-implementation.md).

A positional Record read requires a matching Segment ID and generation, an aligned, fully committed,
bounded extent and the expected sequence, then revalidates the Record checksum. Stale generations
and malformed extents cannot escape the selected recovery boundary.

Runtime reads open Segment handles read-only; the active owner upgrades its one cached handle to
read-write for mutation. Read-only handles reject append and seal. The single-Record visitor
performs one positional read and one canonical, CRC-verified decode, then exposes a view only for
the callback's lifetime. The bounded cache, commit-snapshot check, mutation ordering and fail-closed
behavior are in the [durable runtime catalog](durable-runtime-catalog.md).

## Evidence and limits

Unit tests cover exact size and identity, duplicate creation, durable reopen, alternating slots,
sealing, uncommitted-tail recovery, indeterminate slot poisoning, committed-region corruption,
directory-lifetime fail-closed behavior, read-only mutation rejection, and allocation, write and
sync fault boundaries.

The macOS implementation is exercised locally. Linux, FreeBSD and OpenBSD have native or VM CI build
and test gates; they still require pinned filesystem rows plus real disk-full, short-I/O,
process-kill and power-loss testing before any platform is certified for durable service.
