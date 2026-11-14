#include "experimental/paired_shard.hpp"

#include "experimental/spsc_ring.hpp"
#include "glifistore/core/integer_math.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/index/swiss_control_group.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace glifistore::experimental {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] constexpr auto storage_payload_bytes(const std::size_t count,
                                                   const std::size_t item_bytes) noexcept -> std::uint64_t {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (count > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()) ||
            item_bytes > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
            return std::numeric_limits<std::uint64_t>::max();
        }
    }
    const auto count_u64 = static_cast<std::uint64_t>(count);
    const auto item_u64 = static_cast<std::uint64_t>(item_bytes);
    return count_u64 != 0U && item_u64 > std::numeric_limits<std::uint64_t>::max() / count_u64
               ? std::numeric_limits<std::uint64_t>::max()
               : count_u64 * item_u64;
}

void update_high_watermark(std::atomic_size_t& maximum, const std::size_t candidate) noexcept {
    auto current = maximum.load(std::memory_order_relaxed);
    while (current < candidate &&
           !maximum.compare_exchange_weak(current, candidate, std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
    }
}

struct StableRecord final {
    std::uint64_t hash{};
    std::string key;
    std::vector<std::byte> value;
    std::uint64_t sequence{};
    std::uint64_t expire_at_ns{};
    bool tombstone{};
};

using RecordHandle = std::shared_ptr<const StableRecord>;
using MutableReadIndex = std::vector<RecordHandle>;

[[nodiscard]] auto record_less(const RecordHandle& left, const RecordHandle& right) noexcept -> bool {
    return left->hash < right->hash || (left->hash == right->hash && left->key < right->key);
}

void apply_record(MutableReadIndex& index, RecordHandle record) {
    const auto position = std::ranges::lower_bound(index, record, record_less);
    if (position != index.end() && (*position)->hash == record->hash && (*position)->key == record->key) {
        *position = std::move(record);
        return;
    }
    index.insert(position, std::move(record));
}

// Read-only flat index. Construction and all allocation happen on the Writer;
// lookup is allocation-free and probes fixed eight-byte control groups.
class ImmutableReadIndex final {
  public:
    ImmutableReadIndex()
        : control_(kSwissGroupSize, kSwissEmpty), hashes_(kSwissGroupSize), records_(kSwissGroupSize),
          capacity_(kSwissGroupSize) {}

    explicit ImmutableReadIndex(MutableReadIndex source) {
        capacity_ = kSwissGroupSize;
        while (maximum_occupancy(capacity_) < source.size()) {
            if (capacity_ > std::numeric_limits<std::size_t>::max() / 2U) {
                throw std::bad_alloc{};
            }
            capacity_ *= 2U;
        }
        control_.assign(capacity_, kSwissEmpty);
        hashes_.assign(capacity_, 0);
        records_.resize(capacity_);
        size_ = source.size();
        for (auto& record : source) {
            place(std::move(record));
        }
    }

    [[nodiscard]] auto find(const std::string_view key, const std::uint64_t hash) const noexcept
        -> const StableRecord* {
        const auto fingerprint = h2(hash);
        auto group_start = probe_start(hash);
        for (std::size_t probed = 0; probed < capacity_; probed += kSwissGroupSize) {
            const auto* const group = &control_[group_start];
            const auto control_word = detail::load_control_group64(group);
            const auto matches = detail::equal_byte_mask(group, fingerprint);
            for (std::size_t offset = 0; offset < kSwissGroupSize; ++offset) {
                const auto control = detail::control_byte_at(control_word, offset);
                if (control == kSwissEmpty) {
                    return nullptr;
                }
                if ((matches & (1ULL << offset)) == 0) {
                    continue;
                }
                const auto slot = group_start + offset;
                if (hashes_[slot] == hash && records_[slot]->key == key) {
                    return records_[slot].get();
                }
            }
            group_start = next_group(group_start);
        }
        return nullptr;
    }

    [[nodiscard]] auto records() const -> MutableReadIndex {
        MutableReadIndex result;
        result.reserve(size_);
        for (std::size_t slot = 0; slot < capacity_; ++slot) {
            if (control_[slot] != kSwissEmpty) {
                result.push_back(records_[slot]);
            }
        }
        std::ranges::sort(result, record_less);
        return result;
    }

    [[nodiscard]] auto storage_bytes() const noexcept -> std::uint64_t {
        auto result = storage_payload_bytes(control_.capacity(), sizeof(std::uint8_t));
        result = glifistore::saturating_add(result,
                                            storage_payload_bytes(hashes_.capacity(), sizeof(std::uint64_t)));
        return glifistore::saturating_add(result,
                                          storage_payload_bytes(records_.capacity(), sizeof(RecordHandle)));
    }

  private:
    [[nodiscard]] static constexpr auto maximum_occupancy(const std::size_t capacity) noexcept
        -> std::size_t {
        return capacity - capacity / 4U;
    }

    [[nodiscard]] static auto h2(const std::uint64_t hash) noexcept -> std::uint8_t {
        const auto fingerprint = static_cast<std::uint8_t>(hash & 0x7FU);
        return fingerprint == 0 ? static_cast<std::uint8_t>(1) : fingerprint;
    }

    [[nodiscard]] auto probe_start(const std::uint64_t hash) const noexcept -> std::size_t {
        return ((hash >> 7U) & ((capacity_ / kSwissGroupSize) - 1U)) * kSwissGroupSize;
    }

    [[nodiscard]] auto next_group(const std::size_t group_start) const noexcept -> std::size_t {
        const auto next = group_start + kSwissGroupSize;
        return next == capacity_ ? 0 : next;
    }

    void place(RecordHandle record) noexcept {
        auto group_start = probe_start(record->hash);
        for (;;) {
            for (std::size_t offset = 0; offset < kSwissGroupSize; ++offset) {
                const auto slot = group_start + offset;
                if (control_[slot] == kSwissEmpty) {
                    control_[slot] = h2(record->hash);
                    hashes_[slot] = record->hash;
                    records_[slot] = std::move(record);
                    return;
                }
            }
            group_start = next_group(group_start);
        }
    }

    std::vector<std::uint8_t> control_;
    std::vector<std::uint64_t> hashes_;
    std::vector<RecordHandle> records_;
    std::size_t capacity_{};
    std::size_t size_{};
};

inline constexpr std::size_t kDeltaPageSlots = 16;
inline constexpr std::size_t kDeltaDirectoryBlockPages = 16;
inline constexpr std::size_t kFlatDeltaMaximumPages = 32;
inline constexpr std::size_t kWriterMaximumBatchRecords = 32;
static_assert(kDeltaPageSlots % kSwissGroupSize == 0);

[[nodiscard]] auto delta_fingerprint(const std::uint64_t hash) noexcept -> std::uint8_t {
    const auto fingerprint = static_cast<std::uint8_t>(hash & 0x7FU);
    return fingerprint == 0 ? static_cast<std::uint8_t>(1) : fingerprint;
}

struct DeltaPage final {
    DeltaPage() {
        control.fill(kSwissEmpty);
    }

    std::array<std::uint8_t, kDeltaPageSlots> control{};
    std::array<std::uint64_t, kDeltaPageSlots> hashes{};
    std::array<RecordHandle, kDeltaPageSlots> records{};
};

struct DeltaDirectoryBlock final {
    std::array<std::shared_ptr<const DeltaPage>, kDeltaDirectoryBlockPages> pages{};
};

struct DeltaState final {
    std::size_t capacity{};
    std::size_t size{};
    // Small deltas stay flat to preserve the shortest GET path. Larger deltas
    // use a persistent two-level ownership directory plus a flat non-owning
    // page view. ReadGeneration pins the directory, so GET keeps the direct
    // page lookup without copying hundreds of shared_ptr refcounts.
    std::vector<std::shared_ptr<const DeltaPage>> flat_pages{};
    std::vector<const DeltaPage*> page_views{};
    std::vector<std::shared_ptr<const DeltaDirectoryBlock>> directory{};

    [[nodiscard]] auto hierarchical() const noexcept -> bool {
        return !directory.empty();
    }

    [[nodiscard]] auto find(const std::string_view key, const std::uint64_t hash) const noexcept
        -> const StableRecord* {
        const auto fingerprint = delta_fingerprint(hash);
        auto group_start = probe_start(hash);
        for (std::size_t probed = 0; probed < capacity; probed += kSwissGroupSize) {
            const auto page_index = group_start / kDeltaPageSlots;
            const auto page_offset = group_start % kDeltaPageSlots;
            const auto* const page = page_at(page_index);
            if (page == nullptr) {
                return nullptr;
            }
            const auto* const group = &page->control[page_offset];
            const auto control_word = detail::load_control_group64(group);
            const auto matches = detail::equal_byte_mask(group, fingerprint);
            for (std::size_t offset = 0; offset < kSwissGroupSize; ++offset) {
                const auto control = detail::control_byte_at(control_word, offset);
                if (control == kSwissEmpty) {
                    return nullptr;
                }
                if ((matches & (1ULL << offset)) == 0) {
                    continue;
                }
                const auto page_slot = page_offset + offset;
                if (page->hashes[page_slot] == hash && page->records[page_slot]->key == key) {
                    return page->records[page_slot].get();
                }
            }
            group_start = next_group(group_start);
        }
        return nullptr;
    }

    template <typename Callback> void for_each(Callback&& callback) const {
        if (!hierarchical()) {
            for (const auto& page : flat_pages) {
                visit_page(page.get(), callback);
            }
            return;
        }
        for (const auto* page : page_views) {
            visit_page(page, callback);
        }
    }

    [[nodiscard]] auto storage_bytes() const noexcept -> std::uint64_t {
        auto result = storage_payload_bytes(flat_pages.capacity(), sizeof(std::shared_ptr<const DeltaPage>));
        result = glifistore::saturating_add(
            result, storage_payload_bytes(page_views.capacity(), sizeof(const DeltaPage*)));
        result = glifistore::saturating_add(
            result,
            storage_payload_bytes(directory.capacity(), sizeof(std::shared_ptr<const DeltaDirectoryBlock>)));
        if (!hierarchical()) {
            for (const auto& page : flat_pages) {
                if (page) {
                    result = glifistore::saturating_add(result, sizeof(DeltaPage));
                }
            }
            return result;
        }
        for (const auto& block : directory) {
            if (!block) {
                continue;
            }
            result = glifistore::saturating_add(result, sizeof(DeltaDirectoryBlock));
            for (const auto& page : block->pages) {
                if (page) {
                    result = glifistore::saturating_add(result, sizeof(DeltaPage));
                }
            }
        }
        return result;
    }

  private:
    [[nodiscard]] auto page_at(const std::size_t page_index) const noexcept -> const DeltaPage* {
        if (!hierarchical()) {
            return flat_pages[page_index].get();
        }
        return page_views[page_index];
    }

    template <typename Callback> static void visit_page(const DeltaPage* page, Callback& callback) {
        if (page == nullptr) {
            return;
        }
        for (std::size_t slot = 0; slot < kDeltaPageSlots; ++slot) {
            if (page->control[slot] != kSwissEmpty) {
                callback(page->records[slot]);
            }
        }
    }

    [[nodiscard]] auto probe_start(const std::uint64_t hash) const noexcept -> std::size_t {
        return ((hash >> 7U) & ((capacity / kSwissGroupSize) - 1U)) * kSwissGroupSize;
    }

    [[nodiscard]] auto next_group(const std::size_t group_start) const noexcept -> std::size_t {
        const auto next = group_start + kSwissGroupSize;
        return next == capacity ? 0 : next;
    }
};

[[nodiscard]] auto make_empty_delta(const std::size_t maximum_entries) -> std::shared_ptr<const DeltaState> {
    if (maximum_entries > std::numeric_limits<std::size_t>::max() - (kWriterMaximumBatchRecords - 1U)) {
        throw std::bad_alloc{};
    }
    const auto required_entries = maximum_entries + (kWriterMaximumBatchRecords - 1U);
    auto capacity = kDeltaPageSlots;
    while (capacity - capacity / 4U < required_entries) {
        if (capacity > std::numeric_limits<std::size_t>::max() / 2U) {
            throw std::bad_alloc{};
        }
        capacity *= 2U;
    }
    const auto page_count = capacity / kDeltaPageSlots;
    if (page_count <= kFlatDeltaMaximumPages) {
        return std::make_shared<const DeltaState>(DeltaState{
            .capacity = capacity, .flat_pages = std::vector<std::shared_ptr<const DeltaPage>>(page_count)});
    }
    const auto directory_count = (page_count + kDeltaDirectoryBlockPages - 1U) / kDeltaDirectoryBlockPages;
    return std::make_shared<const DeltaState>(
        DeltaState{.capacity = capacity,
                   .page_views = std::vector<const DeltaPage*>(page_count),
                   .directory = std::vector<std::shared_ptr<const DeltaDirectoryBlock>>(directory_count)});
}

struct DeltaBuildTelemetry final {
    std::uint64_t directory_entries_copied{};
    std::uint64_t page_view_entries_copied{};
    std::uint64_t pages_copied{};
    std::uint64_t pages_allocated{};
};

class DeltaBuilder final {
  public:
    explicit DeltaBuilder(std::shared_ptr<const DeltaState> previous)
        : previous_(std::move(previous)), flat_pages_(previous_->flat_pages),
          page_views_(previous_->page_views), directory_(previous_->directory),
          mutable_blocks_(directory_.size()), mutable_pages_(previous_->capacity / kDeltaPageSlots),
          size_(previous_->size) {
        telemetry_.directory_entries_copied = flat_pages_.size() + directory_.size();
        telemetry_.page_view_entries_copied = page_views_.size();
    }

    void insert_or_assign(RecordHandle record) {
        const auto fingerprint = delta_fingerprint(record->hash);
        auto group_start = probe_start(record->hash);
        for (std::size_t probed = 0; probed < previous_->capacity; probed += kSwissGroupSize) {
            const auto page_index = group_start / kDeltaPageSlots;
            const auto page_offset = group_start % kDeltaPageSlots;
            const auto* const page = page_at(page_index);
            if (page == nullptr) {
                place(page_index, page_offset, fingerprint, std::move(record), true);
                return;
            }
            const auto* const group = &page->control[page_offset];
            const auto control_word = detail::load_control_group64(group);
            const auto matches = detail::equal_byte_mask(group, fingerprint);
            for (std::size_t offset = 0; offset < kSwissGroupSize; ++offset) {
                const auto control = detail::control_byte_at(control_word, offset);
                const auto page_slot = page_offset + offset;
                if (control == kSwissEmpty) {
                    place(page_index, page_slot, fingerprint, std::move(record), true);
                    return;
                }
                if ((matches & (1ULL << offset)) != 0 && page->hashes[page_slot] == record->hash &&
                    page->records[page_slot]->key == record->key) {
                    place(page_index, page_slot, fingerprint, std::move(record), false);
                    return;
                }
            }
            group_start = next_group(group_start);
        }
        throw std::bad_alloc{};
    }

    [[nodiscard]] auto freeze() && -> std::shared_ptr<const DeltaState> {
        return std::make_shared<const DeltaState>(DeltaState{.capacity = previous_->capacity,
                                                             .size = size_,
                                                             .flat_pages = std::move(flat_pages_),
                                                             .page_views = std::move(page_views_),
                                                             .directory = std::move(directory_)});
    }

    [[nodiscard]] auto telemetry() const noexcept -> DeltaBuildTelemetry {
        return telemetry_;
    }

  private:
    [[nodiscard]] auto page_at(const std::size_t page_index) const noexcept -> const DeltaPage* {
        if (!previous_->hierarchical()) {
            return flat_pages_[page_index].get();
        }
        return page_views_[page_index];
    }

    [[nodiscard]] auto probe_start(const std::uint64_t hash) const noexcept -> std::size_t {
        return ((hash >> 7U) & ((previous_->capacity / kSwissGroupSize) - 1U)) * kSwissGroupSize;
    }

    [[nodiscard]] auto next_group(const std::size_t group_start) const noexcept -> std::size_t {
        const auto next = group_start + kSwissGroupSize;
        return next == previous_->capacity ? 0 : next;
    }

    [[nodiscard]] auto mutable_page(const std::size_t page_index) -> DeltaPage& {
        if (!mutable_pages_[page_index]) {
            const auto* const existing = page_at(page_index);
            if (existing != nullptr) {
                mutable_pages_[page_index] = std::make_shared<DeltaPage>(*existing);
                ++telemetry_.pages_copied;
            } else {
                mutable_pages_[page_index] = std::make_shared<DeltaPage>();
            }
            ++telemetry_.pages_allocated;
            if (!previous_->hierarchical()) {
                flat_pages_[page_index] = mutable_pages_[page_index];
            } else {
                const auto block_index = page_index / kDeltaDirectoryBlockPages;
                if (!mutable_blocks_[block_index]) {
                    if (directory_[block_index]) {
                        mutable_blocks_[block_index] =
                            std::make_shared<DeltaDirectoryBlock>(*directory_[block_index]);
                        telemetry_.directory_entries_copied += kDeltaDirectoryBlockPages;
                    } else {
                        mutable_blocks_[block_index] = std::make_shared<DeltaDirectoryBlock>();
                    }
                    directory_[block_index] = mutable_blocks_[block_index];
                }
                mutable_blocks_[block_index]->pages[page_index % kDeltaDirectoryBlockPages] =
                    mutable_pages_[page_index];
                page_views_[page_index] = mutable_pages_[page_index].get();
            }
        }
        return *mutable_pages_[page_index];
    }

    void place(const std::size_t page_index, const std::size_t page_slot, const std::uint8_t fingerprint,
               RecordHandle record, const bool inserted) {
        auto& page = mutable_page(page_index);
        page.control[page_slot] = fingerprint;
        page.hashes[page_slot] = record->hash;
        page.records[page_slot] = std::move(record);
        if (inserted) {
            ++size_;
        }
    }

    std::shared_ptr<const DeltaState> previous_;
    std::vector<std::shared_ptr<const DeltaPage>> flat_pages_;
    std::vector<const DeltaPage*> page_views_;
    std::vector<std::shared_ptr<const DeltaDirectoryBlock>> directory_;
    std::vector<std::shared_ptr<DeltaDirectoryBlock>> mutable_blocks_;
    std::vector<std::shared_ptr<DeltaPage>> mutable_pages_;
    std::size_t size_{};
    DeltaBuildTelemetry telemetry_{};
};

struct ReadGeneration final {
    std::shared_ptr<const ImmutableReadIndex> base;
    std::shared_ptr<const DeltaState> delta;
    std::uint64_t visible_through{};
    std::uint64_t epoch{};
};

struct PublicationDescriptor final {
    const ReadGeneration* generation{};
    std::uint64_t epoch{};
    std::uint64_t visible_through{};
    std::uint32_t slot{};
};

// ADR 0036 prototype: publication is one release/acquire word. The low 16 bits
// encode slot + 1 (zero is reserved); the upper 48 bits identify the slot
// incarnation. Unlike an atomic descriptor pointer, the token cannot exhibit
// pointer ABA when a fixed slot is eventually reused.
constexpr std::uint64_t kPublicationSlotBits = 16U;
constexpr std::uint64_t kPublicationSlotMask = (1ULL << kPublicationSlotBits) - 1U;
constexpr std::uint64_t kMaximumPublicationEpoch =
    std::numeric_limits<std::uint64_t>::max() >> kPublicationSlotBits;

[[nodiscard]] constexpr auto encode_publication_token(const std::uint64_t epoch,
                                                      const std::size_t slot) noexcept -> std::uint64_t {
    return (epoch << kPublicationSlotBits) | (static_cast<std::uint64_t>(slot) + 1U);
}

[[nodiscard]] constexpr auto publication_token_epoch(const std::uint64_t token) noexcept -> std::uint64_t {
    return token >> kPublicationSlotBits;
}

[[nodiscard]] constexpr auto publication_token_slot(const std::uint64_t token) noexcept -> std::size_t {
    return static_cast<std::size_t>((token & kPublicationSlotMask) - 1U);
}

enum class GenerationSlotState : std::uint8_t { free, published, retired };

struct GenerationSlot final {
    std::optional<ReadGeneration> generation;
    PublicationDescriptor descriptor;
    Clock::time_point retired_at{};
    std::uint64_t retire_after_turn{};
    std::atomic_size_t output_pins{};
    std::uint64_t publication_count{}; // Writer-only; survives free/reuse.
    GenerationSlotState state{GenerationSlotState::free};
};

struct MutationSlot final {
    std::array<char, VolatileShardPairPrototype::kMaximumKeyBytes> key{};
    std::uint64_t request_id{};
    std::uint64_t expire_at_ns{};
    std::size_t key_size{};
    std::size_t value_size{};
    PrototypeMutationKind kind{};
};

struct InternalCompletion final {
    PrototypeCompletion completion;
    std::uint32_t slot{};
};

static_assert(std::is_nothrow_move_assignable_v<InternalCompletion>);

} // namespace

