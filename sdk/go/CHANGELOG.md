# Changelog

## Unreleased

- Require Go ≥ 1.27 (CI and packaging bootstrap use 1.27.x / 1.27.1).
- Package a normalized tracked-source archive, rebuild its interop executable outside the checkout,
  and use that artifact-built peer in the installed secure-profile matrix.
- Make the interop CLI assert structured `permission_denied` and `overloaded` mutation outcomes in
  the all-SDK secure-profile prefix/quota matrix, with a bounded burst for quota exhaustion.
- Route the first pipeline key once instead of hashing it once for initialization and again during
  ownership validation; subsequent keys retain the same single-Worker check.
- Add a mixed-owner `ExecuteBatch` benchmark mode and generate keys with the routing identity
  negotiated from the server, including keyed SipHash configurations.
- Replace per-call batch maps, copied staging requests and mutexed result collection with lazily
  preallocated Worker-indexed request/index vectors and disjoint positional result writes.
- Bypass grouping and fan-out for one-Worker batches while preserving positional pre-admission
  failures and the configured per-Worker request limit.

## 0.1.0

- Initial synchronous Go client: wire protocol v2 codec, structured errors, pipelines, batch,
  monotonic deadlines, per-call timeouts, interop CLI, and golden fixtures.
- `client.Version` exported for packaging and lockstep checks.
- Packaging tests a tracked-file module snapshot and an external consumer before tagging.
