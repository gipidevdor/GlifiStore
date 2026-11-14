# TLS performance

**Descriptive** · secure-profile TLS performance, not a wire or disk contract · reviewed 2026-07-20

How to measure the TLS tax of the secure-profile TLS 1.3 outer transport
([ADR 0020](../adr/0020-tls-outer-transport.md)) on the Go client pipeline harness used for
cleartext TCP benchmarks, and how to read the result. This note is Phase 2.5 of the
[security roadmap](roadmap.md).

## Method

```bash
# Requires a TLS-capable glifistored (GLIFISTORE_ENABLE_TLS + LibreSSL/OpenSSL).
OPS=50000 WARMUP=1 REPEATS=5 ./scripts/benchmark_tls_tax.sh
```

`scripts/benchmark_tls_tax.sh`:

1. starts a volatile `glifistored` on loopback in cleartext mode, then again in TLS-only mode, with
   an ephemeral self-signed certificate whose SAN is `DNS:localhost` / `IP:127.0.0.1`;
2. runs `sdk/go/cmd/glifistore-bench` with identical `--ops`, `--pipeline`, `--workers`,
   `--execution concurrent`, warmup and repeats;
3. writes per-cell outputs under `cleartext/` and `tls/`, plus a Markdown summary with the
   `TLS/cleartext` median ops/s ratios.

The Go bench TLS flags have the same semantics as in interop: `--tls`, `--tls-ca`, `--server-name`,
optional mTLS `--tls-cert` and `--tls-key`, and `--insecure-skip-verify` for lab-only escapes.

Hosted CI runners are not absolute throughput evidence. Prefer a quiet, thermally stable developer
host before publishing absolute numbers.

## Representative local result

Captured on 2026-07-20 on Apple Silicon against a Debug/Release TLS-capable `glifistored` (OpenSSL
backend), with volatile storage, Go SDK 0.1.0, `OPS=20000`, `REPEATS=3` and concurrent execution:

| workers | pipeline | cleartext median ops/s | TLS median ops/s | TLS/cleartext |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 38,265 | 37,509 | 0.980 |
| 1 | 32 | 94,338 | 90,562 | 0.960 |
| 1 | 128 | 98,799 | 99,569 | 1.008 |
| 4 | 1 | 113,544 | 102,713 | 0.905 |
| 4 | 32 | 360,342 | 369,840 | 1.026 |
| 4 | 128 | 394,905 | 377,655 | 0.956 |

Ratios near 1.0, and occasionally TLS at or above cleartext within noise, are expected on a warm
loopback connection with deep pipelines. The largest relative tax in this run was about 10%, at
`workers=4`, `pipeline=1`. Re-run with higher `OPS` and `REPEATS` on a quiet host before publishing
capacity claims.

## Interpretation

- Shallow pipelines (p=1) usually show the largest relative TLS tax: per-pair handshake and record
  overhead and syscall boundaries dominate when the engine does not amortize work across a deep
  pipeline.
- Deep pipelines (p=32–128) typically recover most of the cleartext throughput. Record-layer cost
  is amortized across many framed operations on a warm connection, and the engine and SDK remain the
  bottleneck more often than crypto.
- TLS is not free, but it is usually affordable for the session, cache and gateway patterns in
  [where performance matters](../architecture/where-performance-matters.md). Measure your own
  loopback baseline before sizing hardware for a secure-profile deployment.
- OpenBSD with LibreSSL must be re-measured on a native host, or on the OpenBSD CI VM for smoke
  only. Do not extrapolate macOS/OpenSSL ratios to OpenBSD: `fdatasync`≡`fsync` and LibreSSL
  performance characteristics differ. The CI gate, `.github/workflows/openbsd-libressl.yml` with
  `scripts/ci-openbsd-libressl.sh`, checks correctness, not throughput.

## Non-goals

- This note does not change wire protocol v2, client semantics or durability contracts.
- It does not authorize weakening the TLS 1.3-only policy, the hostname verification defaults or
  the fail-closed secure-profile configuration for speed.
- Absolute ops/s from CI VMs are informational only.

## Related

- [Security roadmap](roadmap.md), Phase 2.5 and 2.6
- [Development](../development/README.md): benchmark reports
- `scripts/benchmark_go_client.sh`: the cleartext baseline matrix
