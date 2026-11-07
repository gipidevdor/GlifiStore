/**
 * @file
 * @brief Flat open-addressing Index with inline keys and bounded-capacity preparation.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/index/index_hash_seed.hpp"
#include "glifistore/index/index_types.hpp"
#include "glifistore/index/key_arena.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace glifistore {

inline constexpr std::size_t kSwissGroupSize = 8;
inline constexpr std::uint8_t kSwissEmpty = 0x80;
inline constexpr std::uint8_t kSwissDeleted = 0xFE;
inline constexpr float kSwissMaxLoadFactor = 7.0f / 8.0f;
inline constexpr std::size_t kSwissInlineKeyBytes = 24;

/// Production Index partition: flat open-addressing table in the SwissTable layout.
class SwissTableIndex final {
  public:
    /// Uses process Index hash seed (ADR 0026) unless an explicit seed is passed.
    explicit SwissTableIndex(std::uint64_t seed = get_index_hash_seed());
    SwissTableIndex(WorkerRoutingState routing, std::uint64_t seed = get_index_hash_seed());

    [[nodiscard]] auto seed() const noexcept -> std::uint64_t {
        return seed_;
    }

    /**
     * @brief Look up an exact binary key without taking a lock; the owner supplies serialization.
     */
    [[nodiscard]] auto find(std::string_view key) const -> std::optional<RecordRef>;
    /**
     * @brief Look up an exact binary key without taking a lock; the owner supplies serialization.
     */
    [[nodiscard]] auto find(const HashedKey& key) const -> std::optional<RecordRef>;
    /**
     * @brief Publish a physical reference and return any displaced reference; no Segment pin is created.
     */
    [[nodiscard]] auto insert_or_assign(std::string_view key, RecordRef ref) -> Result<IndexMutationResult>;
    /**
     * @brief Publish a physical reference and return any displaced reference; no Segment pin is created.
     */
    [[nodiscard]] auto insert_or_assign(const HashedKey& key, RecordRef ref) -> Result<IndexMutationResult>;
    /**
     * @brief Remove an Index entry and return its former reference, if present.
     */
    auto erase(std::string_view key) -> IndexMutationResult;
    /**
     * @brief Remove an Index entry and return its former reference, if present.
     */
    auto erase(const HashedKey& key) -> IndexMutationResult;
    /**
     * @brief Erase without rebuilding table/arena storage, for allocation-prepared replay.
     */
    auto erase_no_compact(const HashedKey& key) -> IndexMutationResult;
    /**
     * @brief Reserve table capacity for count entries; capacity/allocation errors remain explicit.
     */
    [[nodiscard]] auto reserve(std::size_t count) -> Status;
    /// Prepares every allocation needed to insert this key. With external
    /// serialization, the subsequent insert_or_assign cannot grow the table or
    /// the heap-key arena.
    [[nodiscard]] auto prepare_insert(const HashedKey& key) -> Status;
    /// Reserves the aggregate table and heap-key capacity required by staged
    /// inserts that will be published later as one externally serialized batch.
    [[nodiscard]] auto prepare_batch_insert(std::size_t additional_entries,
                                            std::size_t additional_heap_key_bytes) -> Status;
    /**
     * @brief Allocate an owning key/reference snapshot; references still require independent Segment lifetime
     * ownership.
     */
    [[nodiscard]] auto entries() const -> std::vector<IndexEntry>;
    [[nodiscard]] auto stats() const noexcept -> IndexStats;
    [[nodiscard]] auto clone_empty() const -> SwissTableIndex;

  private:
    struct Slot {
        static constexpr std::uint32_t kInlineKeyMask = 1U << 31U;

        union KeyStorage {
            std::byte inline_key[kSwissInlineKeyBytes];
            std::uint32_t heap_key_offset;

            constexpr KeyStorage() noexcept : inline_key{} {}
        };

        RecordRef ref{};
        KeyStorage key;
        std::uint32_t key_hash_tag{};
        std::uint32_t key_size_and_mode{};

        Slot() = default;
        Slot(Slot&&) = default;
        auto operator=(Slot&&) -> Slot& = default;
        Slot(const Slot&) = delete;
        auto operator=(const Slot&) -> Slot& = delete;

        [[nodiscard]] auto key_size() const noexcept -> std::uint32_t {
            return key_size_and_mode & ~kInlineKeyMask;
        }

        [[nodiscard]] auto key_is_inline() const noexcept -> bool {
            return (key_size_and_mode & kInlineKeyMask) != 0;
        }

        void set_key_metadata(const std::uint32_t size, const bool is_inline) noexcept {
            key_size_and_mode = size | (is_inline ? kInlineKeyMask : 0U);
        }
    };

    static_assert(sizeof(Slot) == 64, "Swiss slot memory amplification regression");

    [[nodiscard]] auto mix_hash(std::uint64_t key_hash) const noexcept -> std::uint64_t;
    [[nodiscard]] static auto h2(std::uint64_t hash) noexcept -> std::uint8_t;
    [[nodiscard]] auto probe_start(std::uint64_t hash) const noexcept -> std::size_t;
    [[nodiscard]] static auto next_group(std::size_t group_start, std::size_t capacity) noexcept
        -> std::size_t;
    [[nodiscard]] auto key_equals(const Slot& slot, std::string_view key,
                                  std::uint64_t key_hash) const noexcept -> bool;
    [[nodiscard]] auto slot_key(const Slot& slot) const noexcept -> std::string_view;
    [[nodiscard]] auto find_slot_index(std::string_view key, std::uint64_t key_hash) const
        -> std::optional<std::size_t>;
    [[nodiscard]] auto find_insert_index(std::string_view key, std::uint64_t key_hash,
                                         std::uint64_t mixed_hash) -> Result<std::size_t>;
    [[nodiscard]] auto rehash(std::size_t new_capacity, bool force_same_capacity = false) -> Status;
    [[nodiscard]] auto set_key(Slot& slot, std::string_view key, std::uint64_t key_hash) -> Status;
    void clear_slot(std::size_t index);
    [[nodiscard]] auto compact_heap_keys(std::span<Slot> slots, std::span<const std::uint8_t> control)
        -> Status;
    [[nodiscard]] auto maybe_compact_heap_keys() -> Status;
    [[nodiscard]] auto grow_if_needed() -> Status;
    [[nodiscard]] auto effective_occupancy() const noexcept -> std::size_t;
    [[nodiscard]] auto maximum_occupancy() const noexcept -> std::size_t;
    [[nodiscard]] auto tombstone_rebuild_beneficial() const noexcept -> bool;
    void observe_probe(std::size_t groups) const noexcept;
    [[nodiscard]] static auto normalize_capacity(std::size_t minimum_slots) -> Result<std::size_t>;

    WorkerRoutingState routing_{};
    std::uint64_t seed_;
    std::size_t size_{};
    std::size_t deleted_count_{};
    std::size_t capacity_{};
    std::size_t heap_live_bytes_{};
    std::size_t heap_dead_bytes_{};
    std::vector<std::uint8_t> control_;
    std::vector<Slot> slots_;
    KeyArena heap_keys_;
    mutable std::size_t maximum_probe_groups_{};
    std::uint64_t rehash_count_{};
    std::uint64_t tombstone_rebuild_count_{};
};

} // namespace glifistore