PrototypeReadPin::~PrototypeReadPin() {
    reset();
}

PrototypeReadPin::PrototypeReadPin(PrototypeReadPin&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), slot_(other.slot_),
      release_(std::exchange(other.release_, nullptr)) {}

auto PrototypeReadPin::operator=(PrototypeReadPin&& other) noexcept -> PrototypeReadPin& {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        slot_ = other.slot_;
        release_ = std::exchange(other.release_, nullptr);
    }
    return *this;
}

void PrototypeReadPin::reset() noexcept {
    if (owner_ != nullptr) {
        auto* owner = std::exchange(owner_, nullptr);
        const auto release = std::exchange(release_, nullptr);
        release(owner, slot_);
    }
}

struct VolatileShardPairPrototype::Impl final {
    static constexpr std::size_t kGenerationPoolCapacity = kQueueCapacity + 2U;
    static_assert(kGenerationPoolCapacity < kPublicationSlotMask);
    static_assert(std::atomic_uint64_t::is_always_lock_free);

    explicit Impl(const std::size_t maximum_value, const std::size_t merge_entries,
                  const PrototypeWriterBatchConfig writer_batch_config,
                  const PrototypeCompletionNotifier notifier)
        : maximum_value_bytes(maximum_value), merge_delta_entries(merge_entries),
          batch_config(writer_batch_config), completion_notifier(notifier),
          value_arena(kQueueCapacity * maximum_value), base(std::make_shared<const ImmutableReadIndex>()),
          mutable_delta(make_empty_delta(merge_entries)) {
        for (std::size_t index = 0; index < kQueueCapacity; ++index) {
            free_slots[index] = static_cast<std::uint32_t>(kQueueCapacity - 1U - index);
        }
        free_count = kQueueCapacity;

        auto& initial = generations[0];
        initial.generation.emplace(
            ReadGeneration{.base = base, .delta = mutable_delta, .visible_through = 0, .epoch = 0});
        initial.descriptor = {
            .generation = &*initial.generation, .epoch = 0, .visible_through = 0, .slot = 0};
        initial.state = GenerationSlotState::published;
        initial.publication_count = 1;
        current_generation_slot = 0;
        publication.store(encode_publication_token(0, 0), std::memory_order_release);
        local_publication = &initial.descriptor;
        generation_live.store(1, std::memory_order_relaxed);
    }

