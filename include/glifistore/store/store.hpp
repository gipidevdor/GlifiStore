#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/persistence/store_backup.hpp"
#include "glifistore/store/config.hpp"
#include "glifistore/store/maintenance_types.hpp"
#include "glifistore/store/value.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace glifistore {

namespace detail {
class StoreAccess;
}

struct CompactionResult {
    bool compacted{};
    std::optional<std::size_t> worker_index{};
    std::uint64_t source_records_verified{};
    std::uint64_t source_bytes_verified{};
    std::uint64_t records_copied{};
    std::uint64_t bytes_copied{};
    std::uint64_t expired_records_dropped{};
    std::uint64_t pre_intent_duration_ns{};
    std::uint64_t publication_lease_duration_ns{};
    std::uint64_t pacing_delay_ns{};
    std::uint64_t pacing_sleep_count{};
    std::uint64_t pacing_burst_bytes{};
    std::uint64_t transient_metadata_lower_bound_bytes{};
};

/**
 * @brief Embedded key-value Store with paired Reader–Writer ownership domains.
 *
 * Public reads return owning values. The Store is neither copyable nor movable;
 * open() returns its owning handle. Persistence and acknowledgement policies are
 * selected through StoreConfig.
 * @see StoreConfig, OwnedValue
 */
class Store final {
  public:
    /// Open a Store using the supplied configuration; the default is volatile storage.
    [[nodiscard]] static auto open(const StoreConfig& config = {}) -> Result<std::unique_ptr<Store>>;
    ~Store();

    Store(const Store&) = delete;
    auto operator=(const Store&) -> Store& = delete;
    Store(Store&&) = delete;
    auto operator=(Store&&) -> Store& = delete;

    [[nodiscard]] auto worker_count() const noexcept -> std::size_t;

    /// Thread-safe owning read. Returned bytes and metadata are independent of
    /// later Store operations and remain valid after Store destruction.
    [[nodiscard]] auto get(std::string_view key) -> Result<OwnedValue>;
    [[nodiscard]] auto get(std::span<const std::byte> key) -> Result<OwnedValue>;
    [[nodiscard]] auto get_copy(std::string_view key) -> Result<OwnedValue>;
    [[nodiscard]] auto get_copy(std::span<const std::byte> key) -> Result<OwnedValue>;

    [[nodiscard]] auto put(std::string_view key, std::span<const std::byte> value,
                           std::uint64_t expire_at_ns = 0) -> Status;
    [[nodiscard]] auto put(std::span<const std::byte> key, std::span<const std::byte> value,
                           std::uint64_t expire_at_ns = 0) -> Status;
    /// Same-call batch of PUTs. Items may span owners: each shard is linearized and
    /// published independently (pairs are independent). Per-item Status values are
    /// returned in input order. Successful items on one shard share one publication
    /// epoch (≤32 per publish). Key/value views must remain live for the call.
    /// After return, every successful item is visible to GET (no early ACK).
    struct PutItem final {
        std::string_view key{};
        std::span<const std::byte> value{};
        std::uint64_t expire_at_ns{};
    };
    /// Apply PUT items, returning per-item status in input order. @see PutItem
    [[nodiscard]] auto put_batch(std::span<const PutItem> items) -> std::vector<Status>;
    [[nodiscard]] auto erase(std::string_view key) -> Status;
    [[nodiscard]] auto erase(std::span<const std::byte> key) -> Status;
    // Same-call batch of ERASEs. Items may span owners: each shard is linearized and
    // published independently (pairs are independent). Per-item Status values are
    // returned in input order. Successful items on one shard share one publication
    // epoch (≤32 per publish). Key views must remain live for the call.
    // After return, every successful erase is visible to GET (no early ACK).
    struct EraseItem final {
        std::string_view key{};
    };
    [[nodiscard]] auto erase_batch(std::span<const EraseItem> items) -> std::vector<Status>;

    [[nodiscard]] auto flush() -> Status;
    // Online durable backup while this Store holds the data-directory lock. Briefly fences new
    // admissions (writers/readers retry), flushes, then copies catalog files. Not a fully concurrent
    // hot copy during the copy window; external offline backup_durable_store still fails if locked.
    [[nodiscard]] auto backup_to(const std::filesystem::path& destination, bool scan_records = true)
        -> Result<DurableStoreBackupReport>;
    // Runs at most one Worker compaction transaction. Durable mode replaces a
    // whole sealed Worker history; volatile mode vacuums selected sparse sealed
    // Segments. Calls do not queue behind an existing compaction. An empty
    // successful result (`compacted == false`) means no Worker currently offers a
    // physical Segment gain. Durable no-gain planning still reports the Worker
    // examined and exact Index-referenced sealed Record/byte counts verified
    // before the exact layout rejected the rewrite.
    [[nodiscard]] auto compact() -> Result<CompactionResult>;
    // Observability for the optional MaintenanceController (ADR 0023).
    [[nodiscard]] auto maintenance_snapshot() const -> MaintenanceSnapshot;
    // Idempotently prevents new operations, waits for calls already in
    // progress, flushes durable state, stops background executors, and releases
    // Store resources. Destruction performs the same shutdown as a
    // non-observable fallback.
    [[nodiscard]] auto close() -> Status;

    [[nodiscard]] auto verify_index() const -> Status;

  private:
    struct Impl;

    explicit Store(std::unique_ptr<Impl> impl);
    [[nodiscard]] auto compact_for_maintenance(std::optional<std::size_t> preferred_worker,
                                               std::uint64_t max_copy_bytes) -> Result<CompactionResult>;

    std::unique_ptr<Impl> impl_;

    friend class detail::StoreAccess;
};

} // namespace glifistore
