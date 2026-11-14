# Version lifecycle

**Normative** · persistence v1, wire v2, C ABI v1, public C++ API before 1.0 · reviewed 2026-08-01

The upgrade, downgrade, Worker-count, API, ABI and release-artifact policy that GlifiStore publishes
before and at alpha. It is normative for 0.x promises and descriptive for future 1.0 support
windows.

This page adds operator-facing promises to the development-only
[format compatibility matrix](format-compatibility.md). The decision is
[ADR 0024](../adr/0024-offline-worker-migration.md). The machine-readable N↔N-1 rows are in the
[N↔N-1 compatibility matrix](n-n1-compatibility.md) and
[`engineering/compatibility/n-n1-matrix.yaml`](../../engineering/compatibility/n-n1-matrix.yaml);
the operator procedures are in
[compatibility and migration](../operations/compatibility-and-migration.md).

## Release levels

| Level | Disk / wire | C ABI | Embedded C++ API / ABI | Worker count change |
|---|---|---|---|---|
| Prototype | No release claim | ABI 1 contract may be implemented; evidence gates still apply | Unstable / no ABI promise | Unsupported |
| Alpha (0.x durable) | Persistence v1 reopen within declared codec rows; wire v2 exact | ABI major 1 symbols/layout/semantics preserved after first publication | Source may break across minors; no C++ ABI promise | Offline migrate only ([store migration](store-migration.md)) |
| Beta | Same plus tested upgrade/downgrade notes per release | Old-binary/new-library matrix required | Feature-complete contracts; C++ ABI still not promised | Offline migrate; online still out of scope |
| RC / Stable | Supported upgrade paths and support lifetime | Declared ABI-major support window | SemVer API; C++ ABI only if separately declared | Separate project for online reshard |

## Persistence v1 upgrade and downgrade

### Upgrade (newer binary, existing Store)

1. Stop every writer that holds the data-directory lock.
2. Run `glifistore_verify_store` on the data directory.
3. Open with the newer binary using the **same** Worker count: omit the override or pass the
   persisted count. Reopen only reopens; it never rewrites silently.
4. If the newer binary requires a higher format version than the Store encodes, open fails closed.

### Downgrade (older binary, existing Store)

1. An older binary may reopen a Store only when it implements every required Manifest, Segment,
   header, commit-slot and Record version.
2. A correctly checksummed newer required version is rejected, fail-closed. Synthetic coverage: a
   checksum-valid Manifest format v2 and a future Record version pinned in the Manifest are refused
   by `verify_durable_store_path`, `restore_durable_store` and `Store::open`
   (`tests/unit/store_backup_tests.cpp`, HAZ-022 / `STORE-FUTURE-REQUIRED`).
3. For pure persistence v1 (no format bump), a byte-identical
   [backup and restore](../spec/backup-restore-v1.md) followed by an open with the older binary is
   sufficient when the older binary still implements v1.

### What reopen never does

- Change the Worker count or routing algorithm
- Rewrite the Manifest or Segments to a new format version
- Adopt unlisted namespace files
- Repair corruption in place

## Worker count and resharding

| Operation | Supported? |
|---|---|
| Create Store with chosen Worker count | Yes (creation-time only) |
| Reopen with matching count | Yes |
| Reopen with different count | No — fail closed |
| Change count via backup/restore | No — restore preserves catalog bytes |
| Change count via offline migrate | Yes — `glifistore_migrate_store` |
| Online / live reshard | No (deferred; design constraints in [ADR 0033](../adr/0033-online-rebalance-deferred.md)) |

The mechanism is [store migration](store-migration.md); the operator procedure is
[change the Worker count](../operations/compatibility-and-migration.md#change-the-worker-count).

## Public APIs and ABIs

Before 1.0 the C++ ABI is not stable, and consumers rebuild. Patch releases preserve documented
source behavior; minor releases may break source, with changelog notes. Disk and wire formats
follow their encoded versions.

The C facade is versioned separately by `ABI_VERSION`. ABI major 1 is governed by ADR 0038 and
[C ABI v1](../spec/c-abi-v1.md); product 0.x does not cancel that ABI, and the ABI is never
inferred from it. Until a complete prior official ABI-1 release is retained, same-build tests and
the implemented producer establish only the mechanism, and no release note may claim demonstrated
N−1 binary compatibility.

## Released-artifact evidence

| Layout | Role |
|---|---|
| `tests/fixtures/*.hex` | Current-tree canonical codecs |
| `tests/fixtures/released/<label>/` | Optional dropped fixture trees from a tag or packaging script |
| `scripts/package-release-compatibility-artifacts.sh` | Packages current fixtures for a release label |
| `tests/fixtures/released-stores/<semver>/` | Complete stopped Store from a prior tagged release |
| `engineering/tools/persistence_fixture.py` | Creates, validates, and strictly selects complete Store drops |
| `glifistore-abi-v<major>-consumer-<version>-linux-<arch>.tar.xz` | Sealed compiled consumer used by the next ABI release matrix |
| `engineering/tools/prior_release.py` | Validates a complete official prior release before cross-version use |
| `scripts/package-release-claim.sh` | Writes `engineering/claims/<tag>.yaml` for a version tag |
| `engineering/claims/` | Claim ceiling + gate/evidence pointers per tag |

CI packages and decodes self and tag artifacts on every push, pull request and tag, and packages a
claim YAML on version tags. Permanent cross-release binary evidence still depends on tagged trees
being committed into `tests/fixtures/released/`, which is a release-process step.

## Non-goals for persistence v1

- Online Worker resharding or dual-ownership routing tables
- In-place Worker-count rewrite on open
- C++ ABI guarantees before 1.0 (the independent C ABI v1 is the supported binary boundary)
- Automatic downgrade of future format versions
- Preserving sequence numbers or Store identity across migrate
