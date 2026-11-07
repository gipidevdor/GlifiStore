#pragma once

// generation_internals: delta arena, record helpers, compact records.
// Structure-debt extraction from read_generation_impl.

#include "glifistore/core/hot_path_phases.hpp"
#include "glifistore/index/swiss_control_group.hpp"
#include "glifistore/store/paired/generation_slot_pool.hpp"
#include "glifistore/store/paired/read_generation.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace glifistore::store::paired {
inline namespace generation_internals {

inline constexpr std::size_t kDeltaPageSlots = 16;
inline constexpr std::size_t kDeltaDirectoryBlockPages = 16;
// Second COW level above directory blocks. A full 40 960-entry Delta uses 256
// blocks; copying that spine on every single-record publication was a mixed-path
// tax. Chunks keep per-publication spine traffic to one 16-block group.
inline constexpr std::size_t kDeltaDirectoryChunkBlocks = 16;
inline constexpr std::size_t kFlatDeltaMaximumPages = 32;
inline constexpr std::size_t kMaximumPublicationBatch = 32;
inline constexpr std::size_t kDeltaArenaBlockRecords = 64;
inline constexpr std::size_t kLargeImmutableArrayMappingThreshold = 1U << 20U;
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
inline constexpr bool kLargeImmutableArrayMappingEnabled = false;
#else
inline constexpr bool kLargeImmutableArrayMappingEnabled = true;
#endif
#elif defined(__SANITIZE_ADDRESS__)
inline constexpr bool kLargeImmutableArrayMappingEnabled = false;
#else
inline constexpr bool kLargeImmutableArrayMappingEnabled = true;
#endif
inline std::atomic_size_t immutable_base_spare_mapping_payload_bytes{};
static_assert(kDeltaPageSlots % kSwissGroupSize == 0);

#if defined(__unix__) || defined(__APPLE__)
class LargeImmutableMappingPool final {
  public:
    LargeImmutableMappingPool() noexcept = default;
    LargeImmutableMappingPool(const LargeImmutableMappingPool&) = delete;
    auto operator=(const LargeImmutableMappingPool&) -> LargeImmutableMappingPool& = delete;

    ~LargeImmutableMappingPool() {
        std::scoped_lock lock{mutex_};
        release_locked();
    }

    [[nodiscard]] auto take(const std::size_t mapped_bytes) noexcept -> void* {
        std::scoped_lock lock{mutex_};
        if (base_ == nullptr) {
            return nullptr;
        }
        if (mapped_bytes != mapped_bytes_) {
            release_locked();
            return nullptr;
        }
        auto* result = base_;
        immutable_base_spare_mapping_payload_bytes.fetch_sub(payload_bytes_, std::memory_order_relaxed);
        base_ = nullptr;
        mapped_bytes_ = 0;
        payload_bytes_ = 0;
        return result;
    }

    void put(void* base, const std::size_t mapped_bytes, const std::size_t payload_bytes) noexcept {
        std::scoped_lock lock{mutex_};
        release_locked();
        base_ = base;
        mapped_bytes_ = mapped_bytes;
        payload_bytes_ = payload_bytes;
        immutable_base_spare_mapping_payload_bytes.fetch_add(payload_bytes, std::memory_order_relaxed);
    }

  private:
    void release_locked() noexcept {
        if (base_ == nullptr) {
            return;
        }
        immutable_base_spare_mapping_payload_bytes.fetch_sub(payload_bytes_, std::memory_order_relaxed);
        static_cast<void>(::munmap(base_, mapped_bytes_));
        base_ = nullptr;
        mapped_bytes_ = 0;
        payload_bytes_ = 0;
    }

    std::mutex mutex_;
    void* base_{};
    std::size_t mapped_bytes_{};
    std::size_t payload_bytes_{};
};
#endif

// Immutable base rebuilds grow by merge quanta. General-purpose allocators may
// retain every previously used large size, turning bounded live generations
// into unbounded resident high-water. Large arrays therefore use their own
// anonymous mappings in geometric size classes. Each immutable-base lineage
// keeps at most one retired mapping: successive same-class rebuilds alternate
// between two bounded buffers instead of paying zero-fill faults every time.
// A class change or thread exit unmaps the spare. Guard pages preserve coarse
// over/underflow detection even outside ASan.
template <typename T> class ReleasingLargeAllocator {
  public:
    using value_type = T;
    using is_always_equal = std::false_type;
    using propagate_on_container_move_assignment = std::true_type;

#if defined(__unix__) || defined(__APPLE__)
    ReleasingLargeAllocator() : pool_(std::make_shared<LargeImmutableMappingPool>()) {}

    explicit ReleasingLargeAllocator(std::shared_ptr<LargeImmutableMappingPool> pool) noexcept
        : pool_(std::move(pool)) {}

    template <typename U>
    ReleasingLargeAllocator(const ReleasingLargeAllocator<U>& other) noexcept : pool_(other.pool_) {}

    [[nodiscard]] auto mapping_pool() const noexcept -> const std::shared_ptr<LargeImmutableMappingPool>& {
        return pool_;
    }
#else
    ReleasingLargeAllocator() noexcept = default;

    template <typename U> ReleasingLargeAllocator(const ReleasingLargeAllocator<U>&) noexcept {}
#endif

    [[nodiscard]] static constexpr auto mapped_storage_bytes(const std::size_t count) noexcept
        -> std::size_t {
#if defined(__unix__) || defined(__APPLE__)
        if (count <= std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            const auto bytes = count * sizeof(T);
            return kLargeImmutableArrayMappingEnabled && bytes >= kLargeImmutableArrayMappingThreshold ? bytes
                                                                                                       : 0U;
        }
#else
        static_cast<void>(count);
#endif
        return 0U;
    }

#if defined(__unix__) || defined(__APPLE__)
    struct MappingLayout final {
        std::size_t page_size{};
        std::size_t payload_bytes{};
        std::size_t mapped_bytes{};
    };

    [[nodiscard]] static auto layout_for(const std::size_t bytes) -> MappingLayout {
        const auto page_size_value = ::sysconf(_SC_PAGESIZE);
        if (page_size_value <= 0) {
            throw std::bad_alloc{};
        }
        const auto page_size = static_cast<std::size_t>(page_size_value);
        if (page_size > std::numeric_limits<std::size_t>::max() / 2U) {
            throw std::bad_array_new_length{};
        }
        auto payload_class = kLargeImmutableArrayMappingThreshold;
        while (payload_class < bytes) {
            if (payload_class > std::numeric_limits<std::size_t>::max() / 2U) {
                throw std::bad_array_new_length{};
            }
            payload_class *= 2U;
        }
        if (payload_class > std::numeric_limits<std::size_t>::max() - (page_size - 1U)) {
            throw std::bad_array_new_length{};
        }
        const auto payload_bytes = ((payload_class + page_size - 1U) / page_size) * page_size;
        if (payload_bytes > std::numeric_limits<std::size_t>::max() - 2U * page_size) {
            throw std::bad_array_new_length{};
        }
        return {.page_size = page_size,
                .payload_bytes = payload_bytes,
                .mapped_bytes = payload_bytes + 2U * page_size};
    }

#endif

    [[nodiscard]] auto allocate(const std::size_t count) -> T* {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length{};
        }
        const auto bytes = count * sizeof(T);
#if defined(__unix__) || defined(__APPLE__)
        if (kLargeImmutableArrayMappingEnabled && bytes >= kLargeImmutableArrayMappingThreshold) {
            const auto layout = layout_for(bytes);
            auto* mapping = pool_->take(layout.mapped_bytes);
            if (mapping == nullptr) {
#if defined(MAP_ANONYMOUS)
                constexpr auto kAnonymousFlag = MAP_ANONYMOUS;
#else
                constexpr auto kAnonymousFlag = MAP_ANON;
#endif
                mapping = ::mmap(nullptr, layout.mapped_bytes, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | kAnonymousFlag, -1, 0);
                if (mapping == MAP_FAILED) {
                    throw std::bad_alloc{};
                }
                auto* trailing_guard =
                    static_cast<std::byte*>(mapping) + layout.page_size + layout.payload_bytes;
                if (::mprotect(mapping, layout.page_size, PROT_NONE) != 0 ||
                    ::mprotect(trailing_guard, layout.page_size, PROT_NONE) != 0) {
                    static_cast<void>(::munmap(mapping, layout.mapped_bytes));
                    throw std::bad_alloc{};
                }
            }
            return reinterpret_cast<T*>(static_cast<std::byte*>(mapping) + layout.page_size);
        }
#endif
        return std::allocator<T>{}.allocate(count);
    }

    void deallocate(T* pointer, const std::size_t count) noexcept {
        const auto bytes = count * sizeof(T);
#if defined(__unix__) || defined(__APPLE__)
        if (kLargeImmutableArrayMappingEnabled && bytes >= kLargeImmutableArrayMappingThreshold) {
            try {
                const auto layout = layout_for(bytes);
                pool_->put(reinterpret_cast<std::byte*>(pointer) - layout.page_size, layout.mapped_bytes,
                           layout.payload_bytes);
                return;
            } catch (...) {
                std::terminate();
            }
        }
#endif
        std::allocator<T>{}.deallocate(pointer, count);
    }

    template <typename U>
    [[nodiscard]] auto operator==(const ReleasingLargeAllocator<U>& other) const noexcept -> bool {
#if defined(__unix__) || defined(__APPLE__)
        return pool_ == other.pool_;
#else
        static_cast<void>(other);
        return true;
#endif
    }

  private:
#if defined(__unix__) || defined(__APPLE__)
    std::shared_ptr<LargeImmutableMappingPool> pool_;
    template <typename> friend class ReleasingLargeAllocator;
#endif
};

