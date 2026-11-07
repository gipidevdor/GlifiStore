#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/index/swiss_control_group.hpp"
#include "glifistore/index/swiss_table.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace glifistore {
namespace detail {

// Per-Worker durable hot-cache table: Swiss-style flat open addressing with
// 8-slot control groups, H2 fingerprints, and SIMD/scalar group matching via
// swiss_control_group.hpp. Uses the same FNV-1a key hash as Index routing; hash
// alone is never identity — full key bytes are compared on every candidate slot.
// Values ≤ kInlineValueBytes live in the slot; larger values use a shared heap buffer.
struct HotRecordEntry {
    static constexpr std::size_t kInlineValueBytes = 48;

    RecordRef reference{};
    std::shared_ptr<const std::byte[]> heap_value{};
    alignas(std::max_align_t) std::byte inline_value[kInlineValueBytes]{};
    std::size_t value_size{};
    std::uint64_t expire_at_ns{};

    [[nodiscard]] auto is_inline() const noexcept -> bool {
        return value_size <= kInlineValueBytes;
    }

    [[nodiscard]] auto value_span() const noexcept -> std::span<const std::byte> {
        if (value_size == 0) {
            return {};
        }
        if (is_inline()) {
            return {inline_value, value_size};
        }
        return {heap_value.get(), value_size};
    }
};

struct HotRecordSnapshot {
    std::shared_ptr<const std::byte[]> heap_value{};
    alignas(std::max_align_t) std::byte inline_value[HotRecordEntry::kInlineValueBytes]{};
    std::size_t value_size{};
    SequenceNumber sequence{};
    std::uint64_t expire_at_ns{};

    [[nodiscard]] auto is_inline() const noexcept -> bool {
        return value_size <= HotRecordEntry::kInlineValueBytes;
    }

    [[nodiscard]] auto value_span() const noexcept -> std::span<const std::byte> {
        if (value_size == 0) {
            return {};
        }
        if (is_inline()) {
            return {inline_value, value_size};
        }
        return {heap_value.get(), value_size};
    }

    [[nodiscard]] static auto from_entry(const HotRecordEntry& entry) -> HotRecordSnapshot {
        HotRecordSnapshot snapshot{.value_size = entry.value_size,
                                   .sequence = entry.reference.sequence,
                                   .expire_at_ns = entry.expire_at_ns};
        if (entry.is_inline()) {
            std::copy_n(entry.inline_value, entry.value_size, snapshot.inline_value);
        } else {
            snapshot.heap_value = entry.heap_value;
        }
        return snapshot;
    }
};

class HotRecordTable final {
  public:
    static constexpr float kMaxLoadFactor = 0.75f;
    static constexpr std::size_t kMinimumCapacity = 64;
    static constexpr std::size_t kLoadDenominator = 4;

    // Keep disabled and never-used Worker caches allocation-free. The first
    // successful reserve/insert materializes kMinimumCapacity buckets.
    HotRecordTable() = default;

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return size_;
    }

    [[nodiscard]] auto capacity() const noexcept -> std::size_t {
        return capacity_;
    }

    [[nodiscard]] auto bucket_count() const noexcept -> std::size_t {
        return capacity_;
    }

    [[nodiscard]] auto tombstone_count() const noexcept -> std::size_t {
        return tombstones_;
    }

    [[nodiscard]] auto find(const std::string_view key, const std::uint64_t key_hash) noexcept
        -> HotRecordEntry* {
        if (capacity_ == 0) {
            return nullptr;
        }
        const auto fingerprint = h2(key_hash);
        auto group_start = probe_start(key_hash);
        for (std::size_t probed = 0; probed < capacity_; probed += kSwissGroupSize) {
            const auto* const group = &control_[group_start];
            const auto control_word = load_control_group64(group);
            const auto fingerprint_mask = equal_byte_mask(group, fingerprint);
            for (std::size_t offset = 0; offset < kSwissGroupSize; ++offset) {
                const auto control = control_byte_at(control_word, offset);
                if (control == kSwissEmpty) {
                    return nullptr;
                }
                if ((fingerprint_mask & (1ULL << offset)) == 0) {
                    continue;
                }
                const auto index = group_start + offset;
                if (hashes_[index] == key_hash && keys_[index] == key) {
                    return &entries_[index];
                }
            }
            group_start = next_group(group_start);
        }
        return nullptr;
    }

