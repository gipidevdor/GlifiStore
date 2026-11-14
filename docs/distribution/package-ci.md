# Package CI operator guide

**Descriptive** · packaging framework, `scripts/package-ci.sh` and `package-ci.yml`

How to run the packaging framework, read the evidence it writes, bump a package revision and add a
backend. The framework is implemented and locally exercised, but **no packaging gate is closed and
no packaging evidence has ever been retained from a tagged release**.

This page is the operator half of the framework. The declarative half (which backends exist, which
targets and checks they owe, how deep each CI profile may go) is generated into
[package backend status](package-status.md) from
[`engineering/distribution/package-matrix.yaml`](../../engineering/distribution/package-matrix.yaml).
Neither page is evidence: the only packaging proofs are the retained `*-package-evidence.json`
documents described below.

## The pipeline

| Step | Owner | Output |
| --- | --- | --- |
| Resolve version identity | [`generate_release_context.py`](../../engineering/tools/generate_release_context.py) from [`VERSION`](../../VERSION) and [`ABI_VERSION`](../../ABI_VERSION) | `release-context.json` |
| Expand the backend matrix | [`generate_package_matrix.py`](../../engineering/tools/generate_package_matrix.py) | `package-matrix.json` |
| Run one backend lifecycle | [`scripts/package-ci.sh`](../../scripts/package-ci.sh) → `scripts/lib/package-backend-{linux,macos,bsd}.sh` | `check-plan.json`, per-check logs, `<backend>-<profile>-<stage>-package-evidence.json` |
| Refuse dishonest evidence | [`validate_package_evidence.py`](../../engineering/tools/validate_package_evidence.py) | non-zero exit |
| Decide the CI profile and close it | [`package_ci_plan.py`](../../engineering/tools/package_ci_plan.py) | `package-ci-plan.json`, `package-ci-closure.json` |

No version, backend, runner, timeout or retention appears in
[`package-ci.yml`](../../.github/workflows/package-ci.yml): the workflow calls the two tools above
and uploads what they produce.

## Run it locally

It needs `python3` with `PyYAML` and `jsonschema` and a git checkout (the context reads the commit);
the structural rows need nothing else.

```bash
python3 -m pip install --user PyYAML jsonschema

# One backend, pull-request depth.
./scripts/package-ci.sh --profile pr --backend deb

# Every backend the profile declares.
./scripts/package-ci.sh --profile main --all

# One lifecycle stage only (the thin wrappers share the same arguments).
./scripts/package-build.sh --profile main --backend rpm
./scripts/package-verify.sh --profile main --backend rpm
```

Output lands under `build/package-ci/<version>/<commit>/<profile>/<run-id>/<backend>/<stage>/`, one
isolated directory per run. The exit status is non-zero only when a backend reports `FAIL` or a tool
refuses. A run made entirely of `BLOCKED` rows succeeds and says so in the evidence, because "this
host cannot run that" is a fact, not a failure.

Depth is opt-in by design:

| Environment | Effect |
| --- | --- |
| default | structural rows only: release context, matrix, rendered packaging metadata, reference-port and account-marker reads |
| `GLIFISTORE_PACKAGE_CI_CONTAINER=1` | `deb`/`rpm` build, install, service and removal inside the digest-pinned container |
| `GLIFISTORE_PACKAGE_CI_NATIVE=1` | the same rows on a disposable root host; on macOS, the MacPorts/Homebrew native lifecycle |
| `--candidate DIR` | admit a sealed release-candidate directory; the native lifecycles refuse to build without it |

`GLIFISTORE_PACKAGE_CI_NATIVE=1` installs into the host package manager and creates system accounts
and services. Use a disposable machine or VM, never a workstation.

The FreeBSD and OpenBSD native lifecycles also require a native host, root, a complete ports tree,
the sealed candidate and the `PORTS_ACCOUNT_REGISTERED` marker. That marker records a real upstream
UID/GID allocation; neither CI nor this guide ever creates it
([BSD reference packaging](bsd-packaging.md)).

## Read the evidence

Each run writes one evidence document per backend and stage:

```bash
run=build/package-ci/0.1.0/<commit>/pr/<run-id>
python3 engineering/tools/validate_package_evidence.py validate \
  "$run/deb/full/deb-pr-full-package-evidence.json" \
  --release-context "$run/release-context.json" \
  --backend deb --profile pr --artifact-root "$run/deb/full"
```

