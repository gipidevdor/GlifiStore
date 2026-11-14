# Third-party notices

Status: normative for redistributors and contributors  
Copyright (c) 2026 Giacomo Picchiarelli (catalog maintained with GlifiStore)  
Applies to: GlifiStore core, official SDKs, CI tooling acknowledgements
Owner: maintainers
Last reviewed: 2026-08-01

This catalog lists third-party software, algorithms, and materials **involved**
in GlifiStore so copyright and license conditions can be respected. The product
license is BSD-3-Clause ([LICENSE](LICENSE)); project attribution is in
[NOTICE](NOTICE). Operator policy: [docs/legal/licensing.md](docs/legal/licensing.md).

GlifiStore does **not** vendor OpenSSL, LibreSSL, Abseil, or other large
third-party source trees. Runtime SDK modules currently declare **no**
third-party package dependencies (`sdk/go/go.mod`, `sdk/python/pyproject.toml`).

## 1. Linked / system libraries (optional TLS)

| Component | Typical use | License | Notes |
| --- | --- | --- | --- |
| OpenSSL 3.x | TLS 1.3 on Linux/macOS/FreeBSD when `GLIFISTORE_ENABLE_TLS` finds OpenSSL | [Apache-2.0](https://www.openssl.org/source/license.html) | Not shipped as source here. Binary redistributors who link OpenSSL must retain Apache-2.0 notices for that library. See [docs/legal/tls-redistribution.md](docs/legal/tls-redistribution.md). |
| LibreSSL | TLS on OpenBSD (first-class) | [ISC / OpenSSL legacy dual](https://www.libressl.org/) as shipped by the OS | System library; follow OpenBSD packaging norms. See [docs/legal/tls-redistribution.md](docs/legal/tls-redistribution.md). |

Disable TLS (`GLIFISTORE_ENABLE_TLS=OFF`) for cleartext-only builds that must not link a TLS library.
Operator checklist for linked TLS binaries: [docs/legal/tls-redistribution.md](docs/legal/tls-redistribution.md).

## 2. Algorithms and designs (independent implementations)

| Item | Role in GlifiStore | Copyright / terms | Attribution |
| --- | --- | --- | --- |
| SipHash-2-4 | Optional Worker routing (`siphash24-v1`, ADR 0030); C++/SDK implementations | Algorithm by Jean-Philippe Aumasson and Daniel J. Bernstein; reference code historically CC0 | Cite: Aumasson & Bernstein, *SipHash: a fast short-input PRF*. Test vectors in unit tests follow the published paper vectors. GlifiStore’s code is an independent implementation, not a copy of a third-party source tree. |
| FNV-1a 64-bit | Default key hashing / routing | Public-domain style Fowler–Noll–Vo parameters | Independent implementation of the published FNV-1a parameters. |
| CRC32C (Castagnoli) | Persistent Record/Manifest/Segment integrity (ADR 0014) | Castagnoli polynomial `0x82F63B78`; standard IEEE/IETF use | Independent table/hardware implementation; ASCII vector `123456789` → `0xE3069283`. Not a cryptographic MAC. |
| SwissTable-style Index | Worker Index open addressing (ADR 0007) | Design popularized by Abseil Swiss Tables (Apache-2.0) | **Independent reimplementation.** No Abseil source is included. Layout/control-byte ideas are acknowledged; license obligations of Abseil do not attach to GlifiStore source. |

## 3. Official SDK runtime dependencies

| SDK | Third-party runtime deps | License files |
| --- | --- | --- |
| C++ client (in-tree) | None beyond OS / optional OpenSSL|LibreSSL | Root `LICENSE` |
| Python | None (`dependencies = []`) | `sdk/python/LICENSE` |
| Go | None (empty `go.mod` require set) | `sdk/go/LICENSE` |
| Perl | None beyond dual-life core | `sdk/perl/LICENSE` |
| Ruby | Runtime gem has no required third-party gems; CI may install `async` for tests only | `sdk/ruby/LICENSE` |
| Erlang | OTP stdlib only for the client | `sdk/erlang/LICENSE` |

CI `scripts/ci-license-check.sh` fails closed on exclusive GPL/AGPL/SSPL in scanned Go/Python trees.

## 4. Build, test, and CI tooling (not shipped in runtime packages)

These tools are **involved** when building or verifying GlifiStore. They are not copied into
`glifistored` or SDK wheels by default. Operators who redistribute CI scripts or Action
wrappers must respect each tool’s license.

| Tool / Action | Role | Typical license |
| --- | --- | --- |
| CMake, Ninja, compilers (clang/gcc/apple-clang) | Build | Toolchain licenses |
| CTest / project test harness | Tests | BSD-3-Clause (this repo) |
| GitHub Actions (`actions/checkout`, `setup-*`, CodeQL, Scorecard, Trivy, gitleaks, lychee, …) | CI | Each Action’s LICENSE in its repository; pinned by SHA ([actions-pinning](docs/security/actions-pinning.md)) |
| `vmactions/freebsd-vm`, `vmactions/openbsd-vm` | FreeBSD / OpenBSD CI VMs | MIT (vmactions upstream); SHA-pinned in workflows |
| Syft | SPDX SBOM generation | Apache-2.0 (Anchore); installed via checksum-pinned release (`scripts/install-syft.sh`) |
| Cosign / Sigstore | Tag signing | Apache-2.0 |
| go-licenses, pip-licenses | License scanning in CI | Apache-2.0 / MIT as upstream |
| actionlint | Workflow lint | MIT (rhysd) |
| TLC / TLA+ (formal job) | Bounded required model checking (CI fail-closed) | TLA+ / Community licenses as upstream |
| mise | Optional Ruby bootstrap inside packaging containers | MIT (jdx); checksum-pinned release (`scripts/packaging/ensure-installed-sdk-matrix-tools.sh`) |

## 5. Documentation and artwork references

| Material | Role | Notes |
| --- | --- | --- |
| Apple Human Interface Guidelines / Icon Composer docs | Linked from `artwork/README.md` | External documentation; Apple’s copyright applies to Apple’s materials. GlifiStore artwork under this repository is BSD-3-Clause with the project copyright. |
| ADRs / specs | Normative project docs | BSD-3-Clause as part of the repository |
| Doxygen Awesome v2.5.0 | Vendored documentation CSS/JavaScript, commit `46483f1e5a70ffb9ecd3b82d0a1cd1b24edf13da` | Copyright (c) 2021–2023 jothepro, MIT. Original files and [license](docs/doxygen/theme/LICENSE) are retained under `docs/doxygen/theme/`. The license is copied into the generated site; these assets are not linked into the runtime. |

## 6. Redistributor checklist

1. Keep `LICENSE` and this file (and `NOTICE`) with source and binary distributions.
2. If linking OpenSSL or LibreSSL, satisfy **that** library’s license for the binary you ship.
3. Do not use the names of copyright holders to endorse derived products without permission
   (BSD-3-Clause clause 3).
4. Do not strip copyright headers from files that carry them (Perl POD, LICENSE copies, NOTICE).
5. When adding a third-party dependency, update this file **and** `NOTICE` in the same change;
   extend `scripts/ci-license-check.sh` / Dependabot as needed.

## 7. Reporting

License or copyright concerns: see [SECURITY.md](SECURITY.md) for private reporting channels, or
open a non-security issue labeled `legal` for public clarification.