    void start() {
        writer = std::thread([this] { writer_loop(); });
    }

    [[nodiscard]] auto value_span(const std::uint32_t slot) noexcept -> std::span<std::byte> {
        return {value_arena.data() + static_cast<std::size_t>(slot) * maximum_value_bytes,
                maximum_value_bytes};
    }

    [[nodiscard]] auto submit(const PrototypeMutationKind kind, const std::uint64_t request_id,
                              const std::string_view key, const std::span<const std::byte> value,
                              const std::uint64_t expire_at_ns) noexcept -> PrototypeSubmitStatus {
        if (!accepting.load(std::memory_order_acquire)) {
            return PrototypeSubmitStatus::stopped;
        }
        if (key.size() > kMaximumKeyBytes) {
            return PrototypeSubmitStatus::key_too_large;
        }
        if (value.size() > maximum_value_bytes) {
            return PrototypeSubmitStatus::value_too_large;
        }
        if (free_count == 0) {
            queue_full.fetch_add(1U, std::memory_order_relaxed);
            return PrototypeSubmitStatus::queue_full;
        }
        const auto slot = free_slots[--free_count];
        auto& mutation = slots[slot];
        mutation = {.request_id = request_id,
                    .expire_at_ns = expire_at_ns,
                    .key_size = key.size(),
                    .value_size = value.size(),
                    .kind = kind};
        if (!key.empty()) {
            std::memcpy(mutation.key.data(), key.data(), key.size());
        }
        if (!value.empty()) {
            std::ranges::copy(value, value_span(slot).begin());
        }
        if (!mutations.try_push(slot)) {
            free_slots[free_count++] = slot;
            queue_full.fetch_add(1U, std::memory_order_relaxed);
            return PrototypeSubmitStatus::queue_full;
        }
        mutation_pushes.fetch_add(1U, std::memory_order_relaxed);
        update_high_watermark(mutation_high_watermark, mutations.size());
        return PrototypeSubmitStatus::submitted;
    }