struct ReadRecord final {
    std::uint64_t hash{};
    std::string key;
    RecordRef record;
    SegmentPtr segment;
    std::optional<DurableRuntimeCatalog::PublishedReadRecord> durable;
    Opcode opcode{Opcode::put};
};

using ReadRecordHandle = std::shared_ptr<const ReadRecord>;
using MutableReadIndex = std::vector<ReadRecordHandle>;
using DurableReadPin = DurableRuntimeCatalog::PublishedReadPin;

[[nodiscard]] inline auto record_key(const ReadRecord& record) noexcept -> std::string_view {
    return record.durable ? record.durable->key() : std::string_view{record.key};
}

struct ReadRecordView final {
    std::uint64_t hash{};
    std::string_view key;
    RecordRef record;
    const SegmentPtr* segment{};
    const DurableReadPin* durable{};
    Opcode opcode{Opcode::put};
};

[[nodiscard]] inline auto view_of(const ReadRecord& record) noexcept -> ReadRecordView {
    return {.hash = record.hash,
            .key = record_key(record),
            .record = record.record,
            .segment = record.segment ? &record.segment : nullptr,
            .durable = record.durable ? &record.durable->pin() : nullptr,
            .opcode = record.opcode};
}

[[nodiscard]] inline auto view_of(const ReadMutation& mutation) noexcept -> ReadRecordView {
    return {.hash = mutation.key.hash,
            .key = mutation.key.key,
            .record = mutation.record,
            .segment = mutation.segment ? &mutation.segment : nullptr,
            .durable = mutation.durable ? &mutation.durable->pin() : nullptr,
            .opcode = mutation.opcode};
}

