# Retained release evidence

**Normative** · release promotion

What a retained evidence record must contain before it can promote a release, and what each of the
eight required records proves.

Schema: `engineering/schemas/release-evidence.schema.json`. Validator:
`engineering/tools/release_evidence.py`. Transport receipts use
`engineering/schemas/candidate-admission.schema.json` and
`engineering/schemas/evidence-import.schema.json`; their executable authority is
`engineering/tools/release_bundle.py` (`admit-candidate`, `import-evidence` and
`validate-evidence-import`).

## Record contract

A filename ending in `-evidence.json` is not proof by itself. Every promoted evidence record must
bind one exact candidate seal or native package digest to the release commit and product version.
It records the CI workflow and run identity, the native OS and architecture, a UTC generation time,
every mandatory check, the command used for that check and a non-empty retained log file. Unknown
fields, local or unattested producers, missing checks, failed checks, missing logs and subject
digest drift are rejected.

Log files and evidence records enter `release-manifest.json`, `SHA256SUMS` and the verified seal,
so the final attestation binds both the claimed result and its retained diagnostic material. They
are accepted only after `candidate-admission.json` has recorded an exact-set verification of the
original candidate. Adding evidence before admission fails closed; adding it afterwards cannot alter
or remove any candidate member, and the verified seal covers the whole augmented set.

The validator proves structural completeness and identity; the trusted, SHA-pinned workflow remains
responsible for actually executing the recorded commands. Manually fabricating a log or evidence
record is a supply-chain incident, not an accepted waiver mechanism.

## Required records

The release policy requires eight records:

| Record | Required coverage | Subject |
| --- | --- | --- |
| `abi-compatibility-evidence.json` | symbols, layout, old/new binary directions | candidate seal |
| `persistent-compatibility-evidence.json` | old fixture open/recovery and operator tools | candidate seal |
| `wire-compatibility-evidence.json` | new/new and both N−1 directions | candidate seal |
| `sdk-installed-interop-evidence.json` | source isolation plus C, C++ and six SDK rows | candidate seal |
| `security-matrix-evidence.json` | sanitizers, analysis, scans, licenses, hardening | candidate seal |
| `freebsd-package-evidence.json` | native package/service/install/recovery lifecycle | FreeBSD package |
| `openbsd-package-evidence.json` | native package/service/install/recovery lifecycle | OpenBSD package |
| `reproducibility-evidence.json` | independent rebuild and artifact comparison | candidate seal |

## Producers

### C ABI

`abi-compatibility-evidence` refuses a same-build substitute. The candidate builder packages its
compiled C ABI consumer as a deterministic sealed asset. A later tag selects the newest strictly
older annotated release with the same ABI major, downloads its complete immutable asset set,
revalidates its verified seal, checksums, release manifest and release-wide policy, and verifies the
retained Sigstore bundle. It then checks the candidate symbol allowlist and layout, executes the
retained old consumer with the candidate library, and executes a newly compiled consumer with the
prior official library. Both binary directions record the actual loader resolution. The producer is
implemented but cannot pass before the first complete official release exists.

### Persistence

`persistent-compatibility-evidence` verifies the same candidate seal, extracts its installed Linux
prefix and builds only a protocol probe through the installed C++ package. The daemon and the
verify, migrate, backup and repair tools always come from the candidate archive. Four isolated runs
prove open/GET, two-cycle recovery, offline Worker migration, and the backup–restore–orphan-repair
chain against the newest valid complete Store strictly older than the candidate.
`engineering/tools/persistence_fixture.py` enforces tagged provenance, exact metadata, an annotated
tag resolving to the recorded commit and `VERSION`, safe regular paths and a closed SHA-256
inventory. `tests/fixtures/released-stores/` has no prior tagged Store yet, so this producer
intentionally blocks; it cannot substitute codec vectors or a candidate-created self Store for
cross-release proof.

### Wire

`wire-compatibility-evidence` applies the same artifact discipline to protocol v2. The candidate
builder packages the compiled reference client with a closed metadata object, protocol version,
release identity, executable digest, deterministic archive metadata and an adjacent bound SBOM. The
release job extracts both clients and both daemons only from the candidate and the verified,
attested prior release. It then executes PUT→GET, ERASE and NOT_FOUND in three isolated directions:
new client/new server, old client/new server and new client/old server. This is the plain-TCP v2
baseline; it does not claim TLS-profile or cross-wire-version compatibility. The producer is
implemented but remains blocked until a complete prior official release supplies the retained old
client and server bytes.