    [[nodiscard]] auto find(const std::string_view key, const std::uint64_t key_hash) const noexcept
        -> const HotRecordEntry* {
        return const_cast<HotRecordTable*>(this)->find(key, key_hash);
    }

    [[nodiscard]] auto find(const HashedKey& key) noexcept -> HotRecordEntry* {
        return find(key.key, key.hash);
    }

    // Insert or replace. Grows when occupancy would exceed the load factor.
    [[nodiscard]] auto insert_or_assign(std::string key, const std::uint64_t key_hash, HotRecordEntry entry)
        -> Status {
        // Replacing at the occupancy boundary must not double the table.
        if (auto* existing = find(key, key_hash); existing != nullptr) {
            *existing = std::move(entry);
            return {};
        }
        if (auto grown = grow_for_insert(); !grown) {
            return grown;
        }
        const auto fingerprint = h2(key_hash);
        auto group_start = probe_start(key_hash);
        std::optional<std::size_t> tomb;
        for (std::size_t probed = 0; probed < capacity_; probed += kSwissGroupSize) {
            const auto* const group = &control_[group_start];
            const auto control_word = load_control_group64(group);
            for (std::size_t offset = 0; offset < kSwissGroupSize; ++offset) {
                const auto index = group_start + offset;
                const auto control = control_byte_at(control_word, offset);
                if (control == kSwissEmpty) {
                    place_new(tomb.value_or(index), std::move(key), key_hash, fingerprint, std::move(entry));
                    return {};
                }
                if (control == kSwissDeleted && !tomb) {
                    tomb = index;
                }
            }
            group_start = next_group(group_start);
        }
        if (!tomb) {
            return fail(ErrorCode::internal_error, "hot-cache insert probed a full table");
        }
        place_new(*tomb, std::move(key), key_hash, fingerprint, std::move(entry));
        return {};
    }

    // Erase by key. Returns true when a resident entry was removed.
    auto erase(const std::string_view key, const std::uint64_t key_hash) noexcept -> bool {
        if (capacity_ == 0) {
            return false;
        }
        const auto fingerprint = h2(key_hash);
        auto group_start = probe_start(key_hash);
        for (std::size_t probed = 0; probed < capacity_; probed += kSwissGroupSize) {
            const auto* const group = &control_[group_start];
            const auto control_word = load_control_group64(group);
            const auto fingerprint_mask = equal_byte_mask(group, fingerprint);
            for (std::size_t offset = 0; offset < kSwissGroupSize; ++offset) {
                const auto control = control_byte_at(control_word, offset);
                if (control == kSwissEmpty) {
                    return false;
                }
                if ((fingerprint_mask & (1ULL << offset)) == 0) {
                    continue;
                }
                const auto index = group_start + offset;
                if (hashes_[index] == key_hash && keys_[index] == key) {
                    clear_slot(index);
                    return true;
                }
            }
            group_start = next_group(group_start);
        }
        return false;
    }

    auto erase(const HashedKey& key) noexcept -> bool {
        return erase(key.key, key.hash);
    }

    // Reserve capacity for at least `entries` live records at the configured load factor.
    [[nodiscard]] auto reserve(const std::size_t entries) -> Status {
        const auto needed = minimum_capacity_for(entries);
        if (!needed) {
            return unexpected(needed.error());
        }
        if (*needed <= capacity_) {
            return {};
        }
        return rehash(*needed);
    }

    template <typename Predicate> void erase_if(Predicate&& predicate) {
        for (std::size_t index = 0; index < capacity_; ++index) {
            if (!is_full(control_[index])) {
                continue;
            }
            if (predicate(keys_[index], hashes_[index], entries_[index])) {
                clear_slot(index);
            }
        }
    }

    template <typename Callback> void for_each(Callback&& callback) const {
        for (std::size_t index = 0; index < capacity_; ++index) {
            if (is_full(control_[index])) {
                callback(keys_[index], hashes_[index], entries_[index]);
            }
        }
    }

    void clear() noexcept {
        for (std::size_t index = 0; index < capacity_; ++index) {
            if (is_full(control_[index])) {
                keys_[index].clear();
                keys_[index].shrink_to_fit();
                entries_[index] = HotRecordEntry{};
            }
            control_[index] = kSwissEmpty;
            hashes_[index] = 0;
        }
        size_ = 0;
        tombstones_ = 0;
    }