[[nodiscard]] inline auto materialize(const ReadRecordView& view) -> ReadRecordHandle {
    std::optional<DurableRuntimeCatalog::PublishedReadRecord> durable;
    if (view.durable != nullptr) {
        durable.emplace(DurableRuntimeCatalog::PublishedReadRecord::bind(std::string{view.key}, view.hash,
                                                                         view.record, *view.durable));
    }
    return std::make_shared<const ReadRecord>(ReadRecord{
        .hash = view.hash,
        .key = durable ? std::string{} : std::string{view.key},
        .record = view.record,
        .segment = view.segment == nullptr ? SegmentPtr{} : *view.segment,
        .durable = std::move(durable),
        .opcode = view.opcode,
    });
}

[[nodiscard]] inline auto record_less(const ReadRecordHandle& left, const ReadRecordHandle& right) noexcept
    -> bool {
    return left->hash < right->hash || (left->hash == right->hash && record_key(*left) < record_key(*right));
}

inline void apply_record(MutableReadIndex& index, ReadRecordHandle record) {
    const auto position = std::lower_bound(index.begin(), index.end(), record, record_less);
    const bool present = position != index.end() && (*position)->hash == record->hash &&
                         record_key(**position) == record_key(*record);
    if (record->opcode == Opcode::erase) {
        if (present) {
            index.erase(position);
        }
        return;
    }
    if (present) {
        *position = std::move(record);
    } else {
        index.insert(position, std::move(record));
    }
}

[[nodiscard]] inline auto fingerprint(const std::uint64_t hash) noexcept -> std::uint8_t {
    const auto result = static_cast<std::uint8_t>(hash & 0x7FU);
    return result == 0 ? static_cast<std::uint8_t>(1) : result;
}

