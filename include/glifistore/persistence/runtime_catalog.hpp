/**
 * @file
 * @brief Live durable catalog, pinned reads, mutation commits, and maintenance coordination.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/persistence/compaction_builder.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/manifest.hpp"
#include "glifistore/persistence/namespace_audit.hpp"
#include "glifistore/persistence/recovery.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/persistence/store_backup.hpp"
#include "glifistore/segment/record.hpp"
#include "glifistore/store/config.hpp"
#include "glifistore/store/maintenance_types.hpp"
#include "glifistore/store/prepared_read.hpp"
#include "glifistore/store/value.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace glifistore {

class DurableFlushCoordinator;

namespace detail {
class StoreAccess;
class BlobStorage;
} // namespace detail

namespace store::paired {
class ImmutableReadIndex;
class PairReadGeneration;
} // namespace store::paired

namespace server {
using store::paired::ImmutableReadIndex;
using store::paired::PairReadGeneration;
} // namespace server

/**
 * @brief Commit knowledge independent of read publication; indeterminate must not be interpreted as
 * rejection.
 */
enum class DurableMutationOutcome { committed, not_committed, indeterminate };
/**
 * @brief Distinguish a published rewrite, an unchanged catalog, exact no-gain, and restart-required failure.
 */
enum class DurableCompactionOutcome { compacted, not_compacted, not_beneficial, recovery_required };

/**
 * @brief Blob storage, commit/batching policy, resource limits, and exclusive-Writer inputs.
 */
struct DurableRuntimeOptions {
    std::shared_ptr<detail::BlobStorage> blob_storage{};
    SegmentCommitSync commit_sync{SegmentCommitSync::immediate};
    std::uint32_t sync_interval_ms{1000};
    std::optional<DurableGroupConfig> batch{};
    bool strict_ack{false};
    DurableResourceLimits limits{};
    /// Paired exclusive Writer (ADR 0032): ordinary mutate skips the Worker mutex
    /// when no background flusher shares the shard (hot_path_depth). Mutable Index
    /// readers (prepare_get, snapshot_live_keys, snapshot_published_reads,
    /// capture_published_read, compaction Phase A, unread TTL probe,
    /// flush_dirty_segments) arm compaction_commit_active and drain depth before
    /// Index / dirty-file touch.
    bool exclusive_writer{false};
};

/**
 * @brief Durable authority knowledge plus optional assigned sequence and error.
 */
struct DurableMutationResult {
    DurableMutationOutcome outcome{DurableMutationOutcome::not_committed};
    std::optional<SequenceNumber> sequence{};
    std::optional<Error> error{};

    [[nodiscard]] auto committed() const noexcept -> bool {
        return outcome == DurableMutationOutcome::committed;
    }
};

/**
 * @brief Compaction authority outcome, measured work, and optional failure.
 */
struct DurableCompactionResult {
    DurableCompactionOutcome outcome{DurableCompactionOutcome::not_compacted};
    DurableCompactionCopyStats stats{};
    std::optional<Error> error;

    [[nodiscard]] auto compacted() const noexcept -> bool {
        return outcome == DurableCompactionOutcome::compacted;
    }
};

/**
 * @brief Per-Worker bounded hot-cache admission, occupancy, and eviction observations.
 */
struct DurableHotCacheWorkerStats {
    WorkerId worker_id{};
    std::size_t resident_entries{};
    /// External resident key/value bytes. Fixed bucket arrays are reported
    /// separately and are never double-counted here.
    std::uint64_t resident_bytes{};
    std::size_t staged_entries{};
    std::uint64_t staged_bytes{};
    std::uint64_t bucket_bytes{};
    std::uint64_t total_accounted_bytes{};
    std::uint64_t byte_budget{};
    std::uint64_t staging_byte_budget{};
    std::size_t entry_budget{};
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t stale_hits{};
    std::uint64_t evictions{};
    std::uint64_t admission_bypasses{};
    std::uint64_t size_rejected{};
    std::uint64_t expired_gets{};
    /// hits * 10000 / (hits + misses); 0 when no lookups.
    std::uint64_t hit_rate_bp{};
    bool enabled{};
    std::uint64_t max_value_bytes{};
};

