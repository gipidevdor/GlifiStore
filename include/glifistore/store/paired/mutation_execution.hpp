/**
 * @file
 * @brief Synchronous paired execution helpers preserving commit/publication ordering.
 */
#pragma once

// Wire polarity helpers for Store mutate failures (behavior-neutral extraction).
// Normative: docs/spec/error-taxonomy-v1.md

#include "glifistore/core/error.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/store/paired/shard_pair_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace glifistore {
class Store;
}

namespace glifistore::store::paired {

/// Known-not-committed durable/volatile failures must not keep Reactor
/// INTERNAL_ERROR-bucket codes. Sticky committed/indeterminate paths rewrite to
/// unavailable before this runs — callers must not pass unavailable here.
void rewrite_known_not_committed_wire_error(Error& error) noexcept;

/// Volatile put/erase_locked_published: append-failed → rewrite known-not-committed;
/// post-append (unavailable) → sticky indeterminate. Never rewrite unavailable.
[[nodiscard]] auto classify_volatile_mutation_error(Error error, bool& sticky_indeterminate) noexcept
    -> Error;

/// Durable single-op with one conflict retry (as-implemented Writer loop).
[[nodiscard]] auto execute_durable_single(Store& store, std::size_t shard, MutationKind kind,
                                          const HashedKey& key, std::span<const std::byte> value,
                                          std::uint64_t expire_at_ns, ValueType type = ValueType::bytes)
    -> DurableMutationResult;

} // namespace glifistore::store::paired