[[nodiscard]] constexpr auto saturating_add(const std::size_t left, const std::size_t right) noexcept
    -> std::size_t {
    return right > std::numeric_limits<std::size_t>::max() - left ? std::numeric_limits<std::size_t>::max()
                                                                  : left + right;
}

[[nodiscard]] constexpr auto saturating_multiply(const std::size_t left, const std::size_t right) noexcept
    -> std::size_t {
    return left != 0 && right > std::numeric_limits<std::size_t>::max() / left
               ? std::numeric_limits<std::size_t>::max()
               : left * right;
}

[[nodiscard]] inline auto same_segment(const SegmentPtr& segment, const RecordRef& record) noexcept -> bool {
    return segment && segment->id() == record.segment_id && segment->generation() == record.generation;
}

[[nodiscard]] inline auto decode_pinned(const ReadRecordView& record) -> Result<RecordView> {
    if (record.segment == nullptr || !same_segment(*record.segment, record.record)) {
        return fail(ErrorCode::invalid_reference, "read generation has no exact Segment generation pin");
    }
    const auto offset = static_cast<std::size_t>(record.record.offset.value);
    const auto size = static_cast<std::size_t>(record.record.size.value);
    if (offset > (*record.segment)->capacity() || size > (*record.segment)->capacity() - offset) {
        return fail(ErrorCode::invalid_reference, "read generation RecordRef exceeds pinned Segment");
    }
    auto decoded = decode_record({(*record.segment)->base() + offset, size});
    if (!decoded) {
        return unexpected(std::move(decoded.error()));
    }
    if (decoded->sequence != record.record.sequence || decoded->encoded_size != record.record.size.value ||
        decoded->opcode != record.opcode) {
        return fail(ErrorCode::invalid_reference, "read generation RecordRef identity mismatch");
    }
    return std::move(*decoded);
}

// One immutable Delta cell. Ownership of keys and pins is lifted to the
// generation arena; Swiss pages copy only this stable pointer. The Writer is
// the sole appender and Readers can only observe fully constructed cells that
// were published through a generation release/acquire edge.
struct alignas(64) DeltaRecord final {
    union KeyStorage {
        std::array<char, 16> inline_key{};
        const char* external_key;
    };

    RecordRef record;
    KeyStorage key;
    const void* pin{};
    std::uint32_t key_size{};
    Opcode opcode{Opcode::put};
    bool durable{};
};

static_assert(sizeof(DeltaRecord) == 64, "a Delta record must occupy exactly one cache line");
static_assert(std::is_trivially_copyable_v<DeltaRecord>);

[[nodiscard]] inline auto delta_record_key(const DeltaRecord& record) noexcept -> std::string_view {
    return record.key_size <= record.key.inline_key.size()
               ? std::string_view{record.key.inline_key.data(), record.key_size}
               : std::string_view{record.key.external_key, record.key_size};
}

[[nodiscard]] inline auto delta_record_view(const DeltaRecord& record, const std::uint64_t hash) noexcept
    -> ReadRecordView {
    return {.hash = hash,
            .key = delta_record_key(record),
            .record = record.record,
            .segment = !record.durable && record.pin != nullptr ? static_cast<const SegmentPtr*>(record.pin)
                                                                : nullptr,
            .durable = record.durable && record.pin != nullptr
                           ? static_cast<const DurableReadPin*>(record.pin)
                           : nullptr,
            .opcode = record.opcode};
}

struct DeltaKeyBlock final {
    std::unique_ptr<char[]> bytes;
    std::size_t capacity{};
    std::size_t used{};
};

struct DeltaRecordBlock final {
    std::unique_ptr<DeltaRecord[]> records;
    std::size_t used{};
};

class DeltaArena final {
  public:
    explicit DeltaArena(const std::size_t maximum_records) : maximum_records_(maximum_records) {
        record_blocks_.reserve((maximum_records + kDeltaArenaBlockRecords - 1U) / kDeltaArenaBlockRecords);
    }