/// Low-overhead durable GET path telemetry. Totals are nanoseconds summed with
/// relaxed atomics outside the Worker critical section whenever possible; counters
/// under the lock stay plain Worker-local integers.
struct DurableGetPathWorkerStats {
    WorkerId worker_id{};
    std::uint64_t prepare_calls{};
    std::uint64_t complete_calls{};
    std::uint64_t mutex_wait_ns{};
    std::uint64_t prepare_hold_ns{};
    std::uint64_t complete_revalidate_hold_ns{};
    std::uint64_t index_lookup_ns{};
    std::uint64_t hot_cache_lookup_ns{};
    std::uint64_t generation_pin_lookup_ns{};
    std::uint64_t cold_read_ns{};
    std::uint64_t crc_value_copy_ns{};
    std::uint64_t relinearization_retries{};
    std::uint64_t hot_hits{};
    std::uint64_t hot_misses{};
    std::uint64_t hot_stale{};
    std::uint64_t hot_evictions{};
    std::uint64_t expired_ttl_gets{};
    std::size_t hot_resident_entries{};
    std::uint64_t hot_resident_bytes{};
};

/**
 * @brief Per-Worker durable grouping and synchronization observations.
 */
struct DurableBatchWorkerStats {
    WorkerId worker_id{};
    bool enabled{};
    std::size_t pending_records{};
    std::uint64_t pending_bytes{};
    std::size_t current_record_target{};
    std::uint64_t flush_attempts{};
    std::uint64_t committed_batches{};
    std::uint64_t failed_batches{};
    std::uint64_t committed_records{};
    std::uint64_t committed_bytes{};
    std::size_t maximum_batch_records{};
    std::uint64_t maximum_batch_bytes{};
    std::uint64_t total_commit_duration_ns{};
    std::uint64_t maximum_commit_duration_ns{};
    std::uint64_t record_limit_closes{};
    std::uint64_t byte_limit_closes{};
    std::uint64_t adaptive_target_closes{};
    std::uint64_t deadline_closes{};
};

/// Internal materialization of one recovered durable Store. It keeps the
/// directory lock for its complete lifetime. Mutable Segment handles remain
/// Worker-owned; immutable generation pins keep cold-read handles alive across
/// catalog publication and Segment retirement.
class DurableRuntimeCatalog final {
    struct PendingGroupMutation;
    struct RuntimeWorker;
    struct RuntimeSegmentGeneration;
    struct ExclusiveIndexQuiesce;

  public:
    /// Copyable ownership token for one immutable on-disk Segment generation.
    /// Read indexes retain one token per distinct generation instead of one
    /// shared_ptr and duplicate key per record.
    class PublishedReadPin final {
      public:
        PublishedReadPin(const PublishedReadPin&) = default;
        auto operator=(const PublishedReadPin&) -> PublishedReadPin& = default;
        PublishedReadPin(PublishedReadPin&&) noexcept = default;
        auto operator=(PublishedReadPin&&) noexcept -> PublishedReadPin& = default;

        [[nodiscard]] auto worker_index() const noexcept -> std::size_t {
            return worker_index_;
        }

        [[nodiscard]] auto matches(const RecordRef& reference) const noexcept -> bool;

        [[nodiscard]] auto same_generation(const PublishedReadPin& other) const noexcept -> bool {
            return generation_ == other.generation_;
        }

      private:
        PublishedReadPin(std::size_t worker_index,
                         std::shared_ptr<const RuntimeSegmentGeneration> generation) noexcept
            : worker_index_(worker_index), generation_(std::move(generation)) {}

        std::size_t worker_index_{};
        std::shared_ptr<const RuntimeSegmentGeneration> generation_;

        friend class DurableRuntimeCatalog;
    };

    /// Immutable logical record captured together with the exact on-disk
    /// Segment generation that owns its RecordRef. Copies retain the file pin;
    /// the reference is therefore never observable without lifetime ownership.
    class PublishedReadRecord final {
      public:
        PublishedReadRecord(const PublishedReadRecord&) = default;
        auto operator=(const PublishedReadRecord&) -> PublishedReadRecord& = default;
        PublishedReadRecord(PublishedReadRecord&&) noexcept = default;
        auto operator=(PublishedReadRecord&&) noexcept -> PublishedReadRecord& = default;

