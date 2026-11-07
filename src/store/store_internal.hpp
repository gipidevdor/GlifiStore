#pragma once

#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/segment/record.hpp"
#include "glifistore/segment/segment.hpp"
#include "glifistore/store/config.hpp"
#include "glifistore/store/maintenance.hpp"
#include "glifistore/store/paired/shard_pair_runtime.hpp"
#include "glifistore/store/prepared_read.hpp"
#include "glifistore/store/store.hpp"
#include "glifistore/worker/worker.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace glifistore::detail {

// Internal bridge for the native server and white-box tests. This header is
// never installed and is not part of the supported C++ API.
class StoreAccess final {
  public:
    enum class MutationOperation : std::uint8_t { put, erase };

    struct DurableMutationView final {
        MutationOperation operation{};
        HashedKey key{};
        std::span<const std::byte> value{};
        std::uint64_t expire_at_ns{};
    };

    struct DurableWriterBatchResult final {
        DurableMutationResult mutation{};
        bool conflict_retried{};
    };

    struct VolatileMutationPublication final {
        RecordRef record{};
        SegmentPtr segment{};
        Opcode opcode{Opcode::put};
    };

    struct PreparedGet final {
        std::optional<OwnedValue> value{};
        std::optional<PreparedColdRead> cold{};
    };

    using DurablePublishedRead = DurableRuntimeCatalog::PublishedReadRecord;
    using DurablePublishedReadView = DurableRuntimeCatalog::PublishedReadView;
    using DurableReadSnapshot = DurableRuntimeCatalog::PublishedReadSnapshot;

    [[nodiscard]] static auto get_owned(Store& store, std::size_t worker_index, const HashedKey& key,
                                        std::uint64_t now_ns) -> Result<OwnedValue>;
    // Returns an owning value/error when a GET can complete without file I/O;
    // an empty optional means the durable Record requires a pinned cold read.
    [[nodiscard]] static auto prepare_get_owned(Store& store, std::size_t worker_index, const HashedKey& key,
                                                std::uint64_t now_ns) -> Result<PreparedGet>;
    [[nodiscard]] static auto complete_get_owned(Store& store, std::size_t worker_index,
                                                 PreparedColdRead read,
                                                 const ColdReadCancellation* cancellation = nullptr,
                                                 std::vector<std::byte>* scratch = nullptr)
        -> Result<OwnedValue>;
    [[nodiscard]] static auto snapshot_durable_reads(Store& store, std::size_t worker_index,
                                                     bool allow_fail_closed = false)
        -> Result<DurableReadSnapshot>;
    [[nodiscard]] static auto durable_read_catalog_revision(const Store& store,
                                                            std::size_t worker_index) noexcept
        -> std::uint64_t;
    [[nodiscard]] static auto capture_durable_read(Store& store, std::size_t worker_index,
                                                   const HashedKey& key) -> Result<DurablePublishedRead>;
    [[nodiscard]] static auto prepare_published_durable_get(Store& store, std::size_t worker_index,
                                                            DurablePublishedReadView read,
                                                            std::uint64_t now_ns) -> Result<PreparedGet>;
    [[nodiscard]] static auto put(Store& store, std::size_t worker_index, const HashedKey& key,
                                  std::span<const std::byte> value, std::uint64_t expire_at_ns) -> Status;
    [[nodiscard]] static auto erase(Store& store, std::size_t worker_index, const HashedKey& key) -> Status;
    // PublishedAdmission::caller_holds_guard: Writer-only fast path when the submitting
    // thread already owns Store::Impl::OperationGuard for this shard (embedded sync
    // mutate). Skips a nested admission RMW; close() still waits on the caller's guard.
    // The maintenance emergency gate is still checked — only the OperationGuard is skipped.
    // Async submissions must use check_admission — they have no outer OperationGuard.
    enum class PublishedAdmission : std::uint8_t { check_admission, caller_holds_guard };

    [[nodiscard]] static auto
    put_volatile_published(Store& store, std::size_t worker_index, const HashedKey& key,
                           std::span<const std::byte> value, std::uint64_t expire_at_ns,
                           PublishedAdmission admission = PublishedAdmission::check_admission)
        -> Result<VolatileMutationPublication>;
    [[nodiscard]] static auto
    erase_volatile_published(Store& store, std::size_t worker_index, const HashedKey& key,
                             PublishedAdmission admission = PublishedAdmission::check_admission)
        -> Result<VolatileMutationPublication>;
    // Durable daemon path retaining the kernel's committed/not-committed/
    // indeterminate classification. This is required for a bounded internal
    // retry without inferring safety from a portable ErrorCode alone.
    [[nodiscard]] static auto put_durable(Store& store, std::size_t worker_index, const HashedKey& key,
                                          std::span<const std::byte> value, std::uint64_t expire_at_ns)
        -> DurableMutationResult;
    [[nodiscard]] static auto erase_durable(Store& store, std::size_t worker_index, const HashedKey& key)
        -> DurableMutationResult;
    [[nodiscard]] static auto durable_writer_batch_config(const Store& store) noexcept
        -> std::optional<DurableGroupConfig>;
    [[nodiscard]] static auto mutate_durable_batch(Store& store, std::size_t worker_index,
                                                   std::span<const DurableMutationView> mutations)
        -> std::vector<DurableWriterBatchResult>;

    // ErrorCode alone cannot authorize replay: committed and indeterminate
    // results are never retryable even if they carry sequence_conflict.
    [[nodiscard]] static auto should_retry_durable_mutation(const DurableMutationResult& result,
                                                            unsigned attempt) noexcept -> bool {
        return attempt == 0 && result.outcome == DurableMutationOutcome::not_committed && result.error &&
               result.error->code == ErrorCode::sequence_conflict;
    }

    [[nodiscard]] static auto is_durable(const Store& store) noexcept -> bool;
    [[nodiscard]] static auto worker_routing(const Store& store) noexcept -> WorkerRoutingState;
    [[nodiscard]] static auto batch_stats(const Store& store) -> std::vector<DurableBatchWorkerStats>;
    [[nodiscard]] static auto maintenance_controller(Store& store) noexcept -> MaintenanceController*;
    static void report_foreground_latency(Store& store, std::uint64_t latency_ns) noexcept;
    [[nodiscard]] static auto maintenance_mutations_rejected(const Store& store) noexcept -> bool;
    // True while the Store accepts new operations and any durable catalog remains healthy.
    [[nodiscard]] static auto operational(const Store& store) noexcept -> bool;
    // True while begin_operation admissions are open (false during online backup fence).
    [[nodiscard]] static auto admissions_open(const Store& store) noexcept -> bool;
    static void mark_fail_closed(Store& store) noexcept;

    [[nodiscard]] static auto worker(const Store& store, std::size_t index) noexcept -> const Worker&;
    [[nodiscard]] static auto segments(const Store& store) -> std::vector<SegmentPtr>;
    // Durable-only. Lexicographically sorted live Index keys for offline tools.
    [[nodiscard]] static auto snapshot_live_keys(Store& store) -> Result<std::vector<std::string>>;
    [[nodiscard]] static auto durable_manifest(const Store& store) -> Result<Manifest>;
    [[nodiscard]] static auto attach_paired_runtime(Store& store, const StoreConfig& config) -> Status;
    [[nodiscard]] static auto shard_pair_runtime(Store& store) noexcept -> store::paired::ShardPairRuntime*;
    [[nodiscard]] static auto concurrency(const Store& store) noexcept -> StoreConcurrencyMode;
};

} // namespace glifistore::detail
