# C ABI v1

**Normative** · `libglifistore` ABI major 1 on supported 64-bit Unix targets

Authority: `ABI_VERSION`, `include/glifistore/abi/glifistore.h`

What the C ABI exposes and guarantees: version domains, types and layout, lifecycle, GET buffers,
mutation outcomes, exceptions and exported symbols.

## Scope and version domains

C ABI v1 is a small synchronous facade over the official paired embedded `Store`. It exposes only
ABI/product version discovery, status text, open/close, owning GET copy, PUT, ERASE, PUT batch, and
ERASE batch. It does not
expose the C++ object model, daemon/server internals, snapshots, backup, compaction, maintenance,
async callbacks, Index, Segment, `ReadGeneration`, or file handles.

The product version, C ABI version, wire protocol v2, and persistence format v1 are independent.
`ABI_VERSION` is the sole authority for the first two C ABI components. ABI major 1 maps to the
shared-library major (`libglifistore.so.1` on ELF and the platform-equivalent compatibility version
on macOS/BSD). A product release does not imply an ABI bump.

## Types and layout

The ABI uses fixed-width integer typedefs, `size_t`, opaque `gs_store`, and byte views. Public enum
values are `uint32_t` constants rather than C enums. `gs_status` is 16 bytes and
`gs_mutation_result` is 32 bytes. C++ layout assertions pin every fixed prefix and critical offset.

`gs_store_options` starts with `struct_size` and `abi_version`. ABI v1 accepts the complete v1
layout and the documented v1 minimum prefix; a size larger than the library understands fails with
`GS_INCOMPATIBLE_ABI`. Reserved bytes visible through `struct_size` must be zero. Future compatible
fields may only be appended. Reordering, resizing, repurposing, or changing an existing field's
meaning is an ABI break.

`gs_bytes_view.data` may be null only when `size == 0`. Inputs are borrowed only for the synchronous
call and are never retained. Keys and values are opaque bytes. Durable directory bytes use the
native Unix path encoding and may not contain NUL.

## Lifecycle and configuration

`gs_store_options_init` zeroes the current structure and establishes deterministic ABI-v1 defaults:
one paired shard, volatile storage, cooperative maintenance, and open-or-create policy. A caller
must then make storage intent explicit. ABI v1 supports only `GS_STORAGE_VOLATILE` and
`GS_STORAGE_DURABLE_SYNC`; periodic/group policy is deliberately not stabilized yet. Durable sync
requires `data_directory`, while volatile mode rejects it.

`gs_store_open` sets `*out_store` to null before validation and publishes a handle only after the
engine has opened successfully. `gs_store_close` consumes the handle even when the final close
status reports an error. Calls after close are invalid because the pointer no longer denotes an
object. The caller must externally serialize close against all other handle calls.

## GET buffers

`gs_store_get` always writes `output_required` on a successful lookup. If the observed value does
not fit, it leaves the caller buffer untouched and returns `GS_BUFFER_TOO_SMALL`. A null buffer with
zero capacity is a size query. The query and a later retry are distinct GET operations: a concurrent
mutation may change the required size or value between them. No pointer into an internal generation
crosses the ABI.

## Mutation outcomes and batches

Every PUT/ERASE result contains both a status and one of:

- `GS_MUTATION_COMMITTED`: completed and visible; in durable-sync mode it also crossed the engine's
  durable acknowledgement boundary;
- `GS_MUTATION_REJECTED`: known not committed, so policy may permit a retry;
- `GS_MUTATION_INDETERMINATE`: the facade cannot prove non-commit; callers must not blindly retry.

“Committed” on a volatile store means completed and visible, not persistence across process loss.
The facade classifies ambiguous engine failures conservatively as indeterminate rather than
inventing retry safety. In particular, engine-returned statuses such as `GS_NOT_FOUND` on ERASE
(including a same-key FIFO second erase) map to `GS_MUTATION_INDETERMINATE`. `GS_MUTATION_REJECTED`
is reserved for known pre-engine rejects at the facade (for example malformed arguments).

`gs_store_put_batch` accepts at most `GS_MAX_BATCH_ITEMS`, uses caller-owned result storage, and
forwards to the paired engine's shard batching. Items are returned in input order. A batch is not a
transaction: shards publish independently and each item retains its own status and outcome.
Malformed descriptors reject the whole call before submission. Same-key items within one shard
batch observe FIFO ordering (later siblings see earlier mutations).

`gs_store_erase_batch` follows the same bounds, result storage, shard batching, non-transaction
rules, and same-key FIFO semantics as `gs_store_put_batch`, using `gs_erase_request` key
descriptors (no value / expire fields).

## Exceptions and symbols

All entry points are `noexcept` when included from C++; allocation and unexpected exceptions are
translated at the boundary. Error text is deterministic static text copied into caller storage;
there is no `errno`, thread-local error buffer, allocator crossing, or callback.

Default symbol visibility is hidden. `abi/symbols-v1.txt` is the exact export allowlist and both the
link step and `engineering/tools/check_abi_symbols.py` consume it. Any missing or unexpected
`gs_` symbol fails the ABI gate.