        [[nodiscard]] auto key() const noexcept -> std::string_view {
            return key_;
        }

        [[nodiscard]] auto key_hash() const noexcept -> std::uint64_t {
            return key_hash_;
        }

        [[nodiscard]] auto worker_index() const noexcept -> std::size_t {
            return pin_.worker_index();
        }

        [[nodiscard]] auto reference() const noexcept -> const RecordRef& {
            return reference_;
        }

        [[nodiscard]] auto pin() const noexcept -> const PublishedReadPin& {
            return pin_;
        }

        [[nodiscard]] static auto bind(std::string key, std::uint64_t key_hash, RecordRef reference,
                                       PublishedReadPin pin) -> PublishedReadRecord {
            return PublishedReadRecord{std::move(key), key_hash, reference, std::move(pin)};
        }

      private:
        PublishedReadRecord(std::string key, std::uint64_t key_hash, RecordRef reference,
                            PublishedReadPin pin)
            : key_(std::move(key)), key_hash_(key_hash), reference_(reference), pin_(std::move(pin)) {}

        std::string key_;
        std::uint64_t key_hash_{};
        RecordRef reference_;
        PublishedReadPin pin_;

        friend class DurableRuntimeCatalog;
    };

    /// Non-owning view into one immutable paired ReadGeneration. Its key and
    /// pin remain valid only while the Reader holds the corresponding epoch
    /// lease. This type never escapes the paired cold-read task protocol.
    class PublishedReadView final {
      public:
        [[nodiscard]] auto key() const noexcept -> std::string_view {
            return key_;
        }

        [[nodiscard]] auto key_hash() const noexcept -> std::uint64_t {
            return key_hash_;
        }

        [[nodiscard]] auto worker_index() const noexcept -> std::size_t {
            return pin_->worker_index();
        }

        [[nodiscard]] auto reference() const noexcept -> const RecordRef& {
            return reference_;
        }

        [[nodiscard]] auto pin() const noexcept -> const PublishedReadPin& {
            return *pin_;
        }

      private:
        /// Only immutable paired-generation builders may create a borrowed
        /// view. Callers can transport it but cannot manufacture a lifetime
        /// claim from an arbitrary pin reference.
        [[nodiscard]] static auto borrow(std::string_view key, std::uint64_t key_hash, RecordRef reference,
                                         const PublishedReadPin& pin) noexcept -> PublishedReadView {
            return PublishedReadView{key, key_hash, reference, &pin};
        }

        PublishedReadView(std::string_view key, std::uint64_t key_hash, RecordRef reference,
                          const PublishedReadPin* pin) noexcept
            : key_(key), key_hash_(key_hash), reference_(reference), pin_(pin) {}

        std::string_view key_;
        std::uint64_t key_hash_{};
        RecordRef reference_;
        const PublishedReadPin* pin_{};

        friend class DurableRuntimeCatalog;
        friend class store::paired::ImmutableReadIndex;
        friend class store::paired::PairReadGeneration;
    };

    static_assert(std::is_trivially_copyable_v<PublishedReadView>);
    static_assert(sizeof(PublishedReadView) <= 72);

    /**
     * @brief Owning pinned Records captured with one consistent catalog revision.
     */
    struct PublishedReadSnapshot final {
        std::vector<PublishedReadRecord> records;
        std::uint64_t catalog_revision{};
    };

    /**
     * @brief Move-only cold-read state owning its key and exact Segment generation pin across disk I/O.
     */
    class PinnedRead final {
      public:
        PinnedRead(PinnedRead&&) noexcept = default;
        auto operator=(PinnedRead&&) noexcept -> PinnedRead& = default;
        PinnedRead(const PinnedRead&) = delete;
        auto operator=(const PinnedRead&) -> PinnedRead& = delete;

      private:
        PinnedRead(std::string key, std::uint64_t key_hash, std::uint64_t now_ns, std::size_t worker_index,
                   RecordRef reference, std::shared_ptr<const RuntimeSegmentGeneration> generation,
                   bool generation_linearized = false)
            : key_(std::move(key)), key_hash_(key_hash), now_ns_(now_ns), worker_index_(worker_index),
              reference_(reference), generation_(std::move(generation)),
              generation_linearized_(generation_linearized) {}

