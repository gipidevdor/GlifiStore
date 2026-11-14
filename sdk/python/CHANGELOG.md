# Changelog

## Unreleased

- Align packaging classifiers with the architectural-prototype claim ceiling
  (`Development Status :: 2 - Pre-Alpha`) and declare Python 3.14 support to match CI.

- Decode buffered responses directly by offset, avoiding one temporary `memoryview` object per
  response while retaining owned result values.
- Materialize each pipeline response slot only at its final outcome instead of allocating and
  immediately replacing eager failure placeholders.
- Assemble each sync and asyncio pipeline from validated headers and payload views with one native
  final join, avoiding one complete frame allocation and payload copy per request.
- Add an installed-wheel secure-profile matrix that fails if Python resolves the SDK from the
  repository instead of the isolated virtual environment.
- Reuse each batch request's validated Worker assignment while encoding its per-Worker pipeline,
  avoiding a second routing hash in both synchronous and asyncio clients.
- Add direct sync/async batch benchmark modes using the routing identity negotiated from the server.

## 0.1.0 — 2026-07-19

- Initial PyPI-ready release of the native wire-protocol v2 client.
- Synchronous `Client` and asyncio `AsyncClient` with one bound TCP connection per Worker.
- Full request/response codec, FNV-1a routing, ordered pipelines, and indeterminate mutation outcomes.
- Packaging installs wheel and normalized sdist independently and runs conformance against both.
- Async cancellation lifecycle tests close transports locally and treat resource leaks as errors.
