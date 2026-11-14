# Data-directory namespace audit

**Descriptive** · persistence v1 data directory, normal recovery

The read-only, Manifest-relative audit that recovery runs over the data directory after decoding the
authoritative Manifest and before opening any Segment, and the policy it applies to each entry.

The audit never adopts, renames, quarantines, deletes or rewrites an entry, so it cannot turn an
attacker-controlled or crash-left file into Store authority. Ordinary recovery removes only
recognized private engine temporaries, and only after the complete authoritative scan
([ADR 0039](../adr/0039-pre-intent-staged-compaction-output.md)); every other anomaly is rejected
and left in place for an operator.

## Descriptor-relative enumeration

Enumeration stays anchored to the already validated, exclusively locked `DataDirectory`. Each audit
obtains a new open file description with `openat(directory_fd, ".", ...)`, transfers that descriptor
to `fdopendir`, and uses its `dirfd` for metadata checks. A plain `dup` is deliberately not used:
duplicated descriptors share the directory offset, so a second audit could begin at the first
audit's end.

The `readdir` result supplies only the name. The implementation ignores `.` and `..` by name, does
not depend on the non-portable `d_type`, resets `errno` before each read so that EOF is
distinguishable from an error, and validates recognized objects with
`fstatat(..., AT_SYMLINK_NOFOLLOW)`. A recognized object is safe only when it is a regular file,
owned by the effective user, singly linked and inaccessible to group and other users. Recovery later
reopens every catalog Segment with `O_NOFOLLOW` and validates its encoded identity, so the filename
never replaces on-disk metadata authority.

These choices follow the POSIX descriptor-relative directory interfaces and the platform manuals:
[Linux `opendir`/`fdopendir`](https://man7.org/linux/man-pages/man3/opendir.3.html),
[Linux `readdir`](https://man7.org/linux/man-pages/man3/readdir.3.html),
[Apple directory operations](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/dirfd.3.html),
[FreeBSD `fdopendir`](https://man.freebsd.org/cgi/man.cgi?query=fdopendir&sektion=3) and
[OpenBSD directory operations](https://man.openbsd.org/readdir.3). The no-follow metadata behavior
is also explicit in [OpenBSD `fstatat(2)`](https://man.openbsd.org/fstatat.2).

## Canonical names and classifications

Segment filenames accept exactly lowercase hexadecimal, fixed widths, non-zero IDs and these two
forms:

```text
segment-<16 hex Segment ID>-<8 hex generation>.glifi
.segment-<16 hex Segment ID>-<8 hex generation>.glifi.tmp
```

Uppercase digits, width changes, trailing bytes, zero identities and near-matches are malformed;
there is no permissive normalization. Final Segment membership requires both the parsed ID and the
parsed generation to match one exact Manifest entry.

| Classification | Meaning | Normal recovery policy |
|---|---|---|
| expected | lock, manifest, or exact manifest-listed final Segment with safe metadata | continue |
| stale manifest temporary | exact private `.manifest.glifi.tmp` | report and continue; removed after the complete scan |
| stale Segment temporary | exact private canonical Segment temporary | report and continue; removed after the complete scan |
| stale compaction temporary | exact private `.glifistore.compaction.tmp` | report and continue; removed after the complete scan |
| compaction intent | exact private `.glifistore.compaction` | reject; admitted only by compaction-intent recovery, with the exact obsolete names of its transaction |
| unlisted Segment | canonical private final Segment absent from the manifest | reject without opening or adopting |
| malformed engine name | near-match to an engine-owned namespace | reject |
| unknown entry | any other name | reject |
| unsafe entry | recognized name that is a symlink, hard-linked, non-regular, foreign-owned, or non-private | reject |
| missing catalog Segment | manifest entry with no exact final name | reject as corruption |
| missing required entry | lock or authoritative manifest vanished during audit | reject as corruption |

Recognized temporaries are the only non-clean state that normal recovery accepts. They cannot be
authority. After every Worker scan succeeds, ordinary recovery validates each as a private regular
file, removes it, synchronizes the directory once and audits the namespace again
([ADR 0039](../adr/0039-pre-intent-staged-compaction-output.md)).

An unlisted Segment may be a valid crash orphan, but normal recovery does not infer that from its
name and does not advance IDs around it. A future explicit in-place quarantine or repair operation
must first validate the encoded header, reserve the identity safely, synchronize every namespace
mutation and keep an operator-visible audit trail. The offline
[`glifistore_repair_store`](../cli.md#glifistore_repair_store) works differently: it quarantines
into a separate empty workspace and never mutates the source directory.

## Determinism and bounds

The Manifest decoder caps the catalog at 1,000,000 entries. Namespace enumeration also caps non-dot
entries at `manifest Segment count + 4 reserved entries + 4096 anomalies`; exceeding that bound
fails before an unbounded report is built. The issue vector has a separate absolute bound of
`1,000,000 + 4096`, and Manifest presence uses one bit per catalog Segment.

Directory order never affects diagnostics: issues are sorted by complete binary filename, then by
classification. Catalog lookup uses binary search over the already canonical, strictly ordered
Manifest, which avoids a second million-entry hash table. For `N` directory entries, `M` Manifest
Segments and `A` anomalies, the audit costs `O(N log M + A log A)` time and `O(M bits + A)`
auxiliary space. This favors bounded, deterministic memory on a failure path; the fixed
39/44-byte name grammar keeps parsing linear in a tiny constant.

The exclusive lock serializes cooperating GlifiStore processes. As elsewhere in the filesystem
layer, it cannot stop a same-user process that deliberately ignores the advisory protocol from
mutating the directory concurrently, so the later no-follow opens and identity validation stay
mandatory.

## Tests

Unit tests cover exact parsing, repeated enumeration, deterministic classification, canonical crash
temporaries, unlisted files, malformed and unknown names, symlinks, hard links, missing catalog
files and the enumeration bound. Recovery integration proves that recovery removes recognized
temporaries and that an unlisted Segment blocks recovery without adoption or deletion.

Runtime durable Store materialization follows the strict normal-open policy above. Related release
gates: operator quarantine and repair tooling; native FreeBSD and OpenBSD evidence; expanded
process-kill and disk-full matrices; documented handling for every quarantined namespace state.
