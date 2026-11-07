/**
 * @file
 * @brief Owning embedded Store API with per-shard mutation publication.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/persistence/store_backup.hpp"
#include "glifistore/store/blob.hpp"
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

/**
 * @brief Outcome and measured work of one compaction attempt.
 *
 * A successful call can perform no rewrite: inspect compacted before treating the
 * result as a physical gain. Counters describe this attempt, including exact
 * no-gain scans. Durations use monotonic wall-clock nanoseconds; the metadata
 * figure is a lower bound, not peak RSS.
 */
struct CompactionResult {
    bool compacted{}; ///< Whether a replacement history was actually published.
    /// Selected Worker, including an exact durable no-gain attempt.
    std::optional<std::size_t> worker_index{};
    std::uint64_t source_records_verified{}; ///< Index-referenced source Records checked by exact planning.
    std::uint64_t source_bytes_verified{};   ///< Encoded source Record bytes checked by exact planning.
    std::uint64_t records_copied{};          ///< Live Records copied into replacement Segments.
    std::uint64_t bytes_copied{};            ///< Encoded live Record bytes copied.
    std::uint64_t expired_records_dropped{}; ///< Expired Records omitted after validation.
    std::uint64_t pre_intent_duration_ns{};  ///< Scan and private output work before the Manifest lease.
    std::uint64_t publication_lease_duration_ns{}; ///< Time holding the recovery-sensitive publication lease.
    std::uint64_t pacing_delay_ns{};               ///< Time charged to automatic private-output pacing.
    std::uint64_t pacing_sleep_count{};            ///< Number of pacing waits.
    std::uint64_t pacing_burst_bytes{}; ///< Initial physical-write burst allowed by the pacing budget.
    /// Accounted transient metadata, excluding unmeasured overhead.
    std::uint64_t transient_metadata_lower_bound_bytes{};
    std::uint64_t snapshot_duration_ns{};    ///< Time capturing the compaction snapshot.
    std::uint64_t commit_gate_duration_ns{}; ///< Time spent in the final compaction commit gate.
    /// Time replaying concurrent Index changes into the replacement.
    std::uint64_t delta_replay_duration_ns{};
    std::uint64_t delta_records{};   ///< Journal entries replayed for concurrent mutations.
    std::uint64_t delta_key_bytes{}; ///< Key bytes retained by the compaction delta journal.
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
    /**
     * @brief Open a Store; default configuration selects volatile paired storage.
     * @param config Startup configuration; durable reopen validates persisted identity.
     * @return An owning handle after initialization/recovery, or an error without a usable Store.
     * @note Worker count and routing are fixed for the opened Store.
     */
    [[nodiscard]] static auto open(const StoreConfig& config = {}) -> Result<std::unique_ptr<Store>>;
    /**
     * @brief Close and release resources; shutdown failures cannot be returned.
     * @see close() for observable shutdown errors.
     * @pre No other thread may access the object during destruction.
     */
    ~Store();

    Store(const Store&) = delete;
    auto operator=(const Store&) -> Store& = delete;
    Store(Store&&) = delete;
    auto operator=(Store&&) -> Store& = delete;

    /**
     * @brief Return the immutable Worker count, including after close().
     * @note This metadata query does not imply that operation admission is open.
     */
    [[nodiscard]] auto worker_count() const noexcept -> std::size_t;

    /**
     * @brief Thread-safe owning read of the latest visible value.
     * @param key Binary key, borrowed until return; embedded NUL bytes are significant.
     * @return Independent bytes and metadata, or not_found for absence, tombstone, or expiration.
     * @note Success is an owning read; the result survives later mutation and Store destruction.
     */
    [[nodiscard]] auto get(std::string_view key) -> Result<OwnedValue>;
    /**
     * @brief Owning read using a binary byte-span key.
     * @see get(std::string_view) for lifetime and absence semantics.
     */
    [[nodiscard]] auto get(std::span<const std::byte> key) -> Result<OwnedValue>;
    /**
     * @brief Read an owning value, materializing a blob when the blob profile is enabled.
     * @return Owned bytes or an error; concurrent blob materialization is budgeted.
     * @see get(std::string_view)
     */
    [[nodiscard]] auto get_copy(std::string_view key) -> Result<OwnedValue>;
    /**
     * @brief Byte-span overload of the owning read.
     * @see get_copy(std::string_view)
     */
    [[nodiscard]] auto get_copy(std::span<const std::byte> key) -> Result<OwnedValue>;
    /**
     * @brief Begin a bounded upload without changing current key visibility.
     * @param key Binary key copied into transfer state before return.
     * @param options Expiration and optional exact final size.
     * @return An owning writer, or an error when the profile/admission/budgets disallow it.
     * @note Requires the explicit durable embedded blob profile (ADR 0042).
     * @see BlobWriter::commit()
     */
    [[nodiscard]] auto begin_blob_put(std::string_view key, BlobOptions options = {})
        -> Result<std::unique_ptr<BlobWriter>>;
    /**
     * @brief Open a sequential reader for the currently visible value.
     * @return An owning transfer handle, or an error such as not_found or unavailable.
     * @note The reader pins its selected value; close() invalidates further transfer I/O.
     * @see BlobReader
     */
    [[nodiscard]] auto open_blob(std::string_view key) -> Result<std::unique_ptr<BlobReader>>;
    /**
     * @brief Flush, capture catalog roots, and collect unreferenced blob files.
     * @return Removed/retained file counts, or a profile, flush, or I/O error.
     * @note Active transfer roots participate in retention; the operation is explicit.
     */
    [[nodiscard]] auto collect_blobs() -> Result<BlobGcResult>;

