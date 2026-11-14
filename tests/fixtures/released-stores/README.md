# Tagged durable Store fixtures

This directory is reserved for complete, immutable Stores created by a **previous tagged release**.
Codec vectors under `tests/fixtures/released/` are useful but cannot prove that a new daemon opens
an old Store.

Each version lives at `tests/fixtures/released-stores/<semver>/` and contains:

- `STORE-FIXTURE.json`: the exact tag, commit, producer artifact digest, Worker count and a probe
  key and value;
- `SHA256SUMS`: a closed inventory of the metadata and every Store file;
- `store/`: the stopped durable Store, including `manifest.glifi`.

## Create a fixture

Create a drop only from the installed binaries of the tagged release, after writing and reading the
probe through that release. Then package the stopped Store:

```bash
python3 engineering/tools/persistence_fixture.py create \
  --source /path/to/stopped-store \
  --output tests/fixtures/released-stores/0.1.0 \
  --product-version 0.1.0 --tag v0.1.0 \
  --git-sha <40-hex-tag-commit> \
  --producer-artifact /path/to/glifistore-0.1.0-linux-<arch>.tar.xz \
  --worker-count 1 --key-hex <hex> --value-hex <hex> \
  --packaged-at 2026-08-26T00:00:00Z --repository .
```

The tool verifies that the tag is annotated, resolves to `git_sha` and contains the same `VERSION`.

## Selection

The release workflow selects the newest valid fixture strictly older than the candidate. A self
fixture, an untagged directory, a symlink, an incomplete checksum inventory or a same-version
fixture fails closed.

No tagged Store fixture exists yet. Persistent cross-release evidence therefore stays deliberately
blocked until the first baseline has been retained.
