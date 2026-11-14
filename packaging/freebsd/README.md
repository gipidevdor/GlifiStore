# FreeBSD reference port

This directory holds upstream reference material for a future FreeBSD ports submission: staged CMake
installation, system TLS, `USE_LDCONFIG`, a dedicated service account, a `.sample` configuration and
native `rc.subr` integration.

## Not yet a proven package

The tree is deliberately not yet represented as a proven package: the `glifistore` UID and GID still
need allocation by the FreeBSD ports project. Do not create
`packaging/freebsd/PORTS_ACCOUNT_REGISTERED` until that upstream registration exists. The marker is
an honesty latch for `validate_bsd_packaging.py --release` and the fail-closed FreeBSD package
producer, not a claim of ports acceptance.

The release producer generates `distinfo` outside the source archive, from its already sealed
bytes; committing that self-digest here would be circular. After the service account is registered,
retain evidence for `make stage`, `check-plist`, `package`, a clean-host install, service
PUT/GET/ERASE and recovery, upgrade and deinstall.

## Evidence

`scripts/package-ci.sh --backend freebsd` reports this tree as `structural` evidence. Its
`native-build`, `package`, `service` and `upstream-accepted` rows stay `BLOCKED` without a native
host, the marker above and an admitted sealed candidate; see
[BSD reference packaging](../../docs/distribution/bsd-packaging.md).

`package-upgrade` stays `NOT_APPLICABLE_INITIAL_BASELINE` or `NOT_RUN` until sealed N−1 packages are
supplied through `GLIFISTORE_N1_PACKAGE_DIR`. The native lifecycle then runs
install→seed→upgrade→verify against those `.pkg` bytes, never rebuilt from HEAD. FreeBSD does not
invent a PASS from selection alone.

Do not set a release gate to accepted merely because this reference tree parses.