Read it in this order:

1. `result`: the backend outcome, which can never be better than its checks.
2. `lifecycle_state`: how far up the chain this run actually got (`NONE` … `RELEASE_ADMITTED`).
3. `subject`: `source_tree` means no artifact bytes exist; `artifact` carries the SHA-256 that a
   later admission has to match.
4. `checks[]`: each check with its `status`, `category` and the retained log in `evidence_ref`.
5. `limitations` and `residuals`: what this document deliberately does not prove.
6. `producer`: `workflow: local-unattested` / `run_id: local` marks a local run, which the release
   profile refuses to cite.

Statuses are not shades of pass:

| Status | Meaning | What it is not |
| --- | --- | --- |
| `PASS` | the check ran and succeeded, with a retained non-empty log | never inferred from a sibling check |
| `FAIL` | the check ran and failed | fails the run |
| `NOT_RUN` | the check could have run here and did not | not a pass, not a waiver |
| `NOT_APPLICABLE` | the check cannot apply to this backend | must be justified by the backend |
| `NOT_APPLICABLE_INITIAL_BASELINE` | no release precedes this version, so there is nothing to upgrade from | becomes `NOT_RUN` the moment a predecessor exists |
| `BLOCKED` | a prerequisite is missing: no container runtime, no native host, no sealed candidate, no account marker | not a defect and not a pass |
| `OPEN_GATE` | the capability is unproven by construction (upstream acceptance, a service the packaging does not install) | never closes by rerunning CI |

A `category` bounds how a passing row may be read: a `structural` pass says the repository-side
metadata is coherent, and nothing about a built package, a running service or an upstream
acceptance.

## Bump a package revision without a new product version

`VERSION` is the only source of truth for the product version. Packaging-only changes (a corrected
dependency, a service unit fix) use the package revision instead:

```bash
./scripts/package-ci.sh --profile main --backend deb --package-revision 1
```

The revision flows through `release-context.json` into every backend naming convention (`0.1.0-2`
for `deb`/`rpm`, `0.1.0_1` for `macports`, `PORTREVISION` for FreeBSD), and never into `VERSION`,
the ABI or the wire protocol. Changing the packaged upstream bytes is a product-version change, not
a revision bump: a release artifact that differs by a single byte needs a new version, because
publication refuses to clobber an existing tag or asset.

## Add a backend

1. Add the backend id to `BACKENDS` in
   [`package_framework.py`](../../engineering/tools/package_framework.py) and its package kind to
   `ARTIFACT_KINDS`. An id outside that list is refused everywhere.
2. Describe it in [`package-matrix.yaml`](../../engineering/distribution/package-matrix.yaml):
   `status: PLANNED` or `STRUCTURAL`, `required_for_release: false`, its targets (a container
   target must be pinned by digest), the checks it owes, the required checks per profile, and its
   honest `limitations`.
3. Write the adapter under `scripts/lib/` and its driver under `engineering/tools/`. The adapter
   decides statuses only from what it observed, records a log per check, and emits evidence through
   `validate_package_evidence.py emit`. It must never synthesise an account marker, an upstream
   acceptance or a `PASS` for a check it skipped.
4. Add negative tests under [`scripts/tests/`](../../scripts/tests): a refused matrix, a refused
   evidence document, and the lifecycle statuses on a host without the prerequisites.
5. Regenerate the derived view and re-run the validators:

   ```bash
   python3 engineering/tools/generate_package_matrix.py validate
   python3 engineering/tools/generate_package_status.py --write-generated
   python3 -m unittest discover -s scripts/tests
   python3 engineering/tools/validate_assurance.py
   python3 engineering/tools/validate_documentation.py
   ```

6. Leave `required_for_release: false`. Promotion needs `LIFECYCLE_VERIFIED` evidence retained from
   CI, a release-policy artifact in
   [`release_bundle.py`](../../engineering/tools/release_bundle.py), an ADR and a gate update, in
   that order; never a silent flip of the flag.

Windows and Apple `.pkg` are refused by the matrix validator, not merely unimplemented. Both need an
accepted ADR first ([package backend status](package-status.md) lists the prerequisites).

## The release path

[`release.yml`](../../.github/workflows/release.yml) builds the candidate once, seals it, and only
then lets packaging run:

