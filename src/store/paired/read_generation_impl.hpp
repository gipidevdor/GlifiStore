#pragma once

// Internal types for PairReadGeneration translation units.
// Not installed; behavior-neutral extraction (Phase C decomposition).

#include "glifistore/core/hot_path_phases.hpp"
#include "glifistore/store/paired/read_generation.hpp"
#include "store/paired/read_generation_immutable.hpp"
#include "store/paired/read_generation_internals.hpp"

#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace glifistore::store::paired {

class IncrementalBaseBuilder final {
  public:
    IncrementalBaseBuilder(const std::size_t maximum_records,
                           std::shared_ptr<LargeImmutableMappingPool> mapping_pool)
        : index_(std::make_unique<ImmutableReadIndex>(std::move(mapping_pool))) {
        index_->initialize(maximum_records, false);
    }

    [[nodiscard]] auto initialize_next(const std::size_t maximum_slots) noexcept -> std::size_t {
        const auto count = std::min(maximum_slots, index_->capacity_ - initialized_slots_);
        index_->initialize_control_slots(initialized_slots_, count);
        initialized_slots_ += count;
        return count;
    }

    [[nodiscard]] auto initialized() const noexcept -> bool {
        return initialized_slots_ == index_->capacity_;
    }

    [[nodiscard]] auto remaining_initialization_slots() const noexcept -> std::size_t {
        return index_->capacity_ - initialized_slots_;
    }

    [[nodiscard]] auto contains(const HashedKey& key) const noexcept -> bool {
        return index_->find(key) != nullptr;
    }

    void insert(const ReadRecord& record) {
        index_->append(record);
    }

    void insert(const ReadRecordView& record) {
        index_->append(record);
    }

    [[nodiscard]] auto freeze() && -> std::shared_ptr<const ImmutableReadIndex> {
        return std::shared_ptr<const ImmutableReadIndex>{index_.release()};
    }

  private:
    std::unique_ptr<ImmutableReadIndex> index_;
    std::size_t initialized_slots_{};
};

class DeltaState final {
  public:
    DeltaState(const std::size_t capacity, const std::size_t maximum_entries, const std::size_t size,
               const std::size_t allocated_page_count, const std::size_t allocated_block_count,
               const std::size_t allocated_chunk_count,
               std::vector<std::shared_ptr<const DeltaPage>> flat_pages,
               std::vector<std::shared_ptr<const DeltaDirectoryChunk>> directory_chunks,
               std::shared_ptr<DeltaArena> primary_arena, std::shared_ptr<DeltaArena> secondary_arena = {})
        : capacity_(capacity), maximum_entries_(maximum_entries), size_(size),
          allocated_page_count_(allocated_page_count), allocated_block_count_(allocated_block_count),
          allocated_chunk_count_(allocated_chunk_count), flat_pages_(std::move(flat_pages)),
          directory_chunks_(std::move(directory_chunks)), primary_arena_(std::move(primary_arena)),
          secondary_arena_(std::move(secondary_arena)) {
        if (!primary_arena_) {
            throw std::invalid_argument{"Delta state has no primary record arena"};
        }
    }

    [[nodiscard]] auto find_handle(const HashedKey& key) const noexcept -> const DeltaRecord* {
        return find(key);
    }

    [[nodiscard]] auto find(const HashedKey& key) const noexcept -> const DeltaRecord* {
        const auto wanted = fingerprint(key.hash);
        auto group_start = probe_start(key.hash);
        for (std::size_t probed = 0; probed < capacity_; probed += kSwissGroupSize) {
            const auto page_index = group_start / kDeltaPageSlots;
            const auto page_offset = group_start % kDeltaPageSlots;
            const auto* page = page_at(page_index);
            if (page == nullptr) {
                return nullptr;
            }
            const auto* group = &page->control[page_offset];
            const auto control_word = detail::load_control_group64(group);
            const auto matches = detail::equal_byte_mask(group, wanted);
            for (std::size_t offset = 0; offset < kSwissGroupSize; ++offset) {
                const auto control = detail::control_byte_at(control_word, offset);
                if (control == kSwissEmpty) {
                    return nullptr;
                }
                if ((matches & (1ULL << offset)) == 0) {
                    continue;
                }
                const auto slot = page_offset + offset;
                const auto* record = page->records[slot];
                if (page->hashes[slot] == key.hash && delta_record_key(*record) == key.key) {
                    return record;
                }
            }
            group_start = next_group(group_start);
        }
        return nullptr;
    }

