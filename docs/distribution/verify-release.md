# Release verification

**Descriptive** · future tagged releases

How to check a downloaded GlifiStore release before installing it. The commands describe the future
verified release layout; they do not imply that a complete release exists yet.

Download all assets into one directory, then verify the sorted checksum manifest:

```sh
sha256sum -c SHA256SUMS
```

Verify the transitive file seal, the manifest, the release policy and the SBOMs with the repository
tool of the matching version:

```sh
python3 engineering/tools/release_bundle.py verify-seal \
  --directory . --seal verified-seal.json
python3 engineering/tools/release_bundle.py validate-manifest --directory .
python3 engineering/tools/release_bundle.py validate-release-policy --directory .
python3 engineering/tools/release_bundle.py validate-sbom ./*.spdx.json
```

Verify provenance online while constraining the repository and the signer:

```sh
gh attestation verify verified-seal.json \
  --repo gipidevdor/GlifiStore \
  --bundle verified-seal.sigstore.json \
  --signer-workflow https://github.com/gipidevdor/GlifiStore/.github/workflows/release.yml
```

For offline verification, first obtain `trusted_root.jsonl` through a separately trusted channel,
then add `--custom-trusted-root trusted_root.jsonl`.

The bundle proves the seal, and the seal proves the exact digest and size of every manifest,
checksum and payload file. Inspect `build-metadata.json` for the tag, commit, toolchain, target, TLS
backend, build options, and ABI, wire and persistence versions. Each SPDX document must describe
exactly one GlifiStore root package whose SHA-256 matches the adjacent artifact; it is not enough
for the JSON to parse or merely enumerate dependencies.

Do not trust an asset name alone. Do not install if any checksum, seal, policy, identity, SBOM or
attestation check fails.
