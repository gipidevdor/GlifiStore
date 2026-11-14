# Artifact delivery

**Normative** · future tagged releases

How a tagged release travels from one build to a published GitHub prerelease, and which job may
touch which bytes. The decision is [ADR 0038](../adr/0038-stable-c-abi-artifact-delivery.md). The
claim ceiling stays architectural prototype until every linked release gate has retained evidence.

```text
annotated vX.Y.Z tag
        |
        v
Candidate: build once -> identify -> SPDX SBOM -> candidate seal
        |
        v
Verify: exact admission -> test installed bytes -> import evidence/packages
        |          -> verify imported closed set -> compatibility policy
        |          -> release manifest -> SHA256SUMS -> verified seal
        |          -> OIDC/Sigstore attestation of the verified seal
        v
Publish: download -> verify exact seal/checksums/manifest/attestation
        |          -> refuse an existing release
        v
GitHub prerelease: publish the same files, without rebuilding
```

The path is one-way. `.github/workflows/release-candidate.yml` is the only workflow allowed to
compile bytes that can enter the candidate. `.github/workflows/release.yml` orchestrates evidence,
Verify and Publish. Its isolated reproducibility job may compile a second comparison tree, but those
bytes can enter only a diagnostic CI artifact and are never imported into the candidate. Verify may
compile an external consumer against an extracted installed prefix; it must never compile or relink
the engine. Publish must not invoke CMake, a compiler, a linker, a package builder or an archive
builder.

## Source identity

`engineering/tools/release_identity.py` requires all of the following:

- strict `VERSION` and `ABI_VERSION` authorities;
- an annotated `vX.Y.Z` tag matching `VERSION` exactly;
- checkout `HEAD` equal to the tag target;
- no tracked working-tree changes;
- a positive commit timestamp, used as `SOURCE_DATE_EPOCH`.

`scripts/package-source-release.py` feeds that exact tag to `git archive`, normalizes ownership,
orders members, compresses deterministically, and rejects generated, unsafe or missing members. It
never archives the working tree. `scripts/package-install-prefix.py` packages a completed install
tree with normalized metadata and refuses a missing versioned C ABI.

Syft discovers packaged components, but its output is not trusted as the artifact identity.
`release_bundle.py bind-sbom` adds one authoritative GlifiStore SPDX package whose version,
BSD-3-Clause license and SHA-256 identify the adjacent artifact, then normalizes the creation time
from `SOURCE_DATE_EPOCH`. Validation rejects a missing subject, a digest or version mismatch,
duplicate package identities, a missing document-to-package relationship, and any discovered package
for which neither the declared nor the concluded license is resolved. A syntactically valid but
empty or unbound SPDX document therefore cannot pass the candidate gate.

`build-metadata.json` is validated against a closed v1 object model before SBOM generation. Unknown
fields, incomplete nested authorities, tag/version disagreement, invalid commit IDs, empty
toolchain, target or TLS identities, and duplicate or malformed build options fail the candidate.
The JSON Schema under `engineering/schemas/` mirrors that contract for independent tooling.

## Seals and manifests

`candidate-seal.json` is a sorted SHA-256 inventory of every candidate file. Before executing any
candidate bytes, Verify rejects an added, missing, replaced, resized, symlinked or renamed file and
writes `candidate-admission.json`, which binds that exact-set decision to the candidate-seal digest.
The builder also exports the seal's SHA-256 as a workflow output. Every same-run consumer checks
that external anchor before trusting the seal; the reproducibility job repeats the check after its
untrusted comparison build and immediately before reading reference bytes.

Post-build evidence and native packages may be merged only after admission. Later verification
rechecks every admitted member and the admission receipt while allowing those additions; it never
redefines the admitted set. `evidence-import.json` inventories the only permitted additions. Import
rejects protected control names, symlinks, nested or non-regular inputs, duplicate names, collisions
with candidate members and empty sources. Any unrecorded addition or drift in imported bytes stops
verification.

`release-manifest.json` records the product, ABI, wire and persistence identities and the digest and
role of every artifact and evidence file. `SHA256SUMS` covers that manifest. `verified-seal.json`
then covers the complete augmented verification set, including the candidate seal and the admission
receipt. The candidate thus stays immutable while evidence can be added without creating a
seal/evidence cycle.

The Sigstore/in-toto attestation has `verified-seal.json` as its subject: a transitive commitment to
every published payload digest without a manifest/signature cycle. The offline bundle is
`verified-seal.sigstore.json`; verification also pins the repository, signer workflow, source ref,
source digest and OIDC issuer.

## Fail-closed release policy

Publication is prohibited unless the exact candidate contains one source archive, Linux installed
archive, compiled ABI-v1 consumer, compiled wire-v2 client, FreeBSD package and OpenBSD package,
each with an SPDX 2.3 SBOM. It also requires passing, same-commit machine evidence for:

- old/new C ABI compatibility;
- persistence compatibility and recovery;
- wire compatibility;
- installed SDK interoperability;
- the required security matrix;
- native FreeBSD and OpenBSD package and service lifecycles;
- an independent reproducibility comparison.

Each record follows the artifact-bound contract in [retained release evidence](release-evidence.md):
required check IDs, commands and retained non-empty logs are validated in addition to commit,
version and subject digests. `validate-release-policy` enforces these names and bindings.

Installed-SDK, persistence, C ABI, wire, security-matrix, FreeBSD-package, OpenBSD-package and
reproducibility evidence each have a same-run producer:

- Persistence requires a complete Store created by a strictly older tagged release.
- ABI requires the complete attested prior release plus its sealed compiled consumer, and proves
  both dynamic-library directions.
- Wire requires that same prior release plus its sealed compiled wire-v2 client, and proves new/new
  plus both N−1 client/server directions.
- Neither cross-release producer accepts a same-build substitute.
- Reproducibility rebuilds all four deterministic archives on a distinct runner and requires an
  exact size and SHA-256 match; rebuilt bytes cannot replace candidate bytes.
- The security producer executes the complete Linux sanitizer, static, CodeQL and supply-chain
  matrix and inspects the SBOMs and the distributed ELF from the externally anchored candidate.
- Both BSD producers use the native ports and pkg stacks and are intentionally blocked until the
  service accounts are registered.

Prior baselines and retained native packages are still absent, so the release workflow is expected
to stop before attestation and publication; see the [release residuals](wave5-l7-residuals.md).
Removing the gate to cut an early release is forbidden. A deliberate policy change needs an ADR,
requirements, hazards, tests and a residual-risk review.

## Immutability and permissions

Candidate and Verify have `contents: read`; only Verify receives short-lived OIDC and attestation
permissions. Publish alone receives `contents: write` and is protected by the `release`
Environment. It aborts if the GitHub Release already exists, never rebuilds candidate bytes and
never uses asset clobbering (`--clobber`). Different bytes under the same tag name require a new
version; a bad release is withdrawn and replaced, never republished in place.

The reusable workflow is designed to be compatible with a trust boundary, but this repository does
not claim SLSA Build Level 3: that level also requires hosting and pinning the reusable builder in
an appropriately protected trusted repository and retaining successful provenance evidence.