    template <typename Callback> void for_each(Callback&& callback) const {
        const auto page_count = capacity_ / kDeltaPageSlots;
        for (std::size_t page_index = 0; page_index < page_count; ++page_index) {
            const auto* page = page_at(page_index);
            if (page == nullptr) {
                continue;
            }
            for (std::size_t slot = 0; slot < kDeltaPageSlots; ++slot) {
                if (page->control[slot] != kSwissEmpty) {
                    callback(delta_record_view(*page->records[slot], page->hashes[slot]));
                }
            }
        }
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return size_;
    }

    [[nodiscard]] auto capacity() const noexcept -> std::size_t {
        return capacity_;
    }

    [[nodiscard]] auto maximum_entries() const noexcept -> std::size_t {
        return maximum_entries_;
    }

    [[nodiscard]] auto record_versions() const noexcept -> std::size_t {
        return allocation_arena()->record_count();
    }

    [[nodiscard]] auto available_record_versions() const noexcept -> std::size_t {
        return allocation_arena()->available_records();
    }

    [[nodiscard]] auto arena_record_bytes() const noexcept -> std::size_t {
        auto result = primary_arena_->allocated_record_bytes();
        if (secondary_arena_) {
            result += secondary_arena_->allocated_record_bytes();
        }
        return result;
    }

    [[nodiscard]] auto arena_key_bytes() const noexcept -> std::size_t {
        auto result = primary_arena_->key_bytes();
        if (secondary_arena_) {
            result += secondary_arena_->key_bytes();
        }
        return result;
    }

    [[nodiscard]] auto arena_key_storage_bytes() const noexcept -> std::size_t {
        auto result = primary_arena_->allocated_key_bytes();
        if (secondary_arena_) {
            result += secondary_arena_->allocated_key_bytes();
        }
        return result;
    }

    [[nodiscard]] auto record_at(const std::size_t slot) const noexcept -> std::optional<ReadRecordView> {
        if (slot >= capacity_) {
            return std::nullopt;
        }
        const auto* page = page_at(slot / kDeltaPageSlots);
        const auto offset = slot % kDeltaPageSlots;
        return page != nullptr && page->control[offset] != kSwissEmpty
                   ? std::optional<ReadRecordView>{delta_record_view(*page->records[offset],
                                                                     page->hashes[offset])}
                   : std::nullopt;
    }

    void append_memory_stats(ReadGenerationMemoryStats& stats) const noexcept {
        stats.delta_entries = size_;
        stats.delta_capacity = capacity_;
        stats.delta_record_versions = record_versions();
        stats.delta_arena_record_bytes = arena_record_bytes();
        stats.delta_arena_key_bytes = arena_key_bytes();
        stats.delta_arena_key_storage_bytes = arena_key_storage_bytes();

        auto lookup = saturating_add(
            saturating_multiply(flat_pages_.capacity(), sizeof(std::shared_ptr<const DeltaPage>)),
            saturating_multiply(directory_chunks_.capacity(),
                                sizeof(std::shared_ptr<const DeltaDirectoryChunk>)));
        lookup = saturating_add(lookup, saturating_multiply(allocated_page_count_, sizeof(DeltaPage)));
        lookup =
            saturating_add(lookup, saturating_multiply(allocated_block_count_, sizeof(DeltaDirectoryBlock)));
        lookup =
            saturating_add(lookup, saturating_multiply(allocated_chunk_count_, sizeof(DeltaDirectoryChunk)));
        stats.delta_lookup_storage_bytes = lookup;
        stats.delta_allocated_lower_bound_bytes = saturating_add(
            lookup, saturating_add(stats.delta_arena_record_bytes, stats.delta_arena_key_storage_bytes));
    }

