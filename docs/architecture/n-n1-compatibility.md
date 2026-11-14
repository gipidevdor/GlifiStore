# N↔N-1 compatibility matrix

**Normative** · Store open, wire server and client, ABI promises across N↔N-1 · reviewed 2026-08-01

Machine-readable authority:
[`engineering/compatibility/n-n1-matrix.yaml`](../../engineering/compatibility/n-n1-matrix.yaml).
Validator: `python3 engineering/tools/validate_compat_matrix.py`.

What GlifiStore supports, intentionally rejects or does not promise when a writer or server at
version N meets a reader or client at N−1, or the reverse. The rows are normative for 0.1.x policy
and descriptive for future releases; the matrix does not raise the claim ceiling above
architectural prototype.

## Store open (durable directory)

| ID | Writer | Reader | Status |
| --- | --- | --- | --- |
| `STORE-SAME-LINE` | 0.1.x | 0.1.x | supported (same Worker count, persistence v1) |
| `STORE-WORKER-RESHARD` | 0.1.x | 0.1.x | supported offline only (`glifistore_migrate_store`) |
| `STORE-PRE-V1` | pre-v1 | 0.1.x | unsupported |
| `STORE-FUTURE-REQUIRED` | future required version | 0.1.x | intentionally rejected (fail closed) |
| `STORE-DOWNGRADE-REWRITE` | 0.1.x | older required | not promised |

## Wire (server ↔ client)

| ID | Pair | Status |
| --- | --- | --- |
| `WIRE-V2-SAME` | 0.1.x ↔ 0.1.x on wire v2 | supported |
| `WIRE-V1-OR-UNKNOWN` | non-v2 client/server | intentionally rejected |
| `WIRE-CROSS-MAJOR-FUTURE` | future major ↔ 0.1.x | not promised |

Each release candidate keeps its compiled reference wire v2 client as a deterministic sealed asset.
On a later tag, the release producer revalidates the complete prior release and its Sigstore
attestation, then runs new/new, old-client/new-server and new-client/old-server operations without
rebuilding either daemon. The mechanism is implemented, but no N−1 wire evidence exists until a
complete prior release exists; a same-build local run proves only the harness.

## ABI

The installed C++ ABI is not promised before product 1.0 (`ABI-CPP-0X-NO-STABLE`); on 0.x the CMake
package uses `SameMinorVersion`.

The shared C facade has its own version line. `ABI-C1-CURRENT` supports an ABI-1 consumer against
the same ABI-1 build. `ABI-C1-N-MINUS-1` stays `not_promised`, as an evidence statement, until a
complete prior official release exists. Candidates package a deterministic compiled consumer, and
the same-run producer revalidates the old seal, release policy, annotated tag and Sigstore
attestation before proving both dynamic-library directions. That implemented mechanism is not
cross-release proof, which cannot exist yet, and the matrix does not count it as such.

## Released fixtures

In-tree `tests/fixtures/released/self-v1/` and CI packaging through
[`release-compat.yml`](../../.github/workflows/release-compat.yml) prove the harness. Committing a
prior tagged N−1 tree into `tests/fixtures/released/<label>/` is a release-process step once tags
exist; until then the YAML matrix is the policy record. Schema-2 drops use a flat, validated label
and bind their exact persistence and wire versions, fixture count and SHA-256 manifest. The
packaging script never overwrites an existing label.

Complete durable Stores use the separate `tests/fixtures/released-stores/<semver>/` contract. Each
drop binds tag, commit, producer artifact digest, Worker count, probe bytes and every Store file in
a closed checksum inventory. The release producer selects only the newest valid version strictly
older than the candidate, then runs the installed candidate daemon and offline tools without
rebuilding the engine. The directory stays empty until the first tag can become a genuine prior
baseline; `self-v1` codec vectors never satisfy this gate.