        std::string key_;
        std::uint64_t key_hash_{};
        std::uint64_t now_ns_{};
        std::size_t worker_index_{};
        RecordRef reference_;
        std::shared_ptr<const RuntimeSegmentGeneration> generation_;
        /// True when an immutable Reader generation, rather than the mutable
        /// Worker Index, is the GET linearization authority. Such reads need no
        /// post-I/O Worker/catalog revalidation.
        bool generation_linearized_{};

        friend class DurableRuntimeCatalog;
        friend class detail::StoreAccess;
    };

    /**
     * @brief Move-only cold-read view borrowing key/generation; an external immutable-generation lease must
     * outlive its use.
     */
    class BorrowedPinnedRead final {
      public:
        BorrowedPinnedRead(BorrowedPinnedRead&&) noexcept = default;
        auto operator=(BorrowedPinnedRead&&) noexcept -> BorrowedPinnedRead& = default;
        BorrowedPinnedRead(const BorrowedPinnedRead&) = delete;
        auto operator=(const BorrowedPinnedRead&) -> BorrowedPinnedRead& = delete;

      private:
        BorrowedPinnedRead(std::string_view key, std::uint64_t key_hash, std::uint64_t now_ns,
                           std::size_t worker_index, RecordRef reference,
                           const RuntimeSegmentGeneration* generation) noexcept
            : key_(key), key_hash_(key_hash), now_ns_(now_ns), worker_index_(worker_index),
              reference_(reference), generation_(generation) {}

        std::string_view key_;
        std::uint64_t key_hash_{};
        std::uint64_t now_ns_{};
        std::size_t worker_index_{};
        RecordRef reference_;
        const RuntimeSegmentGeneration* generation_{};

        friend class DurableRuntimeCatalog;
        friend class detail::StoreAccess;
    };

    /**
     * @brief Prepared read alternatives: an owning hot value, an owning cold pin, or a cold view protected by
     * an external lease.
     */
    struct PreparedRead final {
        std::optional<OwnedValue> value{};
        std::optional<PinnedRead> cold{};
        std::optional<BorrowedPinnedRead> borrowed_cold{};
    };

    [[nodiscard]] static auto open_existing(const std::filesystem::path& path,
                                            std::uint64_t recovery_now_ns = 0, FilesystemHooks hooks = {})
        -> Result<std::unique_ptr<DurableRuntimeCatalog>>;
    [[nodiscard]] static auto open_locked(DataDirectory directory, std::uint64_t recovery_now_ns = 0,
                                          DurableRuntimeOptions options = {})
        -> Result<std::unique_ptr<DurableRuntimeCatalog>>;

    ~DurableRuntimeCatalog();
    DurableRuntimeCatalog(const DurableRuntimeCatalog&) = delete;
    auto operator=(const DurableRuntimeCatalog&) -> DurableRuntimeCatalog& = delete;
    DurableRuntimeCatalog(DurableRuntimeCatalog&&) = delete;
    auto operator=(DurableRuntimeCatalog&&) -> DurableRuntimeCatalog& = delete;

    [[nodiscard]] auto get(std::string_view key, std::uint64_t now_ns = 0) -> Result<OwnedValue>;
    [[nodiscard]] auto get(std::span<const std::byte> key, std::uint64_t now_ns = 0) -> Result<OwnedValue>;
    [[nodiscard]] auto get(const HashedKey& key, std::uint64_t now_ns = 0) -> Result<OwnedValue>;
    [[nodiscard]] auto put(std::span<const std::byte> key, std::span<const std::byte> value,
                           std::uint64_t expire_at_ns = 0, ValueType type = ValueType::bytes,
                           std::uint32_t flags = 0) -> DurableMutationResult;
    [[nodiscard]] auto put(const HashedKey& key, std::span<const std::byte> value,
                           std::uint64_t expire_at_ns = 0, ValueType type = ValueType::bytes,
                           std::uint32_t flags = 0) -> DurableMutationResult;
    [[nodiscard]] auto erase(std::span<const std::byte> key) -> DurableMutationResult;
    [[nodiscard]] auto erase(const HashedKey& key) -> DurableMutationResult;

