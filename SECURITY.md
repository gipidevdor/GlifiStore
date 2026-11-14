# Security policy

GlifiStore is pre-alpha software. No release carries a production support, durability,
wire-compatibility or disk-format guarantee.

| Version | Security status |
| --- | --- |
| `main` / `0.1.x` | Best-effort security fixes during prototype development |
| Earlier snapshots | Unsupported |

## Reporting a vulnerability

Never report a suspected vulnerability in a public issue, discussion or pull request.

1. On the repository's **Security** page, use **Report a vulnerability** when that control is
   available.
2. Otherwise, contact the repository owner through the existing private project channel and ask
   for a draft GitHub Security Advisory. Keep exploit details out of public channels.

Include as much as you can:

- the affected version and exact commit;
- operating system, architecture, compiler and build preset;
- a minimal reproduction or malformed input sample;
- the expected impact and realistic attack preconditions;
- sanitizer output, stack trace, logs or corrupted Segment metadata;
- whether the issue is already public or under a disclosure deadline.

Expect an acknowledgement within three business days. An acknowledgement does not confirm severity
or eligibility for a CVE.

## Scope

Persisted Segment files, native network frames and recovery metadata are all untrusted input. A
crash, out-of-bounds access, integer overflow, use-after-free, data race or silent corruption is
security-relevant. Reports matter most when they involve:

- memory safety or undefined behavior reachable from untrusted bytes;
- unchecked lengths, offsets, sequence numbers or allocation arithmetic;
- corruption that survives validation or produces a wrong recovery result;
- cross-Worker ownership violations or concurrency defects that expose or corrupt data;
- denial of service that bypasses documented resource bounds;
- compromise of the build, a dependency or the release pipeline.

Performance differences, build failures on unsupported platforms and ordinary correctness bugs
without a security boundary or realistic impact belong in the normal bug template.

## Network posture

`glifistored` defaults to **cleartext TCP with no authentication**. Bind it to `127.0.0.1` or
another trusted private boundary, and do not expose the listener to untrusted networks until mTLS
principals and capabilities are enabled.

Optional TLS 1.3 outer transport is available in builds with LibreSSL or OpenSSL (`--tls-cert`,
`--tls-key`, dual `--tls-port`). The [secure profile](docs/security/secure-profile.md) and ADRs
[0020](docs/adr/0020-tls-outer-transport.md) to [0022](docs/adr/0022-authorization-capabilities.md)
specify it, with optional key-prefix scope in [0025](docs/adr/0025-key-prefix-tenant-scope.md); the
[security roadmap](docs/security/roadmap.md) tracks the open work. TLS 1.3, mTLS principals,
`--authz-map`, `--secure-profile`, abuse limits, audit events, CRL checks and persisted SipHash
Worker routing are implemented. The C++, Python, Perl, Go, Erlang and Ruby source SDKs all implement
opt-in TLS, mTLS and keyed `INIT` routing; an installation still fails closed when its language's
TLS dependency or toolchain is unavailable.

Enable the full secure profile before leaving a trusted perimeter, and configure `--tls-crl` (and
optionally `--tls-ocsp-fail-closed`) before facing the public Internet. Optional `--authz-map`
`prefix=` scopes isolate GET, PUT and ERASE key namespaces; adversarial multi-tenancy on a shared
daemon remains unsupported. Live OCSP over HTTP is unsupported, and physical E3/E4 durability
certification remains open.

OpenBSD is a first-class portability target: LibreSSL, `pledge` and `unveil` after
`Server::create`, not durable-storage certification. CI builds and tests the daemon against system
LibreSSL ([`openbsd-libressl.yml`](.github/workflows/openbsd-libressl.yml)). Threat model:
[docs/security/threat-model.md](docs/security/threat-model.md).

## Coordinated disclosure

Allow time to reproduce, assess, fix and test before disclosure. The maintainer coordinates a
disclosure date and credit through the private advisory once a report is confirmed. Do not include
repository secrets, personal data or third-party confidential material in a report.

## Copyright and license concerns

License and third-party attribution policy: [docs/legal/licensing.md](docs/legal/licensing.md),
[NOTICE](NOTICE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). For a suspected license or
copyright violation that is not a security vulnerability, use a private channel as above or a public
issue labelled `legal`, without attaching proprietary third-party source.
