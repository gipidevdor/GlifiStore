# ADR 0038: Stable C ABI and artifact-delivery architecture

- Status: accepted
- Date: 2026-08-26
- Deciders: storage, API, portability, and release maintainers
- Applies to: embedded shared library and release artifacts; paired runtime unchanged
- Depends on: ADR 0009, ADR 0032, ADR 0037
- Supersedes: the C-ABI portion of `ABI-0X-NO-STABLE`; the C++ ABI remains not promised

## Context

The public C++ API uses PImpl but still exposes C++ standard-library types, compiler ABI, exception
model, and toolchain coupling. Language bindings and native packages need a smaller binary boundary
that can outlive internal engine refactors. Stabilizing the whole C++ engine would permanently
freeze implementation details and conflict with the prototype's active performance work.

Release identity also spans independent product, C ABI, wire, and persistence versions. Inferring
one from another would make SONAME and package upgrades unsafe.

## Decision drivers

- Preserve paired ownership, visibility, durability, and fail-closed recovery exactly.
- Prevent C++ exceptions, allocators, internal pointers, and server code from crossing the ABI.
- Make symbol/layout drift machine-detectable on Linux, macOS, FreeBSD, and OpenBSD.
- Keep the daemon free to link the static core directly with LTO and internal specialization.
- Make a built candidate immutable between verification and publication.

## Alternatives considered

### Stabilize the C++ ABI

Rejected. STL layouts, compiler/library combinations, exceptions, and internal types create a large
and brittle compatibility surface before 1.0.

### Make the daemon consume the shared C ABI

Rejected. It adds an artificial boundary to the official reactor/paired integration, impedes LTO,
and would tempt publication of server-only controls. The daemon links the core directly.

### Expose zero-copy reads or callbacks immediately

Rejected. They export generation reclamation, allocator, reentrancy, and thread-lifetime contracts
that are not necessary for ABI 1.

## Decision

1. `libglifistore` exports only an `extern "C"` facade with opaque handles and caller-owned buffers.
2. `ABI_VERSION` is the single major/minor authority. `VERSION`, wire v2, and persistence v1 remain
   independent.
3. ABI 1 contains only version/error, open/close, GET, PUT, ERASE, and PUT batch.
4. The shared facade links the PIC static core privately. `glifistored` continues to link core/server
   targets and never routes its native hot path through the C ABI. On ELF, a distinct ABI-private
   PIC core prevents the shared-library relocation requirement from constraining the daemon's
   directly linked static core; Darwin does not require the duplicate target.
5. Hidden visibility plus `abi/symbols-v1.txt` controls exports; layout assertions and a pure-C
   consumer pin the header contract.
6. Releases use candidate → verify → publish. Verification must cover archive/package integrity,
   SBOM/provenance, ABI symbols/layout, install consumption, persistence/wire compatibility, and
   negative tamper cases. The candidate is admitted as an exact set before post-build inputs;
   evidence and native packages enter through a collision-free digest inventory and the final seal
   binds the complete augmented set. Publication never rebuilds a verified candidate.
7. Opaque handles prevent Index, Segment, Reader generation, and server-core ownership from becoming
   public ABI.

## Consequences

Positive: language-neutral binary consumption, bounded compatibility surface, allocator isolation,
and freedom to optimize internals. Negative: one owned copy for GET, explicit caller buffers, and a
second version line to govern. Durable group/periodic, backup, maintenance, async, and zero-copy are
not available through ABI 1.

The C ABI contract does not raise the project's production-readiness claim. Cross-release ABI proof
cannot exist until a first tagged ABI-1 artifact is retained.

## Compatibility impact

Persistence v1, wire v2, routing, mutation visibility, ACK ordering, and recovery are unchanged.
C++ ABI remains not promised before 1.0. ABI-1 additions may append optional fields or symbols;
breaking changes require ABI major 2 and a parallel migration window.

## Verification

- Pure C compile/link/runtime test and installed-prefix consumer.
- Static C++ assertions for ABI layouts.
- Exact symbol allowlist plus a negative unexpected-symbol test.
- Assurance requirement/gate/hazard links.
- Candidate/source/install-prefix tooling, negative tamper tests, deterministic archives, release
  manifest/checksum/seals, exact candidate admission, closed evidence import, artifact-bound SPDX
  identity/license checks, and tag-only
  Candidate → Verify → Publish workflow.
- Structural FreeBSD/OpenBSD reference-port validation; native package/service evidence remains a
  fail-closed prerequisite rather than a completed claim.
- Same-run installed-SDK candidate certification covers C/C++ discovery and isolated packaged
  Python, Perl, Go, Ruby, and Erlang interop without rebuilding the engine; retained tag evidence
  remains pending.
- Future: old compiled consumer against each new ABI-1 candidate and native BSD package/service
  validation before release acceptance.

## Relationships

ADR 0009 continues to govern owning reads. ADR 0032/0037 continue to govern the Store underneath the
facade. This ADR may be superseded only by an explicit new ABI-major and artifact migration ADR.