    [[nodiscard]] auto healthy() const noexcept -> bool;
    [[nodiscard]] auto worker_count() const noexcept -> std::size_t;

    [[nodiscard]] auto worker_routing() const noexcept -> WorkerRoutingState {
        return worker_routing_;
    }

    [[nodiscard]] auto manifest() const -> Manifest;
    [[nodiscard]] auto allocate_blob_id() -> Result<BlobId>;
    [[nodiscard]] auto blob_roots() -> Result<std::vector<BlobId>>;
    [[nodiscard]] auto namespace_audit() const -> NamespaceAuditReport;
    [[nodiscard]] auto recovery_stats() const noexcept -> const DurableRecoveryStats&;
    [[nodiscard]] auto hot_cache_stats() const -> std::vector<DurableHotCacheWorkerStats>;
    [[nodiscard]] auto get_path_stats() const -> std::vector<DurableGetPathWorkerStats>;
    [[nodiscard]] auto batch_stats() const -> std::vector<DurableBatchWorkerStats>;
    [[nodiscard]] auto rotation_stats() const noexcept -> DurableRotationStats;
    [[nodiscard]] auto compaction_stats() const noexcept -> DurableCompactionMetrics;
    [[nodiscard]] auto next_sequence(std::size_t worker_index) const -> Result<SequenceNumber>;
    [[nodiscard]] auto active_segment(std::size_t worker_index) const -> Result<SegmentId>;
    [[nodiscard]] auto next_compaction_worker(std::size_t start_worker) const
        -> Result<std::optional<std::size_t>>;
    /// Catalog-level sealed/free snapshot and exact candidate live/dead byte counters.
    /// Optional unread TTL probe reads sealed source Records for the candidate only.
    [[nodiscard]] auto maintenance_observation(std::size_t start_worker = 0, std::uint64_t now_ns = 0,
                                               bool probe_unread_expired_ttl = false)
        -> Result<MaintenanceObservation>;
    [[nodiscard]] auto compact_worker(std::size_t worker_index, std::uint64_t now_ns,
                                      std::uint64_t max_copy_bytes = 0,
                                      std::uint64_t max_copy_bytes_per_second = 0) -> DurableCompactionResult;
    /// Lexicographically sorted live Index keys. Intended for offline tools (migrate).
    [[nodiscard]] auto snapshot_live_keys() -> Result<std::vector<std::string>>;
    [[nodiscard]] auto verify_index() -> Status;
    [[nodiscard]] auto flush() -> Status;
    /// Online catalog backup: caller must fence Store admissions. Copies Manifest + Segments into an
    /// empty destination while holding the catalog exclusive lock (writers already paused).
    /// Destination verify is performed by Store::backup_to after admissions resume.
    [[nodiscard]] auto backup_to(const std::filesystem::path& destination, bool scan_records = true,
                                 const DurableResourceLimits& limits = {})
        -> Result<DurableStoreBackupReport>;
    void request_close_flush();
    [[nodiscard]] auto close() -> Status;
    void mark_fail_closed() noexcept;