  private:
    [[nodiscard]] auto allocation_arena() const noexcept -> const std::shared_ptr<DeltaArena>& {
        return secondary_arena_ ? secondary_arena_ : primary_arena_;
    }

    [[nodiscard]] auto page_at(const std::size_t page_index) const noexcept -> const DeltaPage* {
        if (directory_chunks_.empty()) {
            return flat_pages_[page_index].get();
        }
        const auto block_index = page_index / kDeltaDirectoryBlockPages;
        const auto& chunk = directory_chunks_[block_index / kDeltaDirectoryChunkBlocks];
        if (!chunk) {
            return nullptr;
        }
        const auto& block = chunk->blocks[block_index % kDeltaDirectoryChunkBlocks];
        return block ? block->pages[page_index % kDeltaDirectoryBlockPages].get() : nullptr;
    }

    [[nodiscard]] auto probe_start(const std::uint64_t hash) const noexcept -> std::size_t {
        return ((hash >> 7U) & ((capacity_ / kSwissGroupSize) - 1U)) * kSwissGroupSize;
    }

    [[nodiscard]] auto next_group(const std::size_t group_start) const noexcept -> std::size_t {
        const auto next = group_start + kSwissGroupSize;
        return next == capacity_ ? 0 : next;
    }

    std::size_t capacity_{};
    std::size_t maximum_entries_{};
    std::size_t size_{};
    // Exact topology census maintained by the Writer while cloning the COW
    // directory. memory_stats() must remain O(1): it runs after every publish.
    std::size_t allocated_page_count_{};
    std::size_t allocated_block_count_{};
    std::size_t allocated_chunk_count_{};
    std::vector<std::shared_ptr<const DeltaPage>> flat_pages_;
    std::vector<std::shared_ptr<const DeltaDirectoryChunk>> directory_chunks_;
    std::shared_ptr<DeltaArena> primary_arena_;
    std::shared_ptr<DeltaArena> secondary_arena_;

    friend class DeltaBuilder;
};

[[nodiscard]] inline auto make_empty_delta(const std::size_t maximum_entries) -> DeltaState {
    if (maximum_entries > std::numeric_limits<std::size_t>::max() - kMaximumPublicationBatch) {
        throw std::bad_alloc{};
    }
    const auto required = maximum_entries + kMaximumPublicationBatch;
    auto capacity = kDeltaPageSlots;
    while (capacity - capacity / 4U < required) {
        if (capacity > std::numeric_limits<std::size_t>::max() / 2U) {
            throw std::bad_alloc{};
        }
        capacity *= 2U;
    }
    const auto page_count = capacity / kDeltaPageSlots;
    auto arena = std::make_shared<DeltaArena>(maximum_entries);
    if (page_count <= kFlatDeltaMaximumPages) {
        return DeltaState{capacity,
                          maximum_entries,
                          0,
                          0,
                          0,
                          0,
                          std::vector<std::shared_ptr<const DeltaPage>>(page_count),
                          std::vector<std::shared_ptr<const DeltaDirectoryChunk>>{},
                          std::move(arena)};
    }
    const auto directory_count = (page_count + kDeltaDirectoryBlockPages - 1U) / kDeltaDirectoryBlockPages;
    const auto chunk_count = (directory_count + kDeltaDirectoryChunkBlocks - 1U) / kDeltaDirectoryChunkBlocks;
    return DeltaState{capacity,
                      maximum_entries,
                      0,
                      0,
                      0,
                      0,
                      std::vector<std::shared_ptr<const DeltaPage>>{},
                      std::vector<std::shared_ptr<const DeltaDirectoryChunk>>(chunk_count),
                      std::move(arena)};
}