- `freebsd` and `openbsd` are `required_for_release: true`. Their native jobs consume the sealed
  candidate and produce the package evidence that
  [`release_bundle.py`](../../engineering/tools/release_bundle.py) demands. They currently close as
  `OPEN_GATE`, so the release policy stays red until the upstream account allocation exists.
- `deb`, `rpm`, `macports` and `homebrew` run through a `release`-profile call of
  [`package-ci.yml`](../../.github/workflows/package-ci.yml) with the sealed candidate. That call is
  deliberately outside the verify job's dependencies: those backends are diagnostic and cannot
  admit a release artifact.
- Publication never rebuilds, never clobbers an asset and refuses an existing tag.

Three admission tools and the CI orchestrator are covered by tests and wired fail-closed:

- [`package-ci.yml`](../../.github/workflows/package-ci.yml) closure runs
  [`run_package_admission.py`](../../engineering/tools/run_package_admission.py) whenever a sealed
  candidate is supplied (the release-profile `workflow_call`).
- [`release.yml`](../../.github/workflows/release.yml) runs a dedicated `package-admission` job for
  FreeBSD/OpenBSD `release_evidence`, adapted without inventing package-matrix `PASS` rows.

Both retain `package-admission.json`, `upgrade-baseline.json`, `artifact-manifest.json` and
`installed-sdk-matrix.json`. `--allow-blocking` keeps known residual blockers visible inside the
report without inventing `admitted: true`; the job fails only when the orchestrator cannot produce
a schema-valid report. Local reproduction:

```bash
python3 engineering/tools/run_package_admission.py \
  --profile release --candidate dist/release-candidate \
  --evidence-root build/package-evidence --allow-blocking --replace \
  --output-dir build/package-admission
```

## Sealed N−1 upgrade continuity

`package-upgrade` selects its predecessor from the release context (SemVer precedence over
annotated tags), never as "current version minus one":

| Situation | Status |
| --- | --- |
| No annotated predecessor | `NOT_APPLICABLE_INITIAL_BASELINE` |
| Predecessor selected, no sealed packages | `NOT_RUN` |
| Predecessor selected and `GLIFISTORE_N1_PACKAGE_DIR` set | Linux deb/rpm, FreeBSD/OpenBSD, and MacPorts/Homebrew run install→seed→upgrade→verify inside the native/container lifecycle |
| Invalid `GLIFISTORE_N1_PACKAGE_DIR` | `FAIL` (never rebuild from HEAD) |

Mount the sealed predecessor packages into package CI; they are also forwarded into the Linux
packaging container as `/n1-packages`:

```bash
export GLIFISTORE_N1_PACKAGE_DIR=/path/to/sealed-n1-debs-or-rpms
GLIFISTORE_PACKAGE_CI_CONTAINER=1 scripts/package-ci.sh --profile nightly --backend deb
```

## What this pipeline does not prove

- The `deb`/`rpm` container lifecycle has retained nightly evidence at `LIFECYCLE_VERIFIED` with
  `service-lifecycle` `PASS`
  ([run 34662210614](https://github.com/gipidevdor/GlifiStore/actions/runs/34662210614)); the
  overall result stays `NOT_RUN` without a sealed candidate.
- MacPorts declares an unprivileged launchd startup item; Homebrew declares `brew services`. Both
  are exercised when native packaging CI is enabled; without a retained native run they stay
  `OPEN_GATE` for `service-lifecycle`.
- `package-upgrade` stays unproven until a predecessor exists and its sealed N−1 packages are
  supplied, as in the table above; they are never rebuilt from HEAD.
- The cross-SDK post-install matrix against a package-installed daemon has retained nightly `PASS`
  evidence
  ([run 34666166603](https://github.com/gipidevdor/GlifiStore/actions/runs/34666166603));
  `admitted: true` still waits on a sealed N−1 `package-upgrade`.
- In-repository packaging is the project's own pipeline. It is not a Debian, Fedora, MacPorts,
  Homebrew, FreeBSD or OpenBSD acceptance, and nothing here may be described as one.

The full ledger is in the [release residuals](wave5-l7-residuals.md); the gate states are in
[`engineering/gates/quality-gates.yaml`](../../engineering/gates/quality-gates.yaml) and the derived
[production readiness](../production-readiness.md) view.
