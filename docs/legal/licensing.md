# Licensing and copyright

**Normative** · GlifiStore repository, official SDKs, installs, CI · reviewed 2026-08-01

The license GlifiStore ships under, the files every redistribution must carry, and the rules for
third-party code and contributions. It binds contributors and redistributors.

## Project license

GlifiStore (core, tools, official SDKs, tests, docs and artwork in this repository) is
BSD-3-Clause. The authoritative text is [`LICENSE`](../../LICENSE).

Copyright holder: Giacomo Picchiarelli (2026). SPDX: `BSD-3-Clause`.

## What must travel with a redistribution

| Artifact | Required files |
| --- | --- |
| Source tree / tarball | `LICENSE`, `NOTICE`, `THIRD_PARTY_NOTICES.md` |
| CMake install (Runtime component) | `LICENSE`, `NOTICE`, `VERSION`, `THIRD_PARTY_NOTICES.md` under `${CMAKE_INSTALL_DATADIR}/GlifiStore` |
| Official SDK packages | Each SDK's `LICENSE` (byte-identical to the root one), plus `NOTICE`; Python also ships `NOTICE` via `license-files` |
| TLS-linked binaries | OpenSSL/LibreSSL notices per [TLS library redistribution](tls-redistribution.md) |

BSD-3-Clause requires retaining the copyright notice, conditions and disclaimer in source
redistributions, and reproducing them with binary redistributions.

## Third parties

Do not add a third-party dependency without:

1. confirming a license compatible with BSD-3-Clause redistribution (prefer Apache-2.0, BSD, MIT,
   ISC, MPL-2.0 with compliance, BlueOak, Unlicense/0BSD/CC0);
2. **rejecting exclusive GPL/AGPL/SSPL/"Commons Clause" runtime dependencies**;
3. updating [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md) and [`NOTICE`](../../NOTICE);
4. keeping the CI license scans green (`scripts/ci-license-check.sh`,
   `.github/workflows/license-check.yml`).

Optional TLS linkage to OpenSSL or LibreSSL is allowed; redistributors of linked binaries must
follow those libraries' licenses ([`THIRD_PARTY_NOTICES.md` §1](../../THIRD_PARTY_NOTICES.md)).

## Algorithms and copied code

Independent implementations of published algorithms (SipHash-2-4, FNV-1a, CRC32C, SwissTable-style
layouts) are fine when:

- no substantial third-party source is pasted without its license;
- papers and design inspirations are cited in `THIRD_PARTY_NOTICES.md` or the ADRs;
- test vectors from papers are attributed.

## Contributions

By contributing, you agree that your contribution is licensed under the same BSD-3-Clause terms and
that you have the right to submit it. Prefer adding an SPDX line to new substantial files:

```text
// SPDX-License-Identifier: BSD-3-Clause
// SPDX-FileCopyrightText: 2026 Giacomo Picchiarelli
```

Do not contribute code you are not authorized to relicense under BSD-3-Clause.

## CI enforcement

- `scripts/ci-license-check.sh` checks the notice files, exact SDK `LICENSE` sync, `reuse lint`
  and the Go and Python dependency scans; it fails closed on exclusive GPL/AGPL/SSPL.
- `engineering/tools/validate_actions_pins.py` keeps third-party Actions SHA-pinned. This is supply
  chain, not a substitute for copyright compliance.
- Tag supply-chain jobs emit SPDX SBOMs for packaged SDK artifacts.

## Related

- [`NOTICE`](../../NOTICE)
- [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md)
- [TLS library redistribution](tls-redistribution.md)
- [Actions pinning](../security/actions-pinning.md)
- [Contributing](../../CONTRIBUTING.md)
