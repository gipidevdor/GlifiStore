/**
 * @file
 * @brief One partition owner with serialized mutation and explicit Index access guards.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/index/index.hpp"
#include "glifistore/segment/global_manager.hpp"
#include "glifistore/segment/record.hpp"
#include "glifistore/vacuum/vacuum.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace glifistore {

class Store;

namespace detail {
class StoreAccess;
}

/**
 * @brief Linearized mutation reference paired with the Segment ownership required for read publication.
 */
struct WorkerMutationPublication final {
    RecordRef record;
    SegmentPtr segment;
    Opcode opcode{Opcode::put};
};

/**
 * @brief One partition Index and Segment history. Paired mutations require exclusive Writer ownership;
 * diagnostic guards coordinate separately.
 */
class Worker final {
  public:
    Worker(WorkerId id, GlobalSegmentManager& manager, WorkerRoutingState routing = get_worker_routing());
    Worker(const Worker&) = delete;
    auto operator=(const Worker&) -> Worker& = delete;
    Worker(Worker&&) = delete;
    auto operator=(Worker&&) -> Worker& = delete;

    [[nodiscard]] auto id() const noexcept -> WorkerId {
        return id_;
    }

    [[nodiscard]] auto index() const noexcept -> const Index& {
        return index_;
    }

    [[nodiscard]] auto owned_segments() const noexcept -> const std::vector<SegmentPtr>& {
        return owned_;
    }

    /// Expired keys are tombstoned and removed from the Index on read.
    [[nodiscard]] auto get(const HashedKey& key, std::uint64_t now_ns = 0) -> Result<RecordView>;
    [[nodiscard]] auto put(const HashedKey& key, std::span<const std::byte> value,
                           std::uint64_t expire_at_ns = 0) -> Status;
    [[nodiscard]] auto erase(const HashedKey& key) -> Status;
    [[nodiscard]] auto compact(std::uint64_t now_ns, VacuumPolicy policy = {})
        -> Result<std::optional<VacuumStats>>;

    /// Paired exclusive Writer (ADR 0032): ordinary put/erase publication must not
    /// acquire mutex_. Compaction / verify_index observe hot_path_depth + the Index
    /// quiesce gate instead (mutex alone does not serialize with unlocked publish).
    void set_exclusive_writer(bool exclusive) noexcept {
        exclusive_writer_ = exclusive;
    }

    [[nodiscard]] auto exclusive_writer() const noexcept -> bool {
        return exclusive_writer_;
    }

  private:
    struct ExclusiveHotPathGuard final {
        Worker* worker{};

        explicit ExclusiveHotPathGuard(Worker* owner) noexcept : worker(owner) {
            if (worker != nullptr) {
                worker->hot_path_depth_.fetch_add(1U, std::memory_order_acq_rel);
            }
        }

        ~ExclusiveHotPathGuard() {
            if (worker == nullptr) {
                return;
            }
            if (worker->hot_path_depth_.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
                worker->hot_path_depth_.notify_all();
            }
        }

        ExclusiveHotPathGuard(const ExclusiveHotPathGuard&) = delete;
        auto operator=(const ExclusiveHotPathGuard&) -> ExclusiveHotPathGuard& = delete;
    };

    struct ExclusiveIndexQuiesce final {
        Worker& worker;
        bool armed{};

        explicit ExclusiveIndexQuiesce(Worker& owner, const bool enable) noexcept : worker(owner) {
            if (!enable) {
                return;
            }
            worker.arm_index_quiesce_gate();
            armed = true;
            for (;;) {
                const auto depth = worker.hot_path_depth_.load(std::memory_order_acquire);
                if (depth == 0) {
                    break;
                }
                worker.hot_path_depth_.wait(depth, std::memory_order_acquire);
            }
        }

        void clear() noexcept {
            if (!armed) {
                return;
            }
            armed = false;
            worker.disarm_index_quiesce_gate();
        }

        ~ExclusiveIndexQuiesce() {
            clear();
        }

        ExclusiveIndexQuiesce(const ExclusiveIndexQuiesce&) = delete;
        auto operator=(const ExclusiveIndexQuiesce&) -> ExclusiveIndexQuiesce& = delete;
    };

    [[nodiscard]] auto get_locked(const HashedKey& key, std::uint64_t now_ns) -> Result<RecordView>;
    [[nodiscard]] auto put_locked(const HashedKey& key, std::span<const std::byte> value,
                                  std::uint64_t expire_at_ns) -> Status;
    [[nodiscard]] auto erase_locked(const HashedKey& key) -> Status;
    [[nodiscard]] auto put_locked_published(const HashedKey& key, std::span<const std::byte> value,
                                            std::uint64_t expire_at_ns) -> Result<WorkerMutationPublication>;
    [[nodiscard]] auto erase_locked_published(const HashedKey& key) -> Result<WorkerMutationPublication>;
    [[nodiscard]] auto next_sequence() -> SequenceNumber;
    void register_owned_segment(SegmentPtr segment);
    void unregister_owned_segment(SegmentId id) noexcept;
    [[nodiscard]] auto find_owned_segment(SegmentId id) noexcept -> Segment*;
    [[nodiscard]] auto find_owned_segment(SegmentId id) const noexcept -> const Segment*;
    void maybe_retire(Segment& segment);
    [[nodiscard]] auto append_record(const RecordInput& input) -> Result<RecordRef>;
    [[nodiscard]] auto read_ref(const RecordRef& ref) const -> Result<RecordView>;
    [[nodiscard]] auto publish(const HashedKey& key, const RecordRef& ref, Segment& segment) -> Status;

    void arm_index_quiesce_gate() noexcept {
        if (index_quiesce_holders_.fetch_add(1U, std::memory_order_acq_rel) == 0U) {
            index_quiesce_active_.store(true, std::memory_order_release);
        }
    }

    void disarm_index_quiesce_gate() noexcept {
        if (index_quiesce_holders_.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
            index_quiesce_active_.store(false, std::memory_order_release);
        }
    }

    WorkerId id_;
    GlobalSegmentManager& manager_;
    Index index_;
    SegmentPtr active_;
    SequenceNumber next_sequence_{SequenceNumber{1}};
    std::vector<SegmentPtr> owned_;
    std::unordered_map<SegmentId, Segment*> owned_by_id_;
    mutable std::mutex mutex_;
    bool exclusive_writer_{};
    /// Exclusive Writer Index ownership (mirror durable ExclusiveIndexQuiesce).
    std::atomic_bool index_quiesce_active_{};
    std::atomic_uint32_t index_quiesce_holders_{};
    std::atomic_uint32_t hot_path_depth_{};

    friend class Store;
    friend class detail::StoreAccess;
};

} // namespace glifistore
