# Persistence filesystem layer

**Descriptive** · persistence v1 filesystem primitives, every durable policy · reviewed 2026-08-26

The low-level filesystem primitives behind durable Store creation, recovery, mutation, rotation,
compaction and backup. The selected acknowledgement policy decides how often they synchronize;
filesystem and device certification still requires retained native power-loss evidence, and no row
is E3/E4 certified.

## Security and namespace policy

`DataDirectory` opens an existing directory with `O_DIRECTORY`, `O_CLOEXEC` and `O_NOFOLLOW`, then
anchors every engine-owned name to that descriptor with `openat`, `unlinkat` and `renameat`.
Changing the process working directory cannot redirect an operation. The directory must be owned by
the effective user and must not be writable by group or other users.

The lock, the temporary Manifest and the authoritative Manifest must be private regular files. Reads
reject symbolic links, multiple hard links, foreign ownership and group or other write permission.
Creation uses `O_EXCL`, `O_NOFOLLOW`, close-on-exec and mode `0600`. These checks reduce pathname
substitution and special-file blocking attacks; they do not make advisory locks enforceable against
a process that deliberately ignores the protocol.

| Engine-owned name | Purpose |
|---|---|
| `.glifistore.lock` | lifetime of the exclusive process lock |
| `.manifest.glifi.tmp` | replaceable, recognizable publication temporary |
| `manifest.glifi` | authoritative complete manifest |
| `.glifistore.bootstrap.tmp` | temporary bootstrap-intent publication |
| `.glifistore.bootstrap` | bootstrap intent encoded as a complete v1 Manifest |
| `.glifistore.compaction.tmp` | temporary compaction-intent publication |
| `.glifistore.compaction` | checksummed compaction intent with old and replacement manifests |
| `.segment-<id>-<generation>.glifi.tmp` | recognizable unpublished Segment temporary |
| `segment-<id>-<generation>.glifi` | immutable-identity Segment generation |

## Platform synchronization

| Platform | Data synchronization | Metadata/publication synchronization |
|---|---|---|
| Linux | `fdatasync` when only retrievable file data is required | `fsync` for manifests and directory entries |
| macOS | `F_BARRIERFSYNC` for Record-before-slot ordering; `F_FULLFSYNC` for the durable commit point | `fcntl(F_FULLFSYNC)` for both manifest and directory descriptors |
| FreeBSD | `fdatasync` for data-only requests | `fsync` for manifests and directories |
| OpenBSD | `fdatasync` API, currently implemented as `fsync` by the OS | `fsync` for manifests and directories |