    void push_completion(const std::uint32_t slot, const PrototypeCompletion& completion) noexcept {
        // Admission reserves one completion credit with every slot. Failure is
        // therefore an invariant violation, not ordinary backpressure.
        while (!completions.try_push(InternalCompletion{.completion = completion, .slot = slot})) {
            accepting.store(false, std::memory_order_release);
            invariant_failed.store(true, std::memory_order_release);
            std::this_thread::yield();
        }
        completion_pushes.fetch_add(1U, std::memory_order_relaxed);
        update_high_watermark(completion_high_watermark, completions.size());
    }

    void notify_reader() const noexcept {
        if (completion_notifier.notify != nullptr) {
            completion_notifier.notify(completion_notifier.context);
        }
    }

    static void release_output_pin(void* owner, const std::uint32_t slot) noexcept {
        auto& self = *static_cast<Impl*>(owner);
        self.generations[slot].output_pins.fetch_sub(1U, std::memory_order_release);
        self.generation_output_pins.fetch_sub(1U, std::memory_order_relaxed);
    }

    [[nodiscard]] auto make_record(const std::uint32_t slot, const std::uint64_t sequence) -> RecordHandle {
        const auto& mutation = slots[slot];
        auto record = std::make_shared<StableRecord>();
        record->hash = hash_key(std::string_view{mutation.key.data(), mutation.key_size});
        record->key.assign(mutation.key.data(), mutation.key_size);
        record->sequence = sequence;
        record->expire_at_ns = mutation.expire_at_ns;
        record->tombstone = mutation.kind == PrototypeMutationKind::erase;
        if (mutation.kind != PrototypeMutationKind::erase) {
            const auto source = value_span(slot).first(mutation.value_size);
            record->value.assign(source.begin(), source.end());
            ingress_value_bytes_copied.fetch_add(source.size(), std::memory_order_relaxed);
        }
        payload_allocations.fetch_add(1U, std::memory_order_relaxed);
        payload_bytes_allocated.fetch_add(record->key.size() + record->value.size(),
                                          std::memory_order_relaxed);
        return record;
    }