  private:
    [[nodiscard]] static auto h2(const std::uint64_t hash) noexcept -> std::uint8_t {
        const auto fingerprint = static_cast<std::uint8_t>(hash & 0x7FU);
        return fingerprint == 0 ? static_cast<std::uint8_t>(1) : fingerprint;
    }

    [[nodiscard]] static auto is_full(const std::uint8_t control) noexcept -> bool {
        return control != kSwissEmpty && control != kSwissDeleted;
    }

    [[nodiscard]] auto probe_start(const std::uint64_t hash) const noexcept -> std::size_t {
        const auto group_index = (hash >> 7U) & ((capacity_ / kSwissGroupSize) - 1U);
        return group_index * kSwissGroupSize;
    }

    [[nodiscard]] auto next_group(const std::size_t group_start) const noexcept -> std::size_t {
        const auto next = group_start + kSwissGroupSize;
        return next >= capacity_ ? next - capacity_ : next;
    }

    [[nodiscard]] static auto normalize_capacity(std::size_t minimum) -> Result<std::size_t> {
        if (minimum < kMinimumCapacity) {
            minimum = kMinimumCapacity;
        }
        std::size_t capacity = kMinimumCapacity;
        while (capacity < minimum) {
            if (capacity > std::numeric_limits<std::size_t>::max() / 2U) {
                return fail(ErrorCode::arithmetic_overflow, "hot-cache capacity overflow");
            }
            capacity *= 2U;
        }
        return capacity;
    }

    [[nodiscard]] static auto minimum_capacity_for(const std::size_t entries) -> Result<std::size_t> {
        auto capacity = normalize_capacity(entries);
        if (!capacity) {
            return unexpected(capacity.error());
        }
        while (maximum_occupancy_for(*capacity) < entries) {
            if (*capacity > std::numeric_limits<std::size_t>::max() / 2U) {
                return fail(ErrorCode::arithmetic_overflow, "hot-cache reserve overflow");
            }
            *capacity *= 2U;
        }
        return capacity;
    }

    [[nodiscard]] static constexpr auto maximum_occupancy_for(const std::size_t capacity) noexcept
        -> std::size_t {
        return capacity - capacity / kLoadDenominator;
    }

    [[nodiscard]] auto maximum_occupancy() const noexcept -> std::size_t {
        return maximum_occupancy_for(capacity_);
    }

    [[nodiscard]] auto grow_for_insert() -> Status {
        if (size_ < maximum_occupancy()) {
            // Rebuild when tombstones dominate probes.
            if (tombstones_ > maximum_occupancy() && size_ + tombstones_ > (capacity_ * 3U) / 4U) {
                return rehash(capacity_);
            }
            return {};
        }
        if (capacity_ > std::numeric_limits<std::size_t>::max() / 2U) {
            return fail(ErrorCode::arithmetic_overflow, "hot-cache growth overflow");
        }
        return rehash(capacity_ == 0 ? kMinimumCapacity : capacity_ * 2U);
    }

    [[nodiscard]] auto rehash(const std::size_t new_capacity) -> Status {
        auto normalized = normalize_capacity(new_capacity);
        if (!normalized) {
            return unexpected(normalized.error());
        }
        std::vector<std::uint8_t> control(*normalized, kSwissEmpty);
        std::vector<std::uint64_t> hashes(*normalized, 0);
        std::vector<std::string> keys(*normalized);
        std::vector<HotRecordEntry> entries(*normalized);
        const auto group_mask = (*normalized / kSwissGroupSize) - 1U;
        for (std::size_t index = 0; index < capacity_; ++index) {
            if (!is_full(control_[index])) {
                continue;
            }
            const auto key_hash = hashes_[index];
            const auto fingerprint = h2(key_hash);
            auto group_start = ((key_hash >> 7U) & group_mask) * kSwissGroupSize;
            for (;;) {
                bool placed = false;
                for (std::size_t offset = 0; offset < kSwissGroupSize; ++offset) {
                    const auto slot = group_start + offset;
                    if (control[slot] == kSwissEmpty) {
                        control[slot] = fingerprint;
                        hashes[slot] = key_hash;
                        keys[slot] = std::move(keys_[index]);
                        entries[slot] = std::move(entries_[index]);
                        placed = true;
                        break;
                    }
                }
                if (placed) {
                    break;
                }
                group_start += kSwissGroupSize;
                if (group_start >= *normalized) {
                    group_start -= *normalized;
                }
            }
        }
        control_ = std::move(control);
        hashes_ = std::move(hashes);
        keys_ = std::move(keys);
        entries_ = std::move(entries);
        capacity_ = *normalized;
        tombstones_ = 0;
        return {};
    }

