# OpenBSD reference port

This directory holds upstream reference material for a future OpenBSD ports submission: the native
CMake module, base LibreSSL, kqueue, the existing pledge/unveil path, an `_glifistore` account,
`rc.subr`/`rcctl`, configuration samples and OpenBSD major/minor shared-library notation.

## Not yet a proven package

The tree is deliberately not yet represented as a proven package: the `_glifistore` account details
must be reviewed with the ports project. Do not create `packaging/openbsd/PORTS_ACCOUNT_REGISTERED`
until that upstream registration exists. The marker is an honesty latch for
`validate_bsd_packaging.py --release` and the fail-closed OpenBSD package producer, not a claim of
ports acceptance.

A native release producer must generate `distinfo` outside the source archive, from its already
sealed bytes; committing that self-digest here would be circular. Retain native-VM evidence for fake
installation, plist and shared-symbol checks, package install, the `rcctl` lifecycle,
PUT/GET/ERASE and recovery, upgrade and deinstall.

## Evidence

`scripts/package-ci.sh --backend openbsd` reports this tree as `structural` evidence. Its
`native-build`, `package`, `service` and `upstream-accepted` rows stay `BLOCKED` without a native
host, the marker above and an admitted sealed candidate; see
[BSD reference packaging](../../docs/distribution/bsd-packaging.md).

`package-upgrade` stays `NOT_APPLICABLE_INITIAL_BASELINE` or `NOT_RUN` until sealed N−1 packages are
supplied through `GLIFISTORE_N1_PACKAGE_DIR`. The native lifecycle then runs
install→seed→upgrade→verify against those `.tgz` bytes, never rebuilt from HEAD. OpenBSD does not
invent a PASS from selection alone.

Cross-compilation or structural validation is not package evidence.
