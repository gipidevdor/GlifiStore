# TLS library redistribution

**Normative** · OpenSSL 3.x and LibreSSL linked builds · reviewed 2026-08-01

What a redistributor who ships TLS-enabled GlifiStore binaries must include for the linked TLS
library. GlifiStore does not vendor OpenSSL or LibreSSL source: when `GLIFISTORE_ENABLE_TLS` is
enabled, the build links against a system or user-provided TLS library.

## OpenSSL 3.x (Apache License 2.0)

If you distribute a binary linked against OpenSSL 3.x, statically or dynamically, where your
jurisdiction treats the combination as a redistribution you control:

1. Include a copy of the OpenSSL Apache-2.0 license text with your distribution.
2. Include any `NOTICE` file that accompanies the OpenSSL build you use (Apache-2.0 NOTICE
   retention).
3. Do not claim OpenSSL endorsement.
4. Prefer documenting the OpenSSL version and soname you linked.

Upstream terms: [OpenSSL license](https://www.openssl.org/source/license.html).

## LibreSSL (OpenBSD and ports)

LibreSSL is typically provided by the operating system or the ports tree. Follow the license files
shipped with that package (often ISC and related notices), and do not strip those files from OS
packages. GlifiStore's OpenBSD CI uses system LibreSSL only.

## Cleartext builds

Configure with TLS disabled when you must avoid linking a TLS library. Official secure-profile
deployments require TLS ([secure profile](../security/secure-profile.md)).

## See also

- [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md) §1
- [Licensing and copyright](licensing.md)