    void reclaim_generations() noexcept {
        const auto turn = reader_turns.load(std::memory_order_seq_cst);
        const auto now = Clock::now();
        for (auto& slot : generations) {
            if (slot.state != GenerationSlotState::retired || turn < slot.retire_after_turn) {
                continue;
            }
            if (slot.output_pins.load(std::memory_order_acquire) != 0) {
                generation_retire_pin_blocks.fetch_add(1U, std::memory_order_relaxed);
                continue;
            }
            const auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(now - slot.retired_at);
            generation_retire_delay_ns.fetch_add(static_cast<std::uint64_t>(delay.count()),
                                                 std::memory_order_relaxed);
            slot.generation.reset();
            slot.descriptor = {};
            slot.state = GenerationSlotState::free;
            generation_retire_count.fetch_add(1U, std::memory_order_relaxed);
            generation_live.fetch_sub(1U, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] auto acquire_generation_slot() noexcept -> std::optional<std::size_t> {
        // Bound the wait: QSBR reclaim depends on Reader turns. Spinning forever
        // when the Reader is stuck would hang the Writer and hide pool exhaustion.
        // ADR 0036 V9: backpressure signal, then fail closed — never overwrite a live slot.
        constexpr std::size_t kMaximumAcquireSpins = 1'000'000U;
        for (std::size_t spin = 0; spin <= kMaximumAcquireSpins; ++spin) {
            reclaim_generations();
            for (std::size_t index = 0; index < generations.size(); ++index) {
                if (generations[index].state == GenerationSlotState::free) {
                    return index;
                }
            }
            if (spin == kMaximumAcquireSpins) {
                break;
            }
            publication_backpressure.fetch_add(1U, std::memory_order_relaxed);
            std::this_thread::yield();
        }
        generation_slot_exhaustions.fetch_add(1U, std::memory_order_relaxed);
        return std::nullopt;
    }

    void publish(const std::size_t next_slot, std::shared_ptr<const ImmutableReadIndex> next_base,
                 std::shared_ptr<const DeltaState> next_delta, const std::uint64_t next_visible,
                 const std::uint64_t next_epoch) {
        auto& next = generations[next_slot];
        next.generation.emplace(ReadGeneration{.base = std::move(next_base),
                                               .delta = std::move(next_delta),
                                               .visible_through = next_visible,
                                               .epoch = next_epoch});
        next.descriptor = {.generation = &*next.generation,
                           .epoch = next_epoch,
                           .visible_through = next_visible,
                           .slot = static_cast<std::uint32_t>(next_slot)};
        if (next.publication_count != 0) {
            generation_slot_reuses.fetch_add(1U, std::memory_order_relaxed);
        }
        ++next.publication_count;
        next.state = GenerationSlotState::published;
        generation_live.fetch_add(1U, std::memory_order_relaxed);
        update_high_watermark(generation_high_watermark, generation_live.load(std::memory_order_relaxed));

        const auto previous_slot = current_generation_slot;
        current_generation_slot = next_slot;
        publication.store(encode_publication_token(next_epoch, next_slot), std::memory_order_release);

        // Two quiescent boundaries cover a Reader concurrent with publication:
        // one to finish an old-generation turn, one to prove it advanced.
        auto& previous = generations[previous_slot];
        previous.retire_after_turn = reader_turns.load(std::memory_order_seq_cst) + 2U;
        previous.retired_at = Clock::now();
        previous.state = GenerationSlotState::retired;
    }

    void process_batch(const std::span<const std::uint32_t> batch) noexcept {
        const auto started = Clock::now();
        last_batch_size.store(batch.size(), std::memory_order_relaxed);
        update_high_watermark(maximum_batch_size, batch.size());
        try {
            DeltaBuilder delta_builder{mutable_delta};
            auto next_visible = visible_through;
            for (const auto slot : batch) {
                delta_builder.insert_or_assign(make_record(slot, ++next_visible));
            }
            const auto delta_telemetry = delta_builder.telemetry();
            auto next_delta = std::move(delta_builder).freeze();

            auto next_base = base;
            bool merged = false;
            if (next_delta->size >= merge_delta_entries) {
                auto merged_records = base->records();
                next_delta->for_each(
                    [&merged_records](const RecordHandle& record) { apply_record(merged_records, record); });
                std::erase_if(merged_records, [](const RecordHandle& record) { return record->tombstone; });
                next_base = std::make_shared<const ImmutableReadIndex>(std::move(merged_records));
                next_delta = make_empty_delta(merge_delta_entries);
                merged = true;
            }
            const auto current_epoch = writer_epoch.load(std::memory_order_relaxed);
            if (current_epoch >= kMaximumPublicationEpoch) {
                for (const auto slot : batch) {
                    push_completion(slot, PrototypeCompletion{.request_id = slots[slot].request_id,
                                                              .error = ErrorCode::arithmetic_overflow,
                                                              .visible_through = visible_through,
                                                              .epoch = current_epoch});
                }
                notify_reader();
                return;
            }
            const auto next_epoch = current_epoch + 1U;
            const auto generation_slot = acquire_generation_slot();
            if (!generation_slot.has_value()) {
                for (const auto slot : batch) {
                    push_completion(
                        slot, PrototypeCompletion{.request_id = slots[slot].request_id,
                                                  .error = ErrorCode::resource_exhausted,
                                                  .visible_through = visible_through,
                                                  .epoch = writer_epoch.load(std::memory_order_relaxed)});
                }
                notify_reader();
                return;
            }
            const auto publication_storage = next_base->storage_bytes() + next_delta->storage_bytes();
            publish(*generation_slot, next_base, next_delta, next_visible, next_epoch);

            base = std::move(next_base);
            mutable_delta = std::move(next_delta);
            visible_through = next_visible;
            writer_epoch.store(next_epoch, std::memory_order_relaxed);
            publications.fetch_add(1U, std::memory_order_relaxed);
            publication_records.fetch_add(batch.size(), std::memory_order_relaxed);
            publication_bytes.store(publication_storage, std::memory_order_relaxed);
            delta_directory_entries_copied.fetch_add(delta_telemetry.directory_entries_copied,
                                                     std::memory_order_relaxed);
            delta_page_view_entries_copied.fetch_add(delta_telemetry.page_view_entries_copied,
                                                     std::memory_order_relaxed);
            delta_pages_copied.fetch_add(delta_telemetry.pages_copied, std::memory_order_relaxed);
            delta_pages_allocated.fetch_add(delta_telemetry.pages_allocated, std::memory_order_relaxed);
            if (merged) {
                delta_merges.fetch_add(1U, std::memory_order_relaxed);
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
            publication_latency_ns.fetch_add(static_cast<std::uint64_t>(elapsed.count()),
                                             std::memory_order_relaxed);

            for (const auto slot : batch) {
                push_completion(slot, PrototypeCompletion{.request_id = slots[slot].request_id,
                                                          .visible_through = next_visible,
                                                          .epoch = next_epoch});
            }
        } catch (const std::bad_alloc&) {
            for (const auto slot : batch) {
                push_completion(slot,
                                PrototypeCompletion{.request_id = slots[slot].request_id,
                                                    .error = ErrorCode::resource_exhausted,
                                                    .visible_through = visible_through,
                                                    .epoch = writer_epoch.load(std::memory_order_relaxed)});
            }
        } catch (...) {
            accepting.store(false, std::memory_order_release);
            for (const auto slot : batch) {
                push_completion(slot,
                                PrototypeCompletion{.request_id = slots[slot].request_id,
                                                    .error = ErrorCode::internal_error,
                                                    .visible_through = visible_through,
                                                    .epoch = writer_epoch.load(std::memory_order_relaxed)});
            }
        }
        notify_reader();
    }

    void writer_loop() noexcept {
        std::array<std::uint32_t, kWriterMaximumBatchRecords> batch{};
        while (accepting.load(std::memory_order_acquire) || !mutations.empty()) {
            std::size_t count = 0;
            if (!mutations.try_pop(batch[count])) {
                reclaim_generations();
                std::this_thread::yield();
                continue;
            }
            ++count;
            mutation_pops.fetch_add(1U, std::memory_order_relaxed);

            const auto wait_started = Clock::now();
            const auto deadline = wait_started + batch_config.max_wait;
            bool deadline_closed = false;
            while (count < batch_config.max_records) {
                if (mutations.try_pop(batch[count])) {
                    ++count;
                    mutation_pops.fetch_add(1U, std::memory_order_relaxed);
                    continue;
                }
                if (batch_config.max_wait.count() == 0 ||
                    (!accepting.load(std::memory_order_relaxed) && mutations.empty()) ||
                    Clock::now() >= deadline) {
                    deadline_closed = batch_config.max_wait.count() != 0 && Clock::now() >= deadline;
                    break;
                }
                // Dedicated Writer: bounded busy wait avoids a scheduler round
                // trip while the paired Reader is producing the next mutation.
                std::atomic_signal_fence(std::memory_order_seq_cst);
            }
            const auto waited =
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - wait_started);
            writer_batch_wait_ns.fetch_add(static_cast<std::uint64_t>(waited.count()),
                                           std::memory_order_relaxed);
            if (deadline_closed) {
                writer_batch_deadline_closes.fetch_add(1U, std::memory_order_relaxed);
            }
            process_batch(std::span{batch}.first(count));
        }
        reclaim_generations();
        writer_done.store(true, std::memory_order_release);
    }

    const std::size_t maximum_value_bytes;
    const std::size_t merge_delta_entries;
    const PrototypeWriterBatchConfig batch_config;
    const PrototypeCompletionNotifier completion_notifier;
    std::array<MutationSlot, kQueueCapacity> slots{};
    std::vector<std::byte> value_arena;
    std::array<std::uint32_t, kQueueCapacity> free_slots{};
    std::size_t free_count{};
    SpscRing<std::uint32_t, kQueueCapacity> mutations;
    SpscRing<InternalCompletion, kQueueCapacity> completions;
    std::array<GenerationSlot, kGenerationPoolCapacity> generations{};
    std::atomic_uint64_t publication{};
    const PublicationDescriptor* local_publication{};
    std::size_t current_generation_slot{}; // Writer-only
    std::shared_ptr<const ImmutableReadIndex> base;
    std::shared_ptr<const DeltaState> mutable_delta;
    std::uint64_t visible_through{};
    std::thread writer;
    std::atomic_bool accepting{true};
    std::atomic_bool writer_done{};
    std::atomic_bool invariant_failed{};
    std::atomic_uint64_t reader_turns{};
    std::atomic_uint64_t mutation_pushes{};
    std::atomic_uint64_t mutation_pops{};
    std::atomic_uint64_t completion_pushes{};
    std::atomic_uint64_t completion_pops{};
    std::atomic_uint64_t queue_full{};
    std::atomic_size_t mutation_high_watermark{};
    std::atomic_size_t completion_high_watermark{};
    std::atomic_size_t last_batch_size{};
    std::atomic_size_t maximum_batch_size{};
    std::atomic_uint64_t writer_batch_wait_ns{};
    std::atomic_uint64_t writer_batch_deadline_closes{};
    std::uint64_t reader_gets{}; // Reader-owner thread only
    std::atomic_uint64_t publications{};
    std::atomic_uint64_t publication_records{};
    std::atomic_uint64_t publication_latency_ns{};
    std::atomic_uint64_t publication_bytes{};
    std::atomic_uint64_t ingress_value_bytes_copied{};
    std::atomic_uint64_t payload_allocations{};
    std::atomic_uint64_t payload_bytes_allocated{};
    std::atomic_uint64_t delta_directory_entries_copied{};
    std::atomic_uint64_t delta_page_view_entries_copied{};
    std::atomic_uint64_t delta_pages_copied{};
    std::atomic_uint64_t delta_pages_allocated{};
    std::atomic_uint64_t delta_merges{};
    std::atomic_uint64_t publication_backpressure{};
    std::atomic_uint64_t generation_slot_exhaustions{};
    std::atomic_uint64_t generation_slot_reuses{};
    std::atomic_uint64_t generation_retire_count{};
    std::atomic_uint64_t generation_retire_delay_ns{};
    std::atomic_size_t generation_output_pins{};
    std::atomic_size_t generation_output_pin_high_watermark{};
    std::atomic_uint64_t generation_retire_pin_blocks{};
    std::atomic_size_t generation_live{};
    std::atomic_size_t generation_high_watermark{1};
    std::atomic_uint64_t writer_epoch{};
};

auto VolatileShardPairPrototype::create(const std::size_t maximum_value_bytes,
                                        const std::size_t merge_delta_entries,
                                        const PrototypeWriterBatchConfig batch_config,
                                        const PrototypeCompletionNotifier completion_notifier)
    -> Result<std::unique_ptr<VolatileShardPairPrototype>> {
    if (maximum_value_bytes == 0 || merge_delta_entries == 0 || batch_config.max_records == 0 ||
        batch_config.max_records > kWriterMaximumBatchRecords || batch_config.max_wait.count() < 0 ||
        batch_config.max_wait > std::chrono::milliseconds{1}) {
        return fail(ErrorCode::invalid_argument, "paired prototype limits must be non-zero");
    }
    if (maximum_value_bytes > std::numeric_limits<std::size_t>::max() / kQueueCapacity) {
        return fail(ErrorCode::arithmetic_overflow, "paired prototype value arena overflows");
    }
    try {
        auto prototype =
            std::unique_ptr<VolatileShardPairPrototype>{new VolatileShardPairPrototype{std::make_unique<Impl>(
                maximum_value_bytes, merge_delta_entries, batch_config, completion_notifier)}};
        prototype->impl_->start();
        return prototype;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "paired prototype allocation failed");
    } catch (...) {
        return fail(ErrorCode::internal_error, "paired prototype startup failed");
    }
}

VolatileShardPairPrototype::VolatileShardPairPrototype(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

VolatileShardPairPrototype::~VolatileShardPairPrototype() {
    stop_and_drain();
}

auto VolatileShardPairPrototype::try_submit_put(const std::uint64_t request_id, const std::string_view key,
                                                const std::span<const std::byte> value,
                                                const std::uint64_t expire_at_ns) noexcept
    -> PrototypeSubmitStatus {
    return impl_->submit(PrototypeMutationKind::put, request_id, key, value, expire_at_ns);
}

auto VolatileShardPairPrototype::try_submit_erase(const std::uint64_t request_id,
                                                  const std::string_view key) noexcept
    -> PrototypeSubmitStatus {
    return impl_->submit(PrototypeMutationKind::erase, request_id, key, {}, 0);
}

auto VolatileShardPairPrototype::try_pop_completion() noexcept -> std::optional<PrototypeCompletion> {
    InternalCompletion completion;
    if (!impl_->completions.try_pop(completion)) {
        return std::nullopt;
    }
    impl_->free_slots[impl_->free_count++] = completion.slot;
    impl_->completion_pops.fetch_add(1U, std::memory_order_relaxed);
    return completion.completion;
}

void VolatileShardPairPrototype::adopt_publication() noexcept {
    // A turn boundary invalidates spans returned by the preceding turn. The
    // Writer waits for two such boundaries before reclaiming retired storage.
    impl_->reader_turns.fetch_add(1U, std::memory_order_seq_cst);
    const auto token = impl_->publication.load(std::memory_order_acquire);
    if (token == 0) {
        impl_->accepting.store(false, std::memory_order_release);
        impl_->invariant_failed.store(true, std::memory_order_release);
        return;
    }
    const auto slot = publication_token_slot(token);
    if (slot >= impl_->generations.size() ||
        impl_->generations[slot].descriptor.epoch != publication_token_epoch(token)) {
        impl_->accepting.store(false, std::memory_order_release);
        impl_->invariant_failed.store(true, std::memory_order_release);
        return;
    }
    impl_->local_publication = &impl_->generations[slot].descriptor;
}

auto VolatileShardPairPrototype::get(const std::string_view key, const std::uint64_t now_ns) noexcept
    -> std::optional<PrototypeRead> {
    ++impl_->reader_gets;
    const auto& generation = *impl_->local_publication->generation;
    const auto hash = hash_key(key);
    const auto* record = generation.delta->find(key, hash);
    if (record == nullptr) {
        record = generation.base->find(key, hash);
    }
    if (record == nullptr || record->tombstone ||
        (record->expire_at_ns != 0 && now_ns != 0 && record->expire_at_ns <= now_ns)) {
        return std::nullopt;
    }
    return PrototypeRead{.value = std::span<const std::byte>{record->value}, .sequence = record->sequence};
}

auto VolatileShardPairPrototype::pin_read_generation() noexcept -> PrototypeReadPin {
    const auto slot = impl_->local_publication->slot;
    impl_->generations[slot].output_pins.fetch_add(1U, std::memory_order_relaxed);
    const auto pins = impl_->generation_output_pins.fetch_add(1U, std::memory_order_relaxed) + 1U;
    update_high_watermark(impl_->generation_output_pin_high_watermark, pins);
    return PrototypeReadPin{impl_.get(), slot, &Impl::release_output_pin};
}

void VolatileShardPairPrototype::stop_and_drain() noexcept {
    if (!impl_) {
        return;
    }
    impl_->accepting.store(false, std::memory_order_release);
    // Complete a real adoption between two quiescent boundaries. This keeps
    // local_publication valid even when the Reader had not adopted the most
    // recent pre-shutdown generation. Any batch that cannot obtain a slot is
    // classified explicitly by the bounded exhaustion path.
    adopt_publication();
    impl_->reader_turns.fetch_add(1U, std::memory_order_seq_cst);
    if (impl_->writer.joinable()) {
        impl_->writer.join();
    }
    // The Writer may have published queued mutations during the join. Current
    // storage is never reclaimed, so this final acquire is lifetime-safe.
    const auto token = impl_->publication.load(std::memory_order_acquire);
    if (token == 0) {
        impl_->invariant_failed.store(true, std::memory_order_release);
        return;
    }
    const auto slot = publication_token_slot(token);
    if (slot < impl_->generations.size() &&
        impl_->generations[slot].descriptor.epoch == publication_token_epoch(token)) {
        impl_->local_publication = &impl_->generations[slot].descriptor;
    } else {
        impl_->invariant_failed.store(true, std::memory_order_release);
    }
}

auto VolatileShardPairPrototype::stats() const noexcept -> PrototypePairStats {
    return {
        .mutation_pushes = impl_->mutation_pushes.load(std::memory_order_relaxed),
        .mutation_pops = impl_->mutation_pops.load(std::memory_order_relaxed),
        .completion_pushes = impl_->completion_pushes.load(std::memory_order_relaxed),
        .completion_pops = impl_->completion_pops.load(std::memory_order_relaxed),
        .queue_full = impl_->queue_full.load(std::memory_order_relaxed),
        .mutation_queue_depth = impl_->mutations.size(),
        .mutation_queue_high_watermark = impl_->mutation_high_watermark.load(std::memory_order_relaxed),
        .completion_queue_depth = impl_->completions.size(),
        .completion_queue_high_watermark = impl_->completion_high_watermark.load(std::memory_order_relaxed),
        .last_writer_batch_size = impl_->last_batch_size.load(std::memory_order_relaxed),
        .maximum_writer_batch_size = impl_->maximum_batch_size.load(std::memory_order_relaxed),
        .writer_batch_wait_ns = impl_->writer_batch_wait_ns.load(std::memory_order_relaxed),
        .writer_batch_deadline_closes = impl_->writer_batch_deadline_closes.load(std::memory_order_relaxed),
        .reader_gets = impl_->reader_gets,
        .publications = impl_->publications.load(std::memory_order_relaxed),
        .publication_records = impl_->publication_records.load(std::memory_order_relaxed),
        .publication_latency_ns = impl_->publication_latency_ns.load(std::memory_order_relaxed),
        .publication_bytes = impl_->publication_bytes.load(std::memory_order_relaxed),
        .ingress_value_bytes_copied = impl_->ingress_value_bytes_copied.load(std::memory_order_relaxed),
        .payload_allocations = impl_->payload_allocations.load(std::memory_order_relaxed),
        .payload_bytes_allocated = impl_->payload_bytes_allocated.load(std::memory_order_relaxed),
        .delta_directory_entries_copied =
            impl_->delta_directory_entries_copied.load(std::memory_order_relaxed),
        .delta_page_view_entries_copied =
            impl_->delta_page_view_entries_copied.load(std::memory_order_relaxed),
        .delta_pages_copied = impl_->delta_pages_copied.load(std::memory_order_relaxed),
        .delta_pages_allocated = impl_->delta_pages_allocated.load(std::memory_order_relaxed),
        .delta_merges = impl_->delta_merges.load(std::memory_order_relaxed),
        .publication_backpressure = impl_->publication_backpressure.load(std::memory_order_relaxed),
        .generation_slot_exhaustions = impl_->generation_slot_exhaustions.load(std::memory_order_relaxed),
        .generation_slot_reuses = impl_->generation_slot_reuses.load(std::memory_order_relaxed),
        .generation_live = impl_->generation_live.load(std::memory_order_relaxed),
        .generation_high_watermark = impl_->generation_high_watermark.load(std::memory_order_relaxed),
        .generation_retire_count = impl_->generation_retire_count.load(std::memory_order_relaxed),
        .generation_retire_delay_ns = impl_->generation_retire_delay_ns.load(std::memory_order_relaxed),
        .generation_output_pins = impl_->generation_output_pins.load(std::memory_order_relaxed),
        .generation_output_pin_high_watermark =
            impl_->generation_output_pin_high_watermark.load(std::memory_order_relaxed),
        .generation_retire_pin_blocks = impl_->generation_retire_pin_blocks.load(std::memory_order_relaxed),
        .reader_turns = impl_->reader_turns.load(std::memory_order_relaxed),
        .reader_epoch = impl_->local_publication->epoch,
        .writer_epoch = impl_->writer_epoch.load(std::memory_order_relaxed),
        .visible_through = impl_->local_publication->visible_through};
}

} // namespace glifistore::experimental