class DeltaBuilder final {
  public:
    explicit DeltaBuilder(const DeltaState& previous, DeltaBuilderScratch& scratch,
                          std::shared_ptr<DeltaArena> allocation_arena = {})
        : previous_(&previous), flat_pages_(copy_flat_delta_spine(previous.flat_pages_)),
          directory_chunks_(copy_directory_delta_spine(previous.directory_chunks_)),
          primary_arena_(previous.primary_arena_), secondary_arena_(previous.secondary_arena_),
          scratch_(&scratch), size_(previous.size_), allocated_page_count_(previous.allocated_page_count_),
          allocated_block_count_(previous.allocated_block_count_),
          allocated_chunk_count_(previous.allocated_chunk_count_) {
        if (allocation_arena) {
            if (allocation_arena != primary_arena_ && allocation_arena != secondary_arena_) {
                if (secondary_arena_) {
                    throw std::invalid_argument{"Delta state cannot own more than cut and post arenas"};
                }
                secondary_arena_ = std::move(allocation_arena);
            }
        }
        allocation_arena_ = secondary_arena_ ? secondary_arena_ : primary_arena_;
        scratch_->reset();
    }

    struct PreparedSlot final {
        DeltaPage* page{};
        std::size_t slot{};
        std::uint64_t hash{};
        std::uint8_t control{};
        bool inserted{};
    };

    [[nodiscard]] auto prepare(const std::uint64_t hash, const std::string_view key) -> PreparedSlot {
        const auto wanted = fingerprint(hash);
        auto group_start = probe_start(hash);
        for (std::size_t probed = 0; probed < previous_->capacity_; probed += kSwissGroupSize) {
            const auto page_index = group_start / kDeltaPageSlots;
            const auto page_offset = group_start % kDeltaPageSlots;
            const auto* page = page_at(page_index);
            if (page == nullptr) {
                if (size_ == previous_->maximum_entries_) {
                    throw std::bad_alloc{};
                }
                return {.page = &mutable_page(page_index),
                        .slot = page_offset,
                        .hash = hash,
                        .control = wanted,
                        .inserted = true};
            }
            const auto* group = &page->control[page_offset];
            const auto control_word = detail::load_control_group64(group);
            const auto matches = detail::equal_byte_mask(group, wanted);
            for (std::size_t offset = 0; offset < kSwissGroupSize; ++offset) {
                const auto control = detail::control_byte_at(control_word, offset);
                const auto slot = page_offset + offset;
                if (control == kSwissEmpty) {
                    if (size_ == previous_->maximum_entries_) {
                        throw std::bad_alloc{};
                    }
                    return {.page = &mutable_page(page_index),
                            .slot = slot,
                            .hash = hash,
                            .control = wanted,
                            .inserted = true};
                }
                if ((matches & (1ULL << offset)) != 0 && page->hashes[slot] == hash &&
                    delta_record_key(*page->records[slot]) == key) {
                    return {.page = &mutable_page(page_index), .slot = slot, .hash = hash, .control = wanted};
                }
            }
            group_start = next_group(group_start);
        }
        throw std::bad_alloc{};
    }

    [[nodiscard]] auto store(const ReadRecordView& record) -> const DeltaRecord* {
        GS_PHASE_PUT(delta_record_store);
        return allocation_arena_->store(record);
    }

    void commit(const PreparedSlot prepared, const DeltaRecord* record) noexcept {
        prepared.page->control[prepared.slot] = prepared.control;
        prepared.page->hashes[prepared.slot] = prepared.hash;
        prepared.page->records[prepared.slot] = record;
        if (prepared.inserted) {
            ++size_;
        }
    }

    [[nodiscard]] auto freeze() && -> DeltaState {
        return DeltaState{previous_->capacity_,       previous_->maximum_entries_,  size_,
                          allocated_page_count_,      allocated_block_count_,       allocated_chunk_count_,
                          std::move(flat_pages_),     std::move(directory_chunks_), std::move(primary_arena_),
                          std::move(secondary_arena_)};
    }

    [[nodiscard]] auto allocation_arena() const noexcept -> const std::shared_ptr<DeltaArena>& {
        return allocation_arena_;
    }

