#pragma once

#include "glifistore/core/key_hash.hpp"
#include "glifistore/index/index.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace glifistore::runtime_catalog_detail {

// One compaction owns this preallocated journal. Index publishers append under
// the same ownership as the mutable Index; the compactor reads only after its
// commit gate has drained the exclusive Writer. Overflow abandons maintenance,
// never the foreground mutation. Values are not copied into this journal.
/**
 * @brief Bounded metadata-only journal bridging compaction snapshot and commit gate.
 *
 * The mutable Index owner alone appends entries; the compactor alone reads them
 * after draining that owner. The release/acquire overflow flag reports exhaustion;
 * it does not make the non-atomic entries safe for concurrent replay. Overflow
 * abandons this compaction attempt while foreground mutation continues.
 */
class CompactionDelta final {
    struct Entry {
        std::size_t offset{};
        std::size_t size{};
        std::uint64_t hash{};
        std::optional<RecordRef> reference;
    };

  public:
    CompactionDelta(const std::size_t entries, const std::size_t key_bytes)
        : entries_(entries), keys_(key_bytes) {}

    /**
     * @brief Copy one key and its latest reference into preallocated journal storage.
     * @param key Borrowed bytes copied before return; the routing hash is retained.
     * @param reference New active-Segment reference, or nullopt for removal.
     * @note No allocation occurs. Entry/key exhaustion sets sticky overflow and stops recording.
     */
    void record(const HashedKey& key, const std::optional<RecordRef>& reference) noexcept {
        if (overflow_.load(std::memory_order_relaxed)) {
            return;
        }
        if (size_ == entries_.size() || key.key.size() > keys_.size() - key_bytes_) {
            overflow_.store(true, std::memory_order_release);
            return;
        }
        if (!key.key.empty()) {
            std::memcpy(keys_.data() + key_bytes_, key.key.data(), key.key.size());
        }
        entries_[size_++] = {
            .offset = key_bytes_, .size = key.key.size(), .hash = key.hash, .reference = reference};
        key_bytes_ += key.key.size();
    }

    [[nodiscard]] auto overflowed() const noexcept -> bool {
        return overflow_.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return size_;
    }

    [[nodiscard]] auto key_bytes() const noexcept -> std::size_t {
        return key_bytes_;
    }

    [[nodiscard]] auto metadata_bytes() const noexcept -> std::uint64_t {
        return entries_.capacity() * sizeof(Entry) + keys_.capacity();
    }

    // Reserve the worst-case replay before intent publication. Erases never
    // compact the table, and total inserted key bytes cannot exceed this arena.
    /**
     * @brief Reserve worst-case replay capacity in a private replacement Index before intent.
     * @note Later replay must remain owner-serialized and must not grow tables or key storage.
     */
    [[nodiscard]] auto prepare(Index& index) const -> Status {
        return index.prepare_batch_insert(entries_.size(), keys_.size());
    }

    /**
     * @brief Replay journal order into the private replacement Index and update live-byte totals.
     * @pre The commit gate has drained Index publishers and journal recording has stopped.
     * @pre prepare() succeeded and active_segment still identifies the snapshot's active Segment.
     * @return Success, or overflow/identity/accounting failure requiring compaction abandonment.
     * @note Failure may leave the private Index/counters partially updated; discard them.
     * No rollback of foreground mutations or authoritative catalog state occurs here.
     */
    [[nodiscard]] auto replay(Index& index, const SegmentId active_segment, std::uint64_t& active_bytes,
                              std::uint64_t& sealed_bytes) const -> Status {
        if (overflowed()) {
            return fail(ErrorCode::sequence_conflict, "compaction delta budget exhausted");
        }
        for (std::size_t cursor = 0; cursor < size_; ++cursor) {
            const auto& entry = entries_[cursor];
            const auto key_view = keys_.empty() ? std::string_view{}
                                                : std::string_view{keys_.data() + entry.offset, entry.size};
            const HashedKey key{.key = key_view, .hash = entry.hash};
            const auto previous = index.find(key);
            if (previous) {
                auto& bytes = previous->segment_id == active_segment ? active_bytes : sealed_bytes;
                if (bytes < previous->size.value) {
                    return fail(ErrorCode::corrupted_data, "compaction delta live bytes underflow");
                }
                bytes -= previous->size.value;
            }
            if (entry.reference) {
                if (entry.reference->segment_id != active_segment) {
                    return fail(ErrorCode::sequence_conflict, "compaction delta changed active identity");
                }
                if (entry.reference->size.value > std::numeric_limits<std::uint64_t>::max() - active_bytes) {
                    return fail(ErrorCode::arithmetic_overflow, "compaction delta live bytes overflow");
                }
                auto inserted = index.insert_or_assign(key, *entry.reference);
                if (!inserted) {
                    return unexpected(inserted.error());
                }
                active_bytes += entry.reference->size.value;
            } else {
                static_cast<void>(index.erase_no_compact(key));
            }
        }
        return {};
    }

  private:
    std::vector<Entry> entries_;
    std::vector<char> keys_;
    std::size_t size_{};
    std::size_t key_bytes_{};
    std::atomic_bool overflow_{};
};

} // namespace glifistore::runtime_catalog_detail
