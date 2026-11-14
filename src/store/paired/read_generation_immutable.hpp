#pragma once

// ImmutableReadIndex: published generation base index.
// Structure-debt extraction from read_generation_impl.

#include "glifistore/core/memory_reserve.hpp"
#include "glifistore/core/pageable_array.hpp"
#include "store/paired/read_generation_internals.hpp"

namespace glifistore::store::paired {

class ImmutableReadIndex final {
  public:
    ImmutableReadIndex() : ImmutableReadIndex(std::make_shared<LargeImmutableMappingPool>()) {}

    /// All storage of this base and of every base built from it (merges, snapshot replacements)
    /// is charged to the pool's memory reserve, like the Worker Index (ADR 0046).
    explicit ImmutableReadIndex(std::shared_ptr<LargeImmutableMappingPool> mapping_pool)
        : ImmutableReadIndex(std::move(mapping_pool), 0) {}

    ImmutableReadIndex(MutableReadIndex source, std::shared_ptr<LargeImmutableMappingPool> mapping_pool)
        : ImmutableReadIndex(std::move(mapping_pool), source.size()) {
        for (const auto& record : source) {
            append(*record);
        }
    }

    ImmutableReadIndex(const std::span<const DurableRuntimeCatalog::PublishedReadRecord> source,
                       std::shared_ptr<LargeImmutableMappingPool> mapping_pool)
        : ImmutableReadIndex(std::move(mapping_pool), source.size()) {
        for (const auto& record : source) {
            append({.hash = record.key_hash(),
                    .key = record.key(),
                    .record = record.reference(),
                    .durable = &record.pin(),
                    .opcode = Opcode::put});
        }
    }

    [[nodiscard]] auto mapping_pool() const -> std::shared_ptr<LargeImmutableMappingPool> {
        return records_.get_allocator().mapping_pool();
    }

    [[nodiscard]] auto find(const HashedKey& key) const noexcept -> const CompactReadRecord* {
        const auto wanted = fingerprint(key.hash);
        auto group_start = probe_start(key.hash);
        for (std::size_t probed = 0; probed < capacity_; probed += kSwissGroupSize) {
            const auto* group = &control_[group_start];
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
                const auto slot = group_start + offset;
                const auto record_index = record_indices_[slot];
                if (record_index >= records_.size()) {
                    return nullptr;
                }
                const auto* record = &records_[record_index];
                if (record->hash == key.hash && key_at(*record) == key.key) {
                    return record;
                }
            }
            group_start = next_group(group_start);
        }
        return nullptr;
    }

    [[nodiscard]] auto get(const HashedKey& key, const std::uint64_t now_ns) const -> Result<OwnedValue> {
        const auto* record = find(key);
        if (record == nullptr || record->opcode() == Opcode::erase) {
            return fail(ErrorCode::not_found, "key not found");
        }
        const auto pin_index = record->pin_index();
        if (record->durable() || pin_index == CompactReadRecord::kNoPin || pin_index >= segments_.size()) {
            return fail(record->durable() ? ErrorCode::invalid_argument : ErrorCode::invalid_reference,
                        record->durable() ? "durable read generation requires asynchronous preparation"
                                          : "read generation has no exact Segment generation pin");
        }
        const auto& segment = segments_[pin_index];
        if (!same_segment(segment, record->record)) {
            return fail(ErrorCode::invalid_reference, "read generation has no exact Segment generation pin");
        }
        const auto offset = static_cast<std::size_t>(record->record.offset.value);
        const auto size = static_cast<std::size_t>(record->record.size.value);
        if (offset > segment->capacity() || size > segment->capacity() - offset) {
            return fail(ErrorCode::invalid_reference, "read generation RecordRef exceeds pinned Segment");
        }
        auto decoded = decode_record({segment->base() + offset, size});
        if (!decoded) {
            return unexpected(std::move(decoded.error()));
        }
        if (decoded->sequence != record->record.sequence ||
            decoded->encoded_size != record->record.size.value || decoded->opcode != record->opcode() ||
            decoded->key_string() != key.key) {
            return fail(ErrorCode::invalid_reference, "read generation RecordRef identity mismatch");
        }
        if (decoded->expired(now_ns)) {
            return fail(ErrorCode::not_found, "key not found");
        }
        return OwnedValue::from_bytes(decoded->value, decoded->sequence.value, decoded->expire_at_ns,
                                      decoded->type);
    }

    [[nodiscard]] auto prepare_durable(const HashedKey& key) const
        -> Result<DurableRuntimeCatalog::PublishedReadView> {
        const auto* record = find(key);
        if (record == nullptr || record->opcode() == Opcode::erase) {
            return fail(ErrorCode::not_found, "key not found");
        }
        const auto pin_index = record->pin_index();
        if (!record->durable() || pin_index == CompactReadRecord::kNoPin ||
            pin_index >= durable_pins_.size() || !durable_pins_[pin_index].matches(record->record)) {
            return fail(ErrorCode::invalid_reference,
                        "durable read generation has no exact file-generation pin");
        }
        return DurableRuntimeCatalog::PublishedReadView::borrow(key_at(*record), key.hash, record->record,
                                                                durable_pins_[pin_index]);
    }

