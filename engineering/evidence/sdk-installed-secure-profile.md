# Installed SDK secure-profile evidence

Status: evidence contract; the repository does not retain a successful run by itself.

The existing `sdk-clients` job in `.github/workflows/ci.yml` executes the installed-artifact
secure-profile matrix for C++, Python, Perl, Ruby, Go and Erlang. On pushes to `main`, the same job
uploads one artifact named `sdk-installed-secure-COMMIT` with 30-day retention. Pull requests run
the proof but do not upload a duplicate bundle.

The bundle contains:

- `run.log`: complete combined output from package installation, external builds and interop;
- `manifest.env`: result, exact commit, project/wire versions, runner identity and covered profile;
- `package-SHA256SUMS`: checksums produced before installing the SDK artifacts;
- `sdk-release-index.json`: exact client/role mapping for those checksummed artifacts;
- `toolchains.txt`: compiler and language runtime identities.

Failed `main` runs upload the same shape with `result=failed` before the test step returns failure.
An uploaded artifact is retained CI evidence, not a registry-publication record, a cross-version
compatibility proof or grounds to raise the project beyond its declared architectural-prototype
status. Permanent release claims still require an explicitly curated drop under this directory or
another evidence path accepted by the corresponding gate.