  private:
    [[nodiscard]] auto page_at(const std::size_t page_index) const noexcept -> const DeltaPage* {
        if (directory_chunks_.empty()) {
            return flat_pages_[page_index].get();
        }
        const auto block_index = page_index / kDeltaDirectoryBlockPages;
        const auto& chunk = directory_chunks_[block_index / kDeltaDirectoryChunkBlocks];
        if (!chunk) {
            return nullptr;
        }
        const auto& block = chunk->blocks[block_index % kDeltaDirectoryChunkBlocks];
        return block ? block->pages[page_index % kDeltaDirectoryBlockPages].get() : nullptr;
    }

    [[nodiscard]] auto probe_start(const std::uint64_t hash) const noexcept -> std::size_t {
        return ((hash >> 7U) & ((previous_->capacity_ / kSwissGroupSize) - 1U)) * kSwissGroupSize;
    }

    [[nodiscard]] auto next_group(const std::size_t group_start) const noexcept -> std::size_t {
        const auto next = group_start + kSwissGroupSize;
        return next == previous_->capacity_ ? 0 : next;
    }

    [[nodiscard]] auto mutable_page(const std::size_t page_index) -> DeltaPage& {
        if (last_mutable_page_index_ == page_index && last_mutable_page_ != nullptr) {
            return *last_mutable_page_;
        }
        auto& mutable_pages = scratch_->mutable_pages;
        const auto found_page =
            std::ranges::find_if(mutable_pages, [&](const auto& entry) { return entry.first == page_index; });
        if (found_page != mutable_pages.end()) {
            last_mutable_page_index_ = page_index;
            last_mutable_page_ = found_page->second.get();
            return *found_page->second;
        }
        const auto* existing = page_at(page_index);
        std::shared_ptr<DeltaPage> page;
        {
            GS_PHASE_PUT(delta_page_clone);
            page = existing ? std::make_shared<DeltaPage>(*existing) : std::make_shared<DeltaPage>();
        }
        if (existing == nullptr) {
            ++allocated_page_count_;
        }
        mutable_pages.emplace_back(page_index, page);
        if (directory_chunks_.empty()) {
            flat_pages_[page_index] = page;
        } else {
            const auto block_index = page_index / kDeltaDirectoryBlockPages;
            const auto chunk_index = block_index / kDeltaDirectoryChunkBlocks;
            const auto block_offset = block_index % kDeltaDirectoryChunkBlocks;
            auto& mutable_chunks = scratch_->mutable_chunks;
            auto found_chunk = std::ranges::find_if(
                mutable_chunks, [&](const auto& entry) { return entry.first == chunk_index; });
            if (found_chunk == mutable_chunks.end()) {
                std::shared_ptr<DeltaDirectoryChunk> chunk;
                const bool chunk_exists = directory_chunks_[chunk_index] != nullptr;
                {
                    GS_PHASE_PUT(delta_chunk_clone);
                    chunk = chunk_exists
                                ? std::make_shared<DeltaDirectoryChunk>(*directory_chunks_[chunk_index])
                                : std::make_shared<DeltaDirectoryChunk>();
                }
                if (!chunk_exists) {
                    ++allocated_chunk_count_;
                }
                mutable_chunks.emplace_back(chunk_index, chunk);
                directory_chunks_[chunk_index] = chunk;
                found_chunk = std::prev(mutable_chunks.end());
            }
            auto& mutable_blocks = scratch_->mutable_blocks;
            auto found_block = std::ranges::find_if(
                mutable_blocks, [&](const auto& entry) { return entry.first == block_index; });
            if (found_block == mutable_blocks.end()) {
                std::shared_ptr<DeltaDirectoryBlock> block;
                const bool block_exists = found_chunk->second->blocks[block_offset] != nullptr;
                {
                    GS_PHASE_PUT(delta_block_clone);
                    block = block_exists ? std::make_shared<DeltaDirectoryBlock>(
                                               *found_chunk->second->blocks[block_offset])
                                         : std::make_shared<DeltaDirectoryBlock>();
                }
                if (!block_exists) {
                    ++allocated_block_count_;
                }
                mutable_blocks.emplace_back(block_index, block);
                found_chunk->second->blocks[block_offset] = block;
                found_block = std::prev(mutable_blocks.end());
            }
            found_block->second->pages[page_index % kDeltaDirectoryBlockPages] = page;
        }
        last_mutable_page_index_ = page_index;
        last_mutable_page_ = page.get();
        return *page;
    }