  private:
    DurableRuntimeCatalog(DataDirectory directory, DurableRecoveryState recovered,
                          DurableRuntimeOptions options);
    [[nodiscard]] auto initialize_generation_pins() -> Status;
    [[nodiscard]] auto flush_pending_batches(SegmentCommitSync sync) -> Status;
    [[nodiscard]] auto flush_due_batches(SegmentCommitSync sync) -> Status;
    [[nodiscard]] auto flush_dirty_segments() -> Status;
    [[nodiscard]] auto flush_worker_batch(RuntimeWorker& worker, std::unique_lock<std::mutex>& worker_lock,
                                          std::shared_lock<std::shared_mutex>& catalog_lock,
                                          SegmentCommitSync sync) -> Status;
    [[nodiscard]] auto sync_worker_file(RuntimeWorker& worker, std::unique_lock<std::mutex>& worker_lock,
                                        std::shared_lock<std::shared_mutex>& catalog_lock) -> Status;
    [[nodiscard]] auto should_flush_batch(RuntimeWorker& worker) const noexcept -> bool;
    void abandon_pending_batches() noexcept;
    void wait_for_batch_close(RuntimeWorker& worker, SequenceNumber sequence,
                              std::unique_lock<std::mutex>& lock);
    [[nodiscard]] auto commit_writer_batch(std::size_t worker_index) -> Status;
    [[nodiscard]] auto writer_batch_config() const noexcept -> std::optional<DurableGroupConfig>;
    /// Highest sequence whose index publication has been applied for this Writer
    /// (advanced in flush_worker_batch / non-strict Index paths). Used to distinguish
    /// flushed siblings from abandoned unflushed pending after a later-item sticky
    /// failure. Takes the Worker mutex (happens-before with Index publishers).
    [[nodiscard]] auto writer_durable_through(std::size_t worker_index) const noexcept -> SequenceNumber;
    [[nodiscard]] auto fail_closed(Error error) -> Unexpected;
    [[nodiscard]] auto mutate(std::span<const std::byte> key, std::span<const std::byte> value, Opcode opcode,
                              std::uint64_t key_hash, std::uint64_t expire_at_ns, ValueType type,
                              std::uint32_t flags, bool writer_batch = false) -> DurableMutationResult;
    [[nodiscard]] auto rotate_active(RuntimeWorker& worker, std::unique_lock<std::mutex>& worker_lock)
        -> DurableMutationResult;
    void record_rotation_final_commit(std::uint64_t duration_ns, bool committed) noexcept;
    [[nodiscard]] auto prepare_get(const HashedKey& key, std::uint64_t now_ns) -> Result<PreparedRead>;
    /// Writer-side publication helpers. Both capture methods may take runtime
    /// locks but never perform file I/O. The returned records are immutable and
    /// retain exact Segment-generation pins for lock-free Reader lookup.
    /// allow_fail_closed: one-shot drain after sticky failure so committed siblings
    /// can still enter the published generation when durable already marked itself
    /// unhealthy (pair Writer sync/async recovery publish only).
    [[nodiscard]] auto snapshot_published_reads(std::size_t worker_index, bool allow_fail_closed = false)
        -> Result<PublishedReadSnapshot>;
    [[nodiscard]] auto read_catalog_revision(std::size_t worker_index) const noexcept -> std::uint64_t;
    [[nodiscard]] static auto advance_read_catalog_revision(RuntimeWorker& worker) noexcept -> bool;
    [[nodiscard]] auto capture_published_read(std::size_t worker_index, const HashedKey& key)
        -> Result<PublishedReadRecord>;
    /// Immutable published GETs: pin/identity only. Servable after sticky fail-closed so
    /// drain-snapshot success-ACKs remain RAW via Store::get / reactor prepare_published.
    /// Mutable Index prepare_get still rejects when !healthy().
    [[nodiscard]] auto prepare_published_get(PublishedReadRecord read, std::uint64_t now_ns)
        -> Result<PreparedRead>;
    [[nodiscard]] auto prepare_published_get(PublishedReadView read, std::uint64_t now_ns)
        -> Result<PreparedRead>;
    [[nodiscard]] auto complete_get(PinnedRead read,
                                    const detail::ColdReadCancellation* cancellation = nullptr,
                                    std::vector<std::byte>* scratch = nullptr) -> Result<OwnedValue>;
    [[nodiscard]] auto complete_get(BorrowedPinnedRead read,
                                    const detail::ColdReadCancellation* cancellation = nullptr,
                                    std::vector<std::byte>* scratch = nullptr) -> Result<OwnedValue>;
    [[nodiscard]] auto complete_generation_get(std::string_view key, std::uint64_t key_hash,
                                               std::uint64_t now_ns, std::size_t worker_index,
                                               const RecordRef& reference,
                                               const RuntimeSegmentGeneration& generation,
                                               const detail::ColdReadCancellation* cancellation,
                                               std::vector<std::byte>* scratch) -> Result<OwnedValue>;
    /// Build SegmentId → catalog/pin slot mapping before a persistent publication.
    /// The prepared vector is installed with a non-allocating move after commit.
    [[nodiscard]] static auto prepare_pin_slot_index(const Manifest& manifest)
        -> Result<std::vector<std::uint32_t>>;
    [[nodiscard]] auto catalog_index_for_segment(SegmentId segment_id) const noexcept
        -> std::optional<std::size_t>;

