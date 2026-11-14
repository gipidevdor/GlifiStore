# ADR 0013: Native binary wire protocol v2

- Status: accepted
- Date: 2026-07-19
- Owners: networking maintainers
- Related: ADR 0001, ADR 0006, ADR 0012

## Context

The daemon needs binary-safe keys/values, bounded parsing, pipelining, routing metadata, and Worker
binding. A text command protocol would add parsing and compatibility scope outside the project's
goals.

## Alternatives considered

- Text command protocol: rejected; enlarges parsing and semantic commitment without benefit to the
  exact-key path;
- HTTP: rejected for the initial low-overhead native data path;
- schema framework: deferred until evolution needs outweigh the fixed-header simplicity;
- native C++ struct transmission: rejected because layout, endian, padding, and ABI are unstable.

## Decision

Use a length-prefixed, little-endian binary protocol with explicit 40-byte request and response
headers. Version 2 provides `INIT`, `PING`, `BIND_WORKER`, `GET`, `PUT`, and `ERASE`, request IDs,
bounded frames, pipelined in-order responses, routing epoch/count, and wrong-owner status. Every
field is encoded explicitly; no object representation crosses the wire.

## Consequences

Clients are compact and deterministic but must implement this protocol. TLS, authentication,
compression, idempotency, rebalance, and Unix-domain transport remain independent additions. Any
incompatible field or semantic change requires a new wire version.

## Compatibility and verification

The normative contract is [Wire Protocol v2](../spec/wire-protocol-v2.md). Round-trip, malformed,
boundary, pipelining, and session tests are required. Golden byte fixtures remain required before a
stable external compatibility claim.
