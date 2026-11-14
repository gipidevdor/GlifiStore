# Python client packaging

How maintainers verify, build and publish `glifistore`, the self-contained Python distribution in
this directory, to PyPI. It has no runtime dependency outside the standard library.

## Requirements

- Python 3.11 or later.
- Build tools: `python -m pip install build twine`.
- Canonical wire fixtures vendored under `tests/fixtures/`, matching the repository
  `tests/fixtures/wire_*_v2.hex`. After changing the repository corpus, refresh them with
  `./scripts/sync-sdk-fixtures.sh`.
- The BSD-3-Clause `LICENSE`, matching the GlifiStore project.

## Verify

From the repository root:

```bash
./scripts/package-python-client.sh
```

The script:

1. confirms that the vendored fixtures match the repository corpus;
2. checks that `glifistore.__version__` in the source tree equals the repository `VERSION`;
3. builds an sdist and a wheel with `python -m build`;
4. runs `twine check` on the artifacts;
5. installs the wheel and the normalized sdist into separate clean temporary environments;
6. runs the complete unittest suite against each installed artifact, with `ResourceWarning` fatal.

With a TLS daemon and the C++ and Go interop peers already built, the wheel can also run through
the fail-closed installed-artifact secure-profile matrix:

```bash
GLIFISTORED=/path/to/glifistored \
GLIFISTORE_INTEROP_CLIENT=/path/to/glifistore_interop_client \
GLIFISTORE_GO_INTEROP=/path/to/glifistore-interop \
./scripts/test-secure-profile-installed-artifacts.sh
```

## Build

```bash
cd sdk/python
python -m build
python -m twine check dist/*
```

The artifacts land in `sdk/python/dist/`:

- `glifistore-0.1.0.tar.gz`, the sdist;
- `glifistore-0.1.0-py3-none-any.whl`, the wheel.

## Publish

Upload to TestPyPI first, and install from it:

```bash
python -m twine upload --repository testpypi dist/*
pip install -i https://test.pypi.org/simple/ --extra-index-url https://pypi.org/simple glifistore
```

Then upload to PyPI:

```bash
python -m twine upload dist/*
```

Use a PyPI API token scoped to the `glifistore` project. Do not commit tokens.

## Bump the version

1. Bump `__version__` in `src/glifistore/__init__.py`, the single source; `pyproject.toml` reads it
   dynamically.
2. Update the root `VERSION` and every other official SDK in lockstep, or land an ADR.
3. Update `CHANGELOG.md`.
4. Run `./scripts/check-sdk-versions.sh` and `./scripts/package-python-client.sh`.
5. Tag, release and upload.