    const DeltaState* previous_{};
    std::vector<std::shared_ptr<const DeltaPage>> flat_pages_;
    std::vector<std::shared_ptr<const DeltaDirectoryChunk>> directory_chunks_;
    std::shared_ptr<DeltaArena> primary_arena_;
    std::shared_ptr<DeltaArena> secondary_arena_;
    std::shared_ptr<DeltaArena> allocation_arena_;
    // Writer-thread-local reusable capacity; never shared with Reader and
    // cleared before each publication. The separate current/post scratch
    // instances cover the only two simultaneously live builders.
    DeltaBuilderScratch* scratch_{};
    DeltaPage* last_mutable_page_{};
    std::size_t last_mutable_page_index_{std::numeric_limits<std::size_t>::max()};
    std::size_t size_{};
    std::size_t allocated_page_count_{};
    std::size_t allocated_block_count_{};
    std::size_t allocated_chunk_count_{};
};

struct PairReadMerge::State final {
    enum class Phase : std::uint8_t { initialize, base, delta, ready };

    State(std::shared_ptr<const PairReadGeneration> merge_cut,
          std::unique_ptr<IncrementalBaseBuilder> next_base, DeltaState post, const std::size_t maximum_post)
        : cut(std::move(merge_cut)), current(cut), builder(std::move(next_base)), post_delta(std::move(post)),
          maximum_post_entries(maximum_post) {}

    std::shared_ptr<const PairReadGeneration> cut;
    std::shared_ptr<const PairReadGeneration> current;
    std::unique_ptr<IncrementalBaseBuilder> builder;
    DeltaState post_delta;
    std::size_t maximum_post_entries{};
    std::size_t base_cursor{};
    std::size_t delta_cursor{};
    Phase phase{Phase::initialize};
};

struct PairReadGenerationEnableShared final : PairReadGeneration {
    PairReadGenerationEnableShared(WorkerRoutingState routing, std::shared_ptr<const ImmutableReadIndex> base,
                                   DeltaState delta, const std::uint64_t epoch,
                                   const std::uint64_t visible_through) noexcept
        : PairReadGeneration(routing, std::move(base), nullptr, epoch, visible_through),
          delta_storage_(std::move(delta)) {
        bind_delta(&delta_storage_);
    }

    DeltaState delta_storage_;
};

[[nodiscard]] inline auto
make_shared_generation(WorkerRoutingState routing, std::shared_ptr<const ImmutableReadIndex> base,
                       DeltaState delta, const std::uint64_t epoch, const std::uint64_t visible_through)
    -> std::shared_ptr<const PairReadGeneration> {
    // Co-allocate generation shell + embedded DeltaState in one control block.
    GS_PHASE_PUT(generation_shell_allocate);
    return std::make_shared<PairReadGenerationEnableShared>(routing, std::move(base), std::move(delta), epoch,
                                                            visible_through);
}

// Writer-side incremental publication prep shared by Alt-A shared_ptr, direct
// slot-pool, and experimental fixed-shell allocators (ADR 0036 lab only).
struct IncrementalBuildResult final {
    std::optional<DeltaState> next_delta{};
    std::optional<DeltaState> next_post{};
    std::uint64_t visible_through{};
    std::shared_ptr<const PairReadGeneration> empty_reuse{};
};

struct IncrementalPublicationAccess final {
    [[nodiscard]] static auto prepare(const PairReadGeneration& previous,
                                      const std::shared_ptr<const PairReadGeneration>* previous_owner,
                                      const std::span<const ReadMutation> mutations, PairReadMerge* merge)
        -> Result<IncrementalBuildResult>;

    static void commit_merge(PairReadMerge* merge, IncrementalBuildResult& prepared,
                             std::shared_ptr<const PairReadGeneration> next) noexcept;
};

} // namespace glifistore::store::paired