Linux documents that synchronizing a file does not necessarily synchronize its directory entry, so
publication synchronizes the directory explicitly. It also documents that `fdatasync` includes the
file-size metadata needed to retrieve written data. See the Linux
[`fsync(2)`](https://man7.org/linux/man-pages/man2/fsync.2.html) and
[`rename(2)`](https://man7.org/linux/man-pages/man2/rename.2.html) manuals.

Apple documents that `F_BARRIERFSYNC` orders I/O synchronized before the barrier ahead of I/O issued
after it, while `F_FULLFSYNC` drains the device queue and makes earlier synchronized data
persistent. GlifiStore uses that two-phase protocol for Segment Record and commit-slot publication
on supported HFS/APFS storage. The final commit point, Manifest publication and directory
publication still use `F_FULLFSYNC`; there is no downgrade to ordinary `fsync`. An unsupported
barrier or full flush is an I/O failure. See Apple's
[`fsync(2)`](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html)
and
[`fcntl(2)`](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fcntl.2.html)
manuals.

FreeBSD and OpenBSD use their native `fdatasync` and `fsync` contracts. OpenBSD warns explicitly
that an `EIO` synchronization failure may leave partially written data, and that the failure keeps
being reported until the references close. See the
[FreeBSD manual service](https://man.freebsd.org/cgi/man.cgi?query=fsync&sektion=2) and OpenBSD
[`fsync(2)`](https://man.openbsd.org/fsync.2).

## Complete positional I/O

`FileDescriptor` is move-only and closes exactly once. Positional reads and writes:

- reject extents that `off_t` cannot represent;
- split requests at `SSIZE_MAX`;
- retry only operations interrupted by `EINTR`;
- continue after short `pread`/`pwrite` results;
- fail on a zero-progress write or a premature EOF.

No caller shares a file position, which keeps the design safe for future concurrent access to
distinct Segment offsets. `close` is deliberately not retried, because several supported systems may
already have released the descriptor after an interrupted close; the explicit synchronization call
collects durability errors before RAII destruction.

Each test `DataDirectory` may carry instance-local raw file-I/O callbacks with the same result and
`errno` contract as `pread`, `pwrite` and the synchronization syscalls. Production leaves them null
and takes the direct native call. The deterministic suite forces repeated short transfers, a
first-call `EINTR`, delayed synchronization `EIO`, `ENOSPC`, `EDQUOT` and `EROFS`. Hooks move with
their file descriptor and hold no process-global state, so independent fault matrices can run in
parallel.

## Locking

The process lock is a non-blocking exclusive `flock`, held by an open descriptor for the
`DataDirectory` lifetime. A second cooperating process or independently opened instance fails before
reading or mutating storage. `flock` is advisory on every target; OpenBSD and Apple document those
semantics explicitly in their [`flock(2)`](https://man.openbsd.org/flock.2) and
[`flock(2)`](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/flock.2.html)
manuals.

Network and user-space filesystems are unsupported. Linux notes that a failed NFS rename may
already have happened after RPC replay, so the implementation treats every actual `renameat` failure
as indeterminate rather than claiming that the old name won. Do not deploy durable mode on NFS, SMB,
FUSE, overlay or another remote or user-space filesystem.

The [platform durability evidence matrix](platform-durability-evidence.md) separates deterministic
faults (E1), process-kill and reopen tests (E2), controlled device or VM reset (E3) and release
certification (E4). Hosted CI is a regression signal unless an artifact pins and records the actual
filesystem, mount, device/cache class, reset mechanism and recovery result.

## Manifest publication

```text
encode
  -> validate the existing manifest and require a greater generation
  -> remove recognized stale temporary
  -> create private temporary with O_EXCL
  -> complete positional write
  -> full file synchronization
  -> atomic renameat over manifest.glifi
  -> full directory synchronization
```

The result distinguishes three outcomes:

| Outcome | Meaning | Required caller action |
|---|---|---|
| `durable` | rename and directory synchronization completed | publication may become recovery authority |
| `not_published` | failure is known to precede the rename syscall | old manifest remains authoritative; retry may be allowed |
| `indeterminate` | rename was attempted, or a later directory sync failed | poison the instance, stop service, close, and recover |

`renameat` replacement is atomic on the supported local filesystem model: readers never observe a
missing final name between old and new. Atomic visibility is not durable visibility, which is why
directory synchronization is a separate, mandatory step.

The exclusive lock serializes the read-before-publish generation check. An equal or decreasing
Manifest generation fails before the temporary is touched. A malformed existing authoritative
Manifest also fails closed instead of being overwritten as an implicit repair.

Test-only seams inject failures before the write, the file sync, the rename and the directory sync,
and inside the positional I/O and synchronization retry loops. Tests prove that every Manifest,
bootstrap or Segment failure before the rename preserves the prior authority or a pristine
namespace, while a failure after the rename poisons the current instance and requires reopening.
Real block-device disk-full, controller-cache, torn-sector, filesystem and power-loss matrices
remain mandatory before durable certification.

## Related layers

The same primitives back exact-size preallocated Segment creation and alternate-slot mutation;
platform allocation, Segment outcome boundaries, scan complexity and tests are in
[durable Segment files](segment-filesystem.md). Before opening catalog Segments, recovery also
classifies every entry through a fresh descriptor-relative directory stream; the strict filename
grammar, no-follow metadata checks, deterministic reporting, bounds and crash-temporary policy are
in the [data-directory namespace audit](namespace-policy.md).