    [[nodiscard]] auto store(const ReadRecordView& source) -> const DeltaRecord* {
        if (record_count_ == maximum_records_ ||
            source.key.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::bad_alloc{};
        }
        ensure_record_capacity();
        auto* external_key = source.key.size() > DeltaRecord::KeyStorage{}.inline_key.size()
                                 ? reserve_key(source.key.size())
                                 : nullptr;
        const void* pin = nullptr;
        bool durable{};
        if (source.opcode == Opcode::put && source.durable != nullptr) {
            durable = true;
            pin = retain_durable_pin(*source.durable);
        } else if (source.opcode == Opcode::put && source.segment != nullptr) {
            pin = retain_segment_pin(SegmentPtr{*source.segment}, source.record);
        }

        auto& block = record_blocks_.back();
        auto* destination = &block.records[block.used++];
        destination->record = source.record;
        destination->pin = pin;
        destination->key_size = static_cast<std::uint32_t>(source.key.size());
        destination->opcode = source.opcode;
        destination->durable = durable;
        if (external_key == nullptr) {
            std::ranges::copy(source.key, destination->key.inline_key.begin());
        } else {
            std::ranges::copy(source.key, external_key);
            destination->key.external_key = external_key;
            key_blocks_.back().used += source.key.size();
            key_bytes_ = saturating_add(key_bytes_, source.key.size());
        }
        ++record_count_;
        return destination;
    }

    [[nodiscard]] auto record_count() const noexcept -> std::size_t {
        return record_count_;
    }

    [[nodiscard]] auto available_records() const noexcept -> std::size_t {
        return maximum_records_ - record_count_;
    }

    [[nodiscard]] auto allocated_record_bytes() const noexcept -> std::size_t {
        return saturating_multiply(saturating_multiply(record_blocks_.size(), kDeltaArenaBlockRecords),
                                   sizeof(DeltaRecord));
    }

    [[nodiscard]] auto key_bytes() const noexcept -> std::size_t {
        return key_bytes_;
    }

    [[nodiscard]] auto allocated_key_bytes() const noexcept -> std::size_t {
        return allocated_key_bytes_;
    }

  private:
    void ensure_record_capacity() {
        if (!record_blocks_.empty() && record_blocks_.back().used != kDeltaArenaBlockRecords) {
            return;
        }
        auto records = std::make_unique_for_overwrite<DeltaRecord[]>(kDeltaArenaBlockRecords);
        DeltaRecordBlock block{.records = std::move(records)};
        record_blocks_.push_back(std::move(block));
    }

    [[nodiscard]] auto reserve_key(const std::size_t size) -> char* {
        constexpr std::size_t kKeyBlockBytes = std::size_t{4} * 1024U;
        if (key_blocks_.empty() || size > key_blocks_.back().capacity - key_blocks_.back().used) {
            const auto capacity = std::max(kKeyBlockBytes, size);
            auto bytes = std::make_unique<char[]>(capacity);
            DeltaKeyBlock block{.bytes = std::move(bytes), .capacity = capacity};
            key_blocks_.push_back(std::move(block));
            allocated_key_bytes_ = saturating_add(allocated_key_bytes_, capacity);
        }
        return key_blocks_.back().bytes.get() + key_blocks_.back().used;
    }

    // Pins live in a deque so element addresses stay stable across push_back
    // (DeltaRecord::pin stores a raw SegmentPtr*). By-value intake lets callers
    // move a SegmentPtr in; steady-state same-segment hits avoid any insert.
    [[nodiscard]] auto retain_segment_pin(SegmentPtr source, const RecordRef& record) -> const SegmentPtr* {
        if (!segment_pins_.empty() && same_segment(segment_pins_.back(), record)) {
            return &segment_pins_.back();
        }
        const auto found = std::find_if(segment_pins_.begin(), segment_pins_.end(),
                                        [&](const auto& pin) { return same_segment(pin, record); });
        if (found != segment_pins_.end()) {
            return &*found;
        }
        segment_pins_.push_back(std::move(source));
        return &segment_pins_.back();
    }

    [[nodiscard]] auto retain_durable_pin(const DurableReadPin& source) -> const DurableReadPin* {
        if (!durable_pins_.empty() && durable_pins_.back()->same_generation(source)) {
            return durable_pins_.back().get();
        }
        const auto found = std::find_if(durable_pins_.begin(), durable_pins_.end(),
                                        [&](const auto& pin) { return pin->same_generation(source); });
        if (found != durable_pins_.end()) {
            return found->get();
        }
        auto pin = std::make_unique<DurableReadPin>(source);
        const auto* result = pin.get();
        durable_pins_.push_back(std::move(pin));
        return result;
    }