    [[nodiscard]] auto records() const -> MutableReadIndex {
        MutableReadIndex result;
        result.reserve(size_);
        for (std::size_t slot = 0; slot < capacity_; ++slot) {
            if (control_[slot] != kSwissEmpty) {
                const auto record_index = record_indices_[slot];
                if (record_index < records_.size()) {
                    result.push_back(materialize(view_at(records_[record_index])));
                }
            }
        }
        std::ranges::sort(result, record_less);
        return result;
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return size_;
    }

    [[nodiscard]] auto capacity() const noexcept -> std::size_t {
        return capacity_;
    }

    [[nodiscard]] auto record_at(const std::size_t slot) const noexcept -> std::optional<ReadRecordView> {
        if (slot >= capacity_) {
            return std::nullopt;
        }
        if (control_[slot] == kSwissEmpty || record_indices_[slot] >= records_.size()) {
            return std::nullopt;
        }
        return view_at(records_[record_indices_[slot]]);
    }

    [[nodiscard]] auto memory_stats() const noexcept -> ReadGenerationMemoryStats {
        ReadGenerationMemoryStats stats{
            .base_entries = size_,
            .base_capacity = capacity_,
            .base_record_storage_bytes = saturating_multiply(records_.capacity(), sizeof(CompactReadRecord)),
            .base_record_mapped_storage_bytes =
                ReleasingLargeAllocator<CompactReadRecord>::mapped_storage_bytes(records_.capacity()),
            .base_lookup_storage_bytes =
                saturating_multiply(capacity_, sizeof(std::uint8_t) + sizeof(std::uint32_t)),
            .base_key_bytes = key_bytes_,
            .base_pin_storage_bytes =
                saturating_add(saturating_multiply(segments_.capacity(), sizeof(SegmentPtr)),
                               saturating_multiply(durable_pins_.capacity(), sizeof(DurableReadPin))),
        };
        stats.base_key_storage_bytes =
            saturating_add(saturating_multiply(key_blocks_.capacity(), sizeof(PageableRegion)),
                           allocated_key_storage_bytes_);
        auto total = sizeof(ImmutableReadIndex);
        total = saturating_add(total, stats.base_record_storage_bytes);
        total = saturating_add(total, stats.base_lookup_storage_bytes);
        total = saturating_add(total, stats.base_key_storage_bytes);
        total = saturating_add(total, stats.base_pin_storage_bytes);
        stats.base_allocated_lower_bound_bytes = total;
        return stats;
    }

  private:
    using RecordAllocator = ReleasingLargeAllocator<CompactReadRecord>;

    ImmutableReadIndex(std::shared_ptr<LargeImmutableMappingPool> mapping_pool,
                       const std::size_t maximum_records)
        : records_(RecordAllocator{mapping_pool}), control_(mapping_pool->memory_reserve()),
          record_indices_(mapping_pool->memory_reserve()) {
        initialize(maximum_records, true);
    }

    [[nodiscard]] static constexpr auto maximum_occupancy(const std::size_t capacity) noexcept
        -> std::size_t {
        return capacity - capacity / 4U;
    }

    [[nodiscard]] auto probe_start(const std::uint64_t hash) const noexcept -> std::size_t {
        return ((hash >> 7U) & ((capacity_ / kSwissGroupSize) - 1U)) * kSwissGroupSize;
    }

    [[nodiscard]] auto next_group(const std::size_t group_start) const noexcept -> std::size_t {
        const auto next = group_start + kSwissGroupSize;
        return next == capacity_ ? 0 : next;
    }

    void initialize(const std::size_t maximum_records, const bool initialize_control) {
        if (maximum_records > std::numeric_limits<std::uint32_t>::max()) {
            throw std::bad_alloc{};
        }
        capacity_ = kSwissGroupSize;
        while (maximum_occupancy(capacity_) < maximum_records) {
            if (capacity_ > std::numeric_limits<std::size_t>::max() / 2U) {
                throw std::bad_alloc{};
            }
            capacity_ *= 2U;
        }
        // No element is written here: an incremental builder fills control slots in bounded steps.
        control_.resize_for_overwrite(capacity_);
        record_indices_.resize_for_overwrite(capacity_);
        if (initialize_control) {
            std::fill_n(control_.data(), capacity_, kSwissEmpty);
        }
        records_.reserve(maximum_records);
    }

    void initialize_control_slots(const std::size_t first, const std::size_t count) noexcept {
        std::fill_n(control_.data() + first, count, kSwissEmpty);
    }

    [[nodiscard]] auto key_at(const CompactReadRecord& record) const noexcept -> std::string_view {
        if (record.key_size == 0) {
            return {};
        }
        if (record.key_size <= record.key.inline_key.size()) {
            return {record.key.inline_key.data(), record.key_size};
        }
        return {record.key.external_key, record.key_size};
    }

