# Platform durability evidence (scaffolding)

**No platform/filesystem row here is E3 or E4 certified.** These directories are
reserved evidence-path placeholders for platform durability work. Fill them only
with retained, reviewed campaign artifacts that name the exact OS/kernel,
filesystem, mount options, and guest/host boundary.

| Row | Path | Highest honest claim today |
| --- | --- | --- |
| macOS / APFS | [`macos-apfs/`](macos-apfs/) | E2 when collector artifacts exist; E3/E4 open |
| Linux / ext4 | [`linux-ext4/`](linux-ext4/) | E2 when collector artifacts exist; E3/E4 open |
| Linux / XFS | [`linux-xfs/`](linux-xfs/) | E1 portability; E2/E3/E4 open |
| FreeBSD / UFS | [`freebsd-ufs/`](freebsd-ufs/) | Portability signal; UFS certification open |
| FreeBSD / ZFS | [`freebsd-zfs/`](freebsd-zfs/) | Architectural target; certification open |
| OpenBSD / FFS | [`openbsd-ffs/`](openbsd-ffs/) | Portability signal; FFS certification open |

Evidence matrix (roadmap):
[`docs/architecture/platform-durability-evidence.md`](../../../docs/architecture/platform-durability-evidence.md).
Harness rehearsal (`scripts/run-e3-block-reset.sh`) always emits `e3_certified=no`.