    /// Declared first so cached Segment handles are destroyed before the lock
    /// and anchored directory descriptor.
    DataDirectory directory_;
    WorkerRoutingState worker_routing_{};
    Manifest manifest_;
    NamespaceAuditReport namespace_audit_;
    std::vector<RecoveredSegmentState> segments_;
    std::vector<std::shared_ptr<const RuntimeSegmentGeneration>> generation_pins_;
    /// Dense SegmentId → catalog index. Absent/retired ids store UINT32_MAX.
    /// Keeps GET pin resolution O(1) without changing RecordRef identity.
    std::vector<std::uint32_t> pin_slot_by_segment_id_;
    std::vector<std::unique_ptr<RuntimeWorker>> workers_;
    DurableRecoveryStats recovery_stats_;
    DurableRuntimeOptions options_;
    std::unique_ptr<DurableFlushCoordinator> flusher_;
    bool dedicated_commit_executor_{};
    std::atomic_bool healthy_{true};
    std::atomic_bool closed_{false};
    /// Even values expose a complete rotation telemetry publication; odd means
    /// the short atomic update block is in progress.
    mutable std::atomic_uint64_t rotation_stats_version_{};
    std::atomic_uint64_t rotation_attempts_{};
    std::atomic_uint64_t rotations_committed_{};
    std::atomic_uint64_t rotation_compaction_waits_{};
    std::atomic_uint64_t rotation_final_record_commit_attempts_{};
    std::atomic_uint64_t rotation_final_record_commits_{};
    std::atomic_uint64_t last_rotation_publication_wait_ns_{};
    std::atomic_uint64_t total_rotation_publication_wait_ns_{};
    std::atomic_uint64_t maximum_rotation_publication_wait_ns_{};
    std::atomic_uint64_t last_rotation_seal_ns_{};
    std::atomic_uint64_t total_rotation_seal_ns_{};
    std::atomic_uint64_t maximum_rotation_seal_ns_{};
    std::atomic_uint64_t last_rotation_create_ns_{};
    std::atomic_uint64_t total_rotation_create_ns_{};
    std::atomic_uint64_t maximum_rotation_create_ns_{};
    std::atomic_uint64_t last_rotation_manifest_publication_ns_{};
    std::atomic_uint64_t total_rotation_manifest_publication_ns_{};
    std::atomic_uint64_t maximum_rotation_manifest_publication_ns_{};
    std::atomic_uint64_t last_rotation_execution_ns_{};
    std::atomic_uint64_t total_rotation_execution_ns_{};
    std::atomic_uint64_t maximum_rotation_execution_ns_{};
    std::atomic_uint64_t last_rotation_total_ns_{};
    std::atomic_uint64_t total_rotation_ns_{};
    std::atomic_uint64_t maximum_rotation_total_ns_{};
    std::atomic_uint64_t compaction_attempts_{};
    std::atomic_uint64_t compactions_completed_{};
    std::atomic_uint64_t compaction_delta_overflows_{};
    std::atomic_uint64_t compaction_manifest_conflicts_{};
    std::atomic_uint64_t compaction_abandoned_copy_bytes_{};
    std::atomic_uint64_t compaction_replayed_records_{};
    std::atomic_uint64_t maximum_compaction_snapshot_ns_{};
    std::atomic_uint64_t maximum_compaction_commit_gate_ns_{};
    std::atomic_uint64_t last_rotation_final_record_commit_ns_{};
    std::atomic_uint64_t total_rotation_final_record_commit_ns_{};
    std::atomic_uint64_t maximum_rotation_final_record_commit_ns_{};
    std::mutex close_mutex_;
    std::optional<Error> close_error_;
    /// Guarded by manifest_publication_mutex_. A compaction lease excludes
    /// manifest-changing rotations without keeping the serializer locked while
    /// replacement Records are scanned and copied. Rotations wait for the lease
    /// and then rebuild from the newly published authority.
    bool compaction_publication_active_{};
    bool rotation_publication_active_{};
    std::mutex manifest_publication_mutex_;
    std::condition_variable manifest_publication_changed_;
    mutable std::shared_mutex catalog_mutex_;

    friend class detail::StoreAccess;
};

} // namespace glifistore