    void place_new(const std::size_t slot, std::string key, const std::uint64_t key_hash,
                   const std::uint8_t fingerprint, HotRecordEntry entry) noexcept {
        if (control_[slot] == kSwissDeleted) {
            --tombstones_;
        }
        control_[slot] = fingerprint;
        hashes_[slot] = key_hash;
        keys_[slot] = std::move(key);
        entries_[slot] = std::move(entry);
        ++size_;
    }

    void clear_slot(const std::size_t index) noexcept {
        control_[index] = kSwissDeleted;
        hashes_[index] = 0;
        keys_[index].clear();
        keys_[index].shrink_to_fit();
        entries_[index] = HotRecordEntry{};
        --size_;
        ++tombstones_;
    }

    std::size_t size_{};
    std::size_t tombstones_{};
    std::size_t capacity_{};
    std::vector<std::uint8_t> control_;
    std::vector<std::uint64_t> hashes_;
    std::vector<std::string> keys_;
    std::vector<HotRecordEntry> entries_;
};

[[nodiscard]] inline auto hot_record_slot_bytes() noexcept -> std::uint64_t {
    // Control + hash + SSO string + entry. Conservative upper bound used for budgets.
    constexpr auto per_slot = std::uint64_t{sizeof(std::uint8_t)} + std::uint64_t{sizeof(std::uint64_t)} +
                              std::uint64_t{sizeof(std::string)} + std::uint64_t{sizeof(HotRecordEntry)};
    return per_slot;
}

[[nodiscard]] inline auto hot_record_accounted_bytes_saturated(const std::size_t key_bytes,
                                                               const std::size_t value_bytes) noexcept
    -> std::uint64_t {
    const auto with_key = static_cast<std::uint64_t>(key_bytes);
    const auto charged_value =
        value_bytes <= HotRecordEntry::kInlineValueBytes ? 0U : static_cast<std::uint64_t>(value_bytes);
    return charged_value > std::numeric_limits<std::uint64_t>::max() - with_key
               ? std::numeric_limits<std::uint64_t>::max()
               : with_key + charged_value;
}

[[nodiscard]] inline auto hot_record_accounted_bytes(const std::size_t key_bytes,
                                                     const std::size_t value_bytes) -> Result<std::uint64_t> {
    // Per-entry external payload only. The flat slot arrays are charged once
    // for the complete table by hot_cache_table_bytes(); including fixed slot
    // bytes here would double-account every resident entry. Charging complete
    // key length remains conservative because short keys may use string SSO.
    const auto charged_value =
        value_bytes <= HotRecordEntry::kInlineValueBytes ? 0U : static_cast<std::uint64_t>(value_bytes);
    const auto with_key = static_cast<std::uint64_t>(key_bytes);
    if (charged_value > std::numeric_limits<std::uint64_t>::max() - with_key) {
        return fail(ErrorCode::arithmetic_overflow, "hot-cache value accounting overflow");
    }
    return hot_record_accounted_bytes_saturated(key_bytes, value_bytes);
}

struct HotRecordReservePlan {
    std::size_t target{};
    bool overflow{};
};

[[nodiscard]] constexpr auto plan_hot_record_reserve(const std::size_t current_size,
                                                     const std::size_t additional_records,
                                                     const std::size_t current_capacity) noexcept
    -> HotRecordReservePlan {
    if (additional_records > std::numeric_limits<std::size_t>::max() - current_size) {
        return {.overflow = true};
    }
    const auto required = current_size + additional_records;
    if (additional_records == 0 ||
        required <= current_capacity - current_capacity / HotRecordTable::kLoadDenominator) {
        return {};
    }
    auto target = std::max(current_capacity, HotRecordTable::kMinimumCapacity);
    while (required > target - target / HotRecordTable::kLoadDenominator) {
        if (target > std::numeric_limits<std::size_t>::max() / 2U) {
            return {.overflow = true};
        }
        target *= 2U;
    }
    return {.target = target};
}

} // namespace detail
} // namespace glifistore