### Installed SDKs

The tag-only release graph contains a same-run `sdk-installed-evidence` job. It extracts the sealed
Linux prefix without rebuilding the engine, compiles C and C++ consumers solely through its
installed CMake and pkg-config metadata, packages every language SDK, rejects source-tree module
loads, and exercises C++, Python, Perl, Go, Ruby and Erlang against the distributed daemon. The job
emits `sdk-installed-interop-evidence.json` through the validated evidence writer and uploads it
under the `release-input-<sha>-*` namespace that the closed importer consumes. It is implemented but
not yet retained CI evidence: it becomes proof only after a successful tagged run.

### Security matrix

`security-matrix-evidence` is assembled only after four independent same-tag jobs succeed. The
matrix executes the complete CTest graph under ASan+UBSan and TSan; produces and validates retained
CodeQL SARIF for C/C++, Python, Go and GitHub Actions; runs clang-tidy, formatting, the strict
warnings-as-errors build, Trivy, Gitleaks and dependency-license closure; and then revalidates every
candidate SPDX document plus the hardening properties of the exact distributed Linux ELF. The
aggregator verifies the externally exported candidate-seal digest before reading candidate bytes,
requires one retained success log for every mandatory check, and emits the evidence record only when
the closed matrix is complete. The producer is implemented, but it is not retained CI evidence until
a tagged run succeeds. Its subject is the Linux candidate; it does not establish native
FreeBSD/OpenBSD hardening.

### FreeBSD and OpenBSD packages

Native FreeBSD/OpenBSD CI retains installed-prefix ABI logs. Those are portability and installation
signals, not the package lifecycle records above. The tag graph has native FreeBSD and OpenBSD
package producers; both fail closed until the ports account registration markers exist and a tagged
native run retains evidence. Promotion remains blocked until ports account registration, native
package creation, service lifecycle, restart recovery, configuration preservation and uninstall
have all succeeded on both native OSes. The native jobs generate `distinfo` from the already sealed
source archive, outside that archive; committing its self-digest into the source would be circular.

`freebsd-package-evidence` first checks the external candidate-seal anchor and requires the service
account to be present in the actual FreeBSD ports `UIDs` and `GIDs` authorities. A FreeBSD 14.3 VM
generates `distinfo` from the sealed source archive, runs checksum/stage/check-plist/package,
installs the resulting `.pkg` on a clean host, verifies its file and C-ABI inventory, and exercises
rc.subr start/stop, protocol-v2 PUT/GET/ERASE, durable restart recovery, forced reinstall with
modified configuration, and uninstall with configuration/data preservation. The native package is
the evidence subject; its retained `distinfo`, logs, evidence record and bound SPDX SBOM enter the
closed importer. The producer is implemented but intentionally fails before VM execution until the
upstream account-registration marker exists, and no native tagged result has been retained yet.

`openbsd-package-evidence` mirrors that discipline on OpenBSD 7.9. It requires
`packaging/openbsd/PORTS_ACCOUNT_REGISTERED`, copies the reference port into
`$PORTSDIR/databases/glifistore`, generates non-circular `distinfo`, builds with `DISTDIR` and
`PACKAGE_REPOSITORY`, then `pkg_add`s the `.tgz`, checks ABI symbols on
`libglifistore.so.${ABI_VERSION}`, and exercises `rcctl` enable/start/stop, PUT/GET/ERASE, Store
verify under `/var/glifistore`, restart recovery, config preservation and `pkg_delete`. The same
fail-closed residual applies: no account marker and no retained tagged run yet.

### Reproducibility

`reproducibility-evidence` runs on a distinct runner after the candidate seal exists. It checks out
the exact annotated tag, repeats the declared release configuration and toolchain, installs into the
same normalized layout, rebuilds the compiled ABI and wire fixtures, and regenerates exactly four
archives: source, Linux prefix, ABI consumer and wire client. The comparison rejects a missing or
extra archive and requires equal byte size and SHA-256 for every member of that closed set. The
rebuilt archives are retained as a diagnostic CI artifact but are excluded from candidate import and
promotion. The current scope is the same `ubuntu-latest` image family and declared compiler; it is
not a cross-distribution or cross-compiler reproducibility claim.
