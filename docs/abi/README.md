# C ABI

**Descriptive** · `libglifistore` ABI major 1

The installed shared library exposes a small C ABI, declared in
[`glifistore.h`](../../include/glifistore/abi/glifistore.h); its normative contract is
[C ABI v1](../spec/c-abi-v1.md). This page explains how that surface is versioned, owned, shared and
kept compatible. A stable interface contract is not evidence of production readiness or of platform
durability certification: GlifiStore remains an architectural prototype.

## Surface

ABI v1 contains only version and status functions, open and close, owning GET, PUT, ERASE, PUT batch
and ERASE batch. It is deliberately smaller than the engine, so the permanent compatibility surface
stays small. The daemon links the C++ core directly; server internals are not part of the library.
Consumers use the CMake target `GlifiStore::abi` or `pkg-config glifistore-abi`.

## Versioning

`ABI_VERSION` holds `major.minor` and is independent of `VERSION`, wire v2 and persistence v1.
`gs_abi_major` and `gs_abi_minor` report it at run time, header constants at compile time.
`gs_product_version_major`, `gs_product_version_minor`, `gs_product_version_patch` and the
static-lifetime `gs_product_version_string` report the product version separately. A consumer that
selects a library outside the platform loader's SONAME rules must compare the major first.

- An additive entry point or a tail-appended optional field increments the minor, after
  compatibility tests.
- Removing a symbol, or changing a signature, calling convention, layout, value or established
  semantic contract, increments the major.
- Product-only, wire-only and storage-format-only changes leave `ABI_VERSION` unchanged.

The loader major derives from the same file. Packaging must not duplicate it or infer it from the
product version.

## Lifetime

`gs_store_open` creates one opaque owning handle. Input byte views and option path bytes are
borrowed only until the call returns. GET copies the value into caller storage, so no `RecordRef`,
Segment mapping, file handle or `ReadGeneration` lifetime escapes the engine.

`gs_store_close` closes the engine and consumes the handle whatever status it returns; the caller
must not reuse or free the pointer. Close must be serialized externally with every other call, so
a foreign runtime cannot race a call against destruction of the handle.

Version strings have static lifetime and must not be modified or freed. Status messages are copied
into caller memory and need no release function.

## Threading

After a successful open, GET, PUT, ERASE and the batch calls are thread-safe and synchronized by the
paired Store; concurrent GET/GET and GET/mutation calls are supported. The facade adds no global
data-path mutex and no thread-local allocation.

Caller threads may share one `gs_store`; the variable that holds it stays caller-owned. Close is the
exclusive terminal operation: the caller must stop new calls and wait for every foreign-language
task using the handle before closing. Operations already admitted follow the Store's drain contract,
but the wrapper cannot make a stale pointer safe once its allocation is consumed.

ABI v1 has no callbacks, reentrancy hooks, asynchronous completions or thread-affine output buffers.
Batch input and result arrays must not be modified concurrently during a call.

## Errors and outcomes

`gs_status` carries a stable numeric `code`, a broad `category`, retryability advice and a zero
reserved word. C++ diagnostic strings are not exposed: `gs_status_message` maps a code to
deterministic text and returns the bytes required, including the terminating NUL.

The mutation outcome is separate from the status. `GS_MUTATION_REJECTED` is the only outcome that
asserts known-not-committed; today only facade validation produces it, before engine entry. I/O,
internal, unavailable and integrity failures, `GS_NOT_FOUND` from the engine (including a same-key
FIFO second ERASE) and other ambiguous failures are `GS_MUTATION_INDETERMINATE` unless the facade
has stronger evidence. This can be more conservative than the internal failure; it never authorizes
an unsafe replay.

Retryability is advice, not permission to replay an indeterminate mutation. Callers must combine
both fields with their own idempotency protocol.

## Compatibility

ABI major 1 preserves every published symbol signature, numeric constant, fixed layout prefix,
ownership rule, mutation polarity and acknowledgement semantic. Compatible additions are new symbols
or optional tail fields that old consumers can ignore. Source aliases and new header helpers may be
source-compatible without changing the binary layout.

A new major is required to remove or rename a symbol; change a parameter or return type; change
size, offset or alignment within an existing prefix; reuse a numeric value; expose an internal
pointer lifetime; weaken close or thread guarantees; or change a mutation from rejected to
indeterminate or committed, or the reverse, without a new contract.

The initial ABI 1.0 library has same-build C and symbol evidence. Each candidate packages its
installed C consumer as a deterministic, sealed, platform-specific fixture. A later release must
verify the prior release's seal, release-wide policy, annotated tag and Sigstore attestation before
extracting that compiled consumer, then runs old binary against new library and new binary against
old library, proving dynamic-loader resolution in each direction. Until a complete prior official
release exists, this gate stays below release acceptance, although the contract and its producer are
in force.