    const std::size_t maximum_records_;
    std::size_t record_count_{};
    std::size_t key_bytes_{};
    std::size_t allocated_key_bytes_{};
    std::vector<DeltaRecordBlock> record_blocks_;
    std::vector<DeltaKeyBlock> key_blocks_;
    std::deque<SegmentPtr> segment_pins_;
    std::vector<std::unique_ptr<DurableReadPin>> durable_pins_;
};

struct DeltaPage final {
    DeltaPage() {
        control.fill(kSwissEmpty);
    }

    std::array<std::uint8_t, kDeltaPageSlots> control{};
    std::array<std::uint64_t, kDeltaPageSlots> hashes{};
    std::array<const DeltaRecord*, kDeltaPageSlots> records{};
};

struct DeltaDirectoryBlock final {
    std::array<std::shared_ptr<const DeltaPage>, kDeltaDirectoryBlockPages> pages{};
};

struct DeltaDirectoryChunk final {
    std::array<std::shared_ptr<const DeltaDirectoryBlock>, kDeltaDirectoryChunkBlocks> blocks{};
};

struct DeltaBuilderScratch final {
    DeltaBuilderScratch() {
        mutable_chunks.reserve(kMaximumPublicationBatch);
        mutable_blocks.reserve(kMaximumPublicationBatch);
        mutable_pages.reserve(kMaximumPublicationBatch);
    }

    void reset() noexcept {
        mutable_chunks.clear();
        mutable_blocks.clear();
        mutable_pages.clear();
    }

    std::vector<std::pair<std::size_t, std::shared_ptr<DeltaDirectoryChunk>>> mutable_chunks;
    std::vector<std::pair<std::size_t, std::shared_ptr<DeltaDirectoryBlock>>> mutable_blocks;
    std::vector<std::pair<std::size_t, std::shared_ptr<DeltaPage>>> mutable_pages;
};

[[nodiscard]] inline auto copy_flat_delta_spine(const std::vector<std::shared_ptr<const DeltaPage>>& source)
    -> std::vector<std::shared_ptr<const DeltaPage>> {
    GS_PHASE_PUT(delta_flat_spine_copy);
    return source;
}

[[nodiscard]] inline auto
copy_directory_delta_spine(const std::vector<std::shared_ptr<const DeltaDirectoryChunk>>& source)
    -> std::vector<std::shared_ptr<const DeltaDirectoryChunk>> {
    GS_PHASE_PUT(delta_directory_spine_copy);
    return source;
}

[[nodiscard]] inline auto current_delta_builder_scratch() -> DeltaBuilderScratch& {
    thread_local DeltaBuilderScratch scratch;
    return scratch;
}

[[nodiscard]] inline auto post_delta_builder_scratch() -> DeltaBuilderScratch& {
    thread_local DeltaBuilderScratch scratch;
    return scratch;
}

struct CompactReadRecord;

struct CompactReadRecord final {
    union KeyStorage {
        std::array<char, 16> inline_key{};
        const char* external_key;
    };

    RecordRef record{};
    std::uint64_t hash{};
    KeyStorage key{};
    std::uint32_t key_size{};
    std::uint32_t pin_and_flags{};

    static constexpr std::uint32_t kDurableFlag = 1U << 31U;
    static constexpr std::uint32_t kEraseFlag = 1U << 30U;
    static constexpr std::uint32_t kPinMask = kEraseFlag - 1U;
    static constexpr std::uint32_t kNoPin = kPinMask;

    [[nodiscard]] auto pin_index() const noexcept -> std::uint32_t {
        return pin_and_flags & kPinMask;
    }

    [[nodiscard]] auto durable() const noexcept -> bool {
        return (pin_and_flags & kDurableFlag) != 0;
    }

    [[nodiscard]] auto opcode() const noexcept -> Opcode {
        return (pin_and_flags & kEraseFlag) != 0 ? Opcode::erase : Opcode::put;
    }

    void set_metadata(const std::uint32_t pin_index, const bool is_durable,
                      const Opcode record_opcode) noexcept {
        pin_and_flags =
            pin_index | (is_durable ? kDurableFlag : 0U) | (record_opcode == Opcode::erase ? kEraseFlag : 0U);
    }
};

static_assert(sizeof(CompactReadRecord) == 64,
              "the immutable Reader record and full hash must occupy one cache line");

struct KeyBlock final {
    std::unique_ptr<char[]> bytes;
    std::size_t capacity{};
    std::size_t used{};
};

} // namespace generation_internals

} // namespace glifistore::store::paired