    [[nodiscard]] auto view_at(const CompactReadRecord& record) const noexcept -> ReadRecordView {
        const auto pin_index = record.pin_index();
        return {.hash = record.hash,
                .key = key_at(record),
                .record = record.record,
                .segment = !record.durable() && pin_index != CompactReadRecord::kNoPin ? &segments_[pin_index]
                                                                                       : nullptr,
                .durable = record.durable() && pin_index != CompactReadRecord::kNoPin
                               ? &durable_pins_[pin_index]
                               : nullptr,
                .opcode = record.opcode()};
    }

    void append(const ReadRecord& source) {
        append(view_of(source));
    }

    void append(const ReadRecordView& source) {
        if (records_.size() == records_.capacity()) {
            throw std::bad_alloc{};
        }
        if (source.key.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::bad_alloc{};
        }
        auto pin_index = CompactReadRecord::kNoPin;
        bool durable{};
        if (source.opcode == Opcode::put && source.durable != nullptr) {
            durable = true;
            const auto found = std::ranges::find_if(
                durable_pins_, [&](const auto& pin) { return pin.same_generation(*source.durable); });
            if (found == durable_pins_.end()) {
                if (durable_pins_.size() == CompactReadRecord::kNoPin) {
                    throw std::bad_alloc{};
                }
                pin_index = static_cast<std::uint32_t>(durable_pins_.size());
                durable_pins_.push_back(*source.durable);
            } else {
                pin_index = static_cast<std::uint32_t>(std::distance(durable_pins_.begin(), found));
            }
        } else if (source.opcode == Opcode::put && source.segment != nullptr) {
            const auto found = std::ranges::find(segments_, *source.segment);
            if (found == segments_.end()) {
                if (segments_.size() == CompactReadRecord::kNoPin) {
                    throw std::bad_alloc{};
                }
                pin_index = static_cast<std::uint32_t>(segments_.size());
                segments_.push_back(*source.segment);
            } else {
                pin_index = static_cast<std::uint32_t>(std::distance(segments_.begin(), found));
            }
        }
        CompactReadRecord compact{.record = source.record,
                                  .hash = source.hash,
                                  .key_size = static_cast<std::uint32_t>(source.key.size())};
        compact.set_metadata(pin_index, durable, source.opcode);
        if (source.key.size() <= compact.key.inline_key.size()) {
            std::ranges::copy(source.key, compact.key.inline_key.begin());
        } else {
            compact.key.external_key = append_key(source.key);
        }
        records_.push_back(std::move(compact));
        place(source.hash, records_.size() - 1U);
    }

    [[nodiscard]] auto append_key(const std::string_view key) -> const char* {
        if (key.empty()) {
            return nullptr;
        }
        constexpr std::size_t kKeyBlockBytes = std::size_t{64} * 1024U;
        if (key_blocks_.empty() || key.size() > key_blocks_.back().size() - key_block_used_) {
            const auto capacity = std::max(kKeyBlockBytes, key.size());
            key_blocks_.reserve(key_blocks_.size() + 1U);
            // Region data never moves, so external key pointers stay valid as the vector grows.
            key_blocks_.push_back(
                PageableRegion::allocate(capacity, PageableRegion::Use::index, control_.memory_reserve()));
            key_block_used_ = 0;
            allocated_key_storage_bytes_ = saturating_add(allocated_key_storage_bytes_, capacity);
        }
        auto* destination = reinterpret_cast<char*>(key_blocks_.back().data()) + key_block_used_;
        std::ranges::copy(key, destination);
        key_block_used_ += key.size();
        key_bytes_ = saturating_add(key_bytes_, key.size());
        return destination;
    }

    void place(const std::uint64_t hash, const std::size_t record_index) {
        if (record_index > std::numeric_limits<std::uint32_t>::max()) {
            throw std::bad_alloc{};
        }
        auto group_start = probe_start(hash);
        for (;;) {
            for (std::size_t offset = 0; offset < kSwissGroupSize; ++offset) {
                const auto slot = group_start + offset;
                if (control_[slot] == kSwissEmpty) {
                    control_[slot] = fingerprint(hash);
                    record_indices_[slot] = static_cast<std::uint32_t>(record_index);
                    ++size_;
                    return;
                }
            }
            group_start = next_group(group_start);
        }
    }

    std::size_t capacity_{};
    std::size_t size_{};
    std::size_t key_bytes_{};
    std::size_t allocated_key_storage_bytes_{};
    std::vector<CompactReadRecord, RecordAllocator> records_;
    std::vector<PageableRegion> key_blocks_;
    std::size_t key_block_used_{};
    std::vector<SegmentPtr> segments_;
    std::vector<DurableReadPin> durable_pins_;
    PageableArray<std::uint8_t> control_;
    PageableArray<std::uint32_t> record_indices_;

    friend class IncrementalBaseBuilder;
};

} // namespace glifistore::store::paired
