/**
 * @file
 * @brief Bounded mutation grouping helpers for externally serialized Writer execution.
 */
#pragma once

// Sync/async mutation batching helpers (behavior-neutral extraction).
// Normative: docs/spec/mutation-lifecycle.md — FIFO, ≤32 publication chunks.

#include <cstddef>
#include <utility>

namespace glifistore::store::paired {

inline constexpr std::size_t kMaximumPublicationBatch = 32U;

// Advance [begin, end) over same-key-distinct items for durable_group sub-batches.
// Stops before the first key that duplicates an earlier key in the window.
template <typename KeyFn>
[[nodiscard]] auto durable_subbatch_end(const std::size_t begin, const std::size_t size, KeyFn&& key_at)
    -> std::size_t {
    std::size_t end = begin;
    for (; end < size; ++end) {
        bool duplicate = false;
        for (std::size_t previous = begin; previous < end; ++previous) {
            const auto& left = key_at(previous);
            const auto& right = key_at(end);
            duplicate = left.hash == right.hash && left.key == right.key;
            if (duplicate) {
                break;
            }
        }
        if (duplicate) {
            break;
        }
    }
    return end;
}

/// Cap a sync volatile publication chunk at kMaximumPublicationBatch.
[[nodiscard]] constexpr auto sync_publication_chunk_cap(const std::size_t remaining) noexcept -> std::size_t {
    return remaining < kMaximumPublicationBatch ? remaining : kMaximumPublicationBatch;
}

} // namespace glifistore::store::paired