    /**
     * @brief Publish a value for a complete binary key.
     * @param key Key bytes borrowed until return.
     * @param value Value bytes borrowed until return; no input view is retained.
     * @param expire_at_ns Absolute Unix nanoseconds; zero disables expiration.
     * @return Success after read publication, or an admission, validation, capacity, or I/O error.
     * @note Persistence at acknowledgement depends on StoreConfig::storage_mode.
     */
    [[nodiscard]] auto put(std::string_view key, std::span<const std::byte> value,
                           std::uint64_t expire_at_ns = 0) -> Status;
    /**
     * @brief Byte-span overload of put() with identical lifetime and expiration rules.
     * @see put(std::string_view, std::span<const std::byte>, std::uint64_t)
     */
    [[nodiscard]] auto put(std::span<const std::byte> key, std::span<const std::byte> value,
                           std::uint64_t expire_at_ns = 0) -> Status;

    /// Same-call batch of PUTs. Items may span owners: each shard is linearized and
    /// published independently (pairs are independent). Per-item Status values are
    /// returned in input order. Successful items on one shard share one publication
    /// epoch (≤32 per publish). Key/value views must remain live for the call.
    /// After return, every successful item is visible to GET (no early ACK).
    struct PutItem final {
        std::string_view key{};             ///< Binary key borrowed for the enclosing batch call.
        std::span<const std::byte> value{}; ///< Value bytes borrowed for the enclosing batch call.
        std::uint64_t expire_at_ns{};       ///< Absolute Unix nanoseconds; zero means no expiration.
    };

    /// Apply PUT items, returning per-item status in input order. @see PutItem
    [[nodiscard]] auto put_batch(std::span<const PutItem> items) -> std::vector<Status>;
    /**
     * @brief Remove key visibility, recording a durable tombstone when required.
     * @param key Binary key borrowed until return.
     * @return Success after publication, or not_found for an absent/expired key; other failures propagate.
     */
    [[nodiscard]] auto erase(std::string_view key) -> Status;
    /**
     * @brief Byte-span overload of erase() with identical publication/absence semantics.
     */
    [[nodiscard]] auto erase(std::span<const std::byte> key) -> Status;

    /// Same-call batch of ERASEs. Items may span owners: each shard is linearized and
    /// published independently (pairs are independent). Per-item Status values are
    /// returned in input order. Successful items on one shard share one publication
    /// epoch (≤32 per publish). Key views must remain live for the call.
    /// After return, every successful erase is visible to GET (no early ACK).
    struct EraseItem final {
        std::string_view key{}; ///< Binary key borrowed for the enclosing batch call.
    };

    /**
     * @brief Apply independent erases and return one status per item in input order.
     * @param items Key views borrowed for this call only.
     * @note Each owner preserves its subsequence; this is not a cross-key transaction.
     * @see EraseItem, put_batch()
     */
    [[nodiscard]] auto erase_batch(std::span<const EraseItem> items) -> std::vector<Status>;

    /**
     * @brief Wait for the configured persistence boundary of mutations covered by this call.
     * @return Success (a no-op in volatile mode), or a flush/availability error.
     * @note Later concurrent mutations need not be covered; a durable failure can fail the runtime closed.
     */
    [[nodiscard]] auto flush() -> Status;
    /// Online durable backup while this Store holds the data-directory lock. Briefly fences new
    /// admissions (writers/readers retry), flushes, then copies catalog files. Not a fully concurrent
    /// hot copy during the copy window; external offline backup_durable_store still fails if locked.
    [[nodiscard]] auto backup_to(const std::filesystem::path& destination, bool scan_records = true)
        -> Result<DurableStoreBackupReport>;
    /// Runs at most one Worker compaction transaction. Durable mode replaces a
    /// whole sealed Worker history; volatile mode vacuums selected sparse sealed
    /// Segments. Calls do not queue behind an existing compaction. An empty
    /// successful result (`compacted == false`) means no Worker currently offers a
    /// physical Segment gain. Durable no-gain planning still reports the Worker
    /// examined and exact Index-referenced sealed Record/byte counts verified
    /// before the exact layout rejected the rewrite.
    [[nodiscard]] auto compact() -> Result<CompactionResult>;
    /// Observability for the optional MaintenanceController (ADR 0023).
    [[nodiscard]] auto maintenance_snapshot() const -> MaintenanceSnapshot;
    /// Idempotently prevents new operations, waits for calls already in
    /// progress, flushes durable state, stops background executors, and releases
    /// Store resources. Destruction performs the same shutdown as a
    /// non-observable fallback.
    [[nodiscard]] auto close() -> Status;

    /**
     * @brief Check synchronized in-memory Index/reference invariants for diagnostics.
     * @return Success at the checked point, or the detected invariant/availability error.
     * @note May briefly stop Worker progress; this is not a full disk scrub.
     */
    [[nodiscard]] auto verify_index() const -> Status;

  private:
    struct Impl;

    explicit Store(std::unique_ptr<Impl> impl);
    [[nodiscard]] auto get_record(std::string_view key) -> Result<OwnedValue>;
    [[nodiscard]] auto put_record(std::string_view key, std::span<const std::byte> value,
                                  std::uint64_t expire_at_ns, ValueType type) -> Status;
    [[nodiscard]] auto compact_for_maintenance(std::optional<std::size_t> preferred_worker,
                                               std::uint64_t max_copy_bytes) -> Result<CompactionResult>;

    std::unique_ptr<Impl> impl_;

    friend class detail::StoreAccess;
};

} // namespace glifistore
