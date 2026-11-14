# Certificate rotation and revocation

**Descriptive** · `glifistored` secure profile (mTLS and `--authz-map`) · reviewed 2026-07-28

How to rotate TLS and mTLS material, revoke a principal and respond to authentication incidents on
a secure-profile daemon. The profile itself is defined in the
[secure profile reference](../security/secure-profile.md).

**Note.** Official SDKs parse the keyed SipHash Worker-routing `INIT` identity that
`--secure-profile` selects ([ADR 0030](../adr/0030-keyed-worker-routing.md)). The smoke test
`scripts/test-secure-profile-interop.sh` covers mTLS, authz, prefix, CRL, quotas and keyed routing
for cpp, python and go, plus perl, ruby and erlang when their toolchains are present. See the
[SDK roadmap](../architecture/sdk-roadmap.md).

## Rotate server or client CA material

1. Issue replacement PEMs (server certificate and key, client CA, client certificate and key) with
   short lifetimes.
2. Install them with mode `0600` for private keys, keeping paths stable when possible.
3. Recycle the daemon (or restart the listener process) so `TlsContext` reloads the PEMs. There is
   no in-band credential opcode.
4. Confirm that `--dump-config` still shows path-only TLS fields, and that `--log-format json` (or
   `--secure-profile`) emits `auth` accept events for healthy clients.
5. Roll the client SDK `cert_file`, `key_file` and `ca_file` in the same release window.

## Revoke a principal

The authorization map is the preferred, immediate control:

1. Remove the principal's line from `--authz-map` (or replace the map file) and recycle the daemon.
2. Data-plane opcodes for that principal now return wire `PERMISSION_DENIED` (8). Lifecycle
   `HEALTH` and `READY` still require a verified mTLS principal.

Cryptographic revocation fails closed:

1. Publish an updated PEM CRL that lists the revoked serial.
2. Point the daemon at it with `--tls-crl /path/clients.crl`.
3. Optionally set `--tls-ocsp-fail-closed`, which refuses startup unless `--tls-crl` is also set.
   Live AIA OCSP HTTP lookups are unsupported: they conflict with OpenBSD `pledge` without `dns`.
4. Recycle the daemon. Revoked peers fail the handshake, the JSON audit emits
   `{"event":"auth","decision":"deny",...}`, and `STATS` increments `auth_denies`.

## Incident response

| Signal | Action |
| --- | --- |
| Spike in `auth_denies` or `tls_errors` | Check CRL freshness, client CA trust and clock skew |
| Spike in `authz_denies` | Confirm the map contents; look for principal typos (exact SAN or CN match) |
| Compromised client key | Revoke the serial through the CRL and remove the principal from the authz map |

## Residual risks

- Multi-tenant adversarial isolation remains Phase 8.
- Physical E3/E4 power-loss certification remains open.
- Live OCSP HTTP is intentionally out of scope; the CRL is the supported fail-closed revocation
  path.
