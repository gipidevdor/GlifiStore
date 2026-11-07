#include "glifistore/store/paired/read_generation.hpp"

#include "glifistore/core/hot_path_phases.hpp"
#include "glifistore/index/swiss_control_group.hpp"
#include "glifistore/store/paired/generation_slot_pool.hpp"

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

#include "store/paired/read_generation_impl.hpp"

namespace glifistore::store::paired {

auto immutable_base_spare_mapping_bytes() noexcept -> std::size_t {
    return immutable_base_spare_mapping_payload_bytes.load(std::memory_order_relaxed);
}

PairReadGeneration::PairReadGeneration(const WorkerRoutingState routing,
                                       std::shared_ptr<const ImmutableReadIndex> base,
                                       const DeltaState* delta, const std::uint64_t epoch,
                                       const std::uint64_t visible_through) noexcept
    : routing_(routing), base_(std::move(base)), delta_(delta), epoch_(epoch),
      visible_through_(visible_through) {}

auto PairReadGeneration::get(const HashedKey& key, const std::uint64_t now_ns) const -> Result<OwnedValue> {
    GS_PHASE_GET(index_lookup);
    const auto* delta_record = delta_->find(key);
    if (delta_record == nullptr) {
        return base_->get(key, now_ns);
    }
    const auto record = delta_record_view(*delta_record, key.hash);
    if (record.opcode == Opcode::erase) {
        return fail(ErrorCode::not_found, "key not found");
    }
    if (record.durable != nullptr) {
        return fail(ErrorCode::invalid_argument, "durable read generation requires asynchronous preparation");
    }
    auto decoded = decode_pinned(record);
    if (!decoded) {
        return unexpected(std::move(decoded.error()));
    }
    if (decoded->key_string() != key.key) {
        return fail(ErrorCode::invalid_reference, "read generation Index key mismatch");
    }
    if (decoded->expired(now_ns)) {
        return fail(ErrorCode::not_found, "key not found");
    }
    GS_PHASE_GET(value_copy);
    return OwnedValue::from_bytes(decoded->value, decoded->sequence.value, decoded->expire_at_ns,
                                  decoded->type);
}

auto PairReadGeneration::prepare_durable(const HashedKey& key) const
    -> Result<DurableRuntimeCatalog::PublishedReadView> {
    const auto* delta_record = delta_->find(key);
    if (delta_record == nullptr) {
        return base_->prepare_durable(key);
    }
    const auto record = delta_record_view(*delta_record, key.hash);
    if (record.opcode == Opcode::erase) {
        return fail(ErrorCode::not_found, "key not found");
    }
    if (record.durable == nullptr || record.hash != key.hash || record.key != key.key ||
        !record.durable->matches(record.record)) {
        return fail(ErrorCode::invalid_reference, "durable read generation has no exact file-generation pin");
    }
    return DurableRuntimeCatalog::PublishedReadView::borrow(record.key, record.hash, record.record,
                                                            *record.durable);
}

auto PairReadGeneration::delta_entries() const noexcept -> std::size_t {
    return delta_->size();
}

auto PairReadGeneration::delta_record_versions() const noexcept -> std::size_t {
    return delta_->record_versions();
}

auto PairReadGeneration::delta_arena_record_bytes() const noexcept -> std::size_t {
    return delta_->arena_record_bytes();
}

auto PairReadGeneration::delta_arena_key_bytes() const noexcept -> std::size_t {
    return delta_->arena_key_bytes();
}

auto PairReadGeneration::delta_arena_key_storage_bytes() const noexcept -> std::size_t {
    return delta_->arena_key_storage_bytes();
}

auto PairReadGeneration::base_entries() const noexcept -> std::size_t {
    return base_->size();
}

auto PairReadGeneration::memory_stats() const noexcept -> ReadGenerationMemoryStats {
    auto stats = base_->memory_stats();
    delta_->append_memory_stats(stats);
    stats.generation_shell_bytes = sizeof(PairReadGenerationEnableShared);
    stats.current_allocated_lower_bound_bytes =
        saturating_add(stats.generation_shell_bytes, saturating_add(stats.base_allocated_lower_bound_bytes,
                                                                    stats.delta_allocated_lower_bound_bytes));
    return stats;
}

} // namespace glifistore::store::paired
