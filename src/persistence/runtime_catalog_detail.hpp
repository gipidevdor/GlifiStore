#pragma once

#include "glifistore/index/index.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "persistence/adaptive_batch_sizer.hpp"
#include "persistence/hot_record_table.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace glifistore::runtime_catalog_detail {

#if defined(NDEBUG) && !defined(GLIFISTORE_GET_PATH_TIMING)
inline constexpr bool kGetPathTimingEnabled = false;
#else
inline constexpr bool kGetPathTimingEnabled = true;
#endif

using detail::HotRecordEntry;
using detail::HotRecordSnapshot;
using detail::HotRecordTable;

[[nodiscard]] auto as_string_view(std::span<const std::byte> bytes) noexcept -> std::string_view;

[[nodiscard]] auto steady_duration_ns(std::chrono::steady_clock::time_point start,
                                      std::chrono::steady_clock::time_point end) noexcept -> std::uint64_t;

[[nodiscard]] auto steady_elapsed_ns(std::chrono::steady_clock::time_point start) noexcept -> std::uint64_t;

[[nodiscard]] auto timing_now() noexcept -> std::optional<std::chrono::steady_clock::time_point>;

[[nodiscard]] auto timing_elapsed_ns(std::optional<std::chrono::steady_clock::time_point> start) noexcept
    -> std::uint64_t;

[[nodiscard]] auto timing_duration_ns(std::optional<std::chrono::steady_clock::time_point> start,
                                      std::optional<std::chrono::steady_clock::time_point> end) noexcept
    -> std::uint64_t;

struct ReadContext {
    std::span<const std::byte> expected_key{};
    std::uint64_t expected_hash{};
    std::uint64_t now_ns{};
    OwnedValue value{};
    std::uint64_t crc_value_copy_ns{};
};

auto copy_verified_value(void* opaque, const RecordView& record) -> Status;

auto mutation_failure(DurableMutationOutcome outcome, Error error) -> DurableMutationResult;

void atomic_saturating_add(std::atomic_uint64_t& destination, std::uint64_t value) noexcept;

template <typename T> void atomic_observe_max(std::atomic<T>& destination, const T value) noexcept {
    auto current = destination.load(std::memory_order_relaxed);
    while (current < value && !destination.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

[[nodiscard]] auto begin_atomic_stats_publication(std::atomic_uint64_t& version) noexcept -> std::uint64_t;

void end_atomic_stats_publication(std::atomic_uint64_t& version, std::uint64_t previous) noexcept;

template <typename Callback> class ScopeExit final {
  public:
    explicit ScopeExit(Callback callback) noexcept : callback_(std::move(callback)) {}

    ~ScopeExit() noexcept {
        callback_();
    }

    ScopeExit(const ScopeExit&) = delete;
    auto operator=(const ScopeExit&) -> ScopeExit& = delete;

  private:
    Callback callback_;
};

template <typename Callback> ScopeExit(Callback) -> ScopeExit<Callback>;

// Staged hot-cache admission held outside the resident flat table until Index
// publication. RAII releases staged byte/entry charges if publication aborts.
class PreparedHotRecord final {
  public:
    PreparedHotRecord() = default;

    PreparedHotRecord(std::string key, const std::uint64_t key_hash, HotRecordEntry entry,
                      std::uint64_t* staged_bytes, std::size_t* staged_entries,
                      const std::uint64_t staged_charge, const std::uint64_t resident_charge) noexcept
        : key_(std::move(key)), key_hash_(key_hash), entry_(std::move(entry)), staged_bytes_(staged_bytes),
          staged_entries_(staged_entries), staged_charge_(staged_charge), resident_charge_(resident_charge),
          active_(true) {}

    ~PreparedHotRecord() {
        release_stage();
    }

    PreparedHotRecord(PreparedHotRecord&& other) noexcept
        : key_(std::move(other.key_)), key_hash_(std::exchange(other.key_hash_, 0)),
          entry_(std::move(other.entry_)), staged_bytes_(std::exchange(other.staged_bytes_, nullptr)),
          staged_entries_(std::exchange(other.staged_entries_, nullptr)),
          staged_charge_(std::exchange(other.staged_charge_, 0)),
          resident_charge_(std::exchange(other.resident_charge_, 0)),
          active_(std::exchange(other.active_, false)) {}

    auto operator=(PreparedHotRecord&& other) noexcept -> PreparedHotRecord& {
        if (this != &other) {
            release_stage();
            key_ = std::move(other.key_);
            key_hash_ = std::exchange(other.key_hash_, 0);
            entry_ = std::move(other.entry_);
            staged_bytes_ = std::exchange(other.staged_bytes_, nullptr);
            staged_entries_ = std::exchange(other.staged_entries_, nullptr);
            staged_charge_ = std::exchange(other.staged_charge_, 0);
            resident_charge_ = std::exchange(other.resident_charge_, 0);
            active_ = std::exchange(other.active_, false);
        }
        return *this;
    }

    PreparedHotRecord(const PreparedHotRecord&) = delete;
    auto operator=(const PreparedHotRecord&) -> PreparedHotRecord& = delete;

    [[nodiscard]] auto empty() const noexcept -> bool {
        return !active_;
    }

    [[nodiscard]] auto key() const -> const std::string& {
        return key_;
    }

    [[nodiscard]] auto take_key() noexcept -> std::string {
        return std::move(key_);
    }

    [[nodiscard]] auto key_hash() const noexcept -> std::uint64_t {
        return key_hash_;
    }

    [[nodiscard]] auto mapped() -> HotRecordEntry& {
        return entry_;
    }

    [[nodiscard]] auto resident_charge() const noexcept -> std::uint64_t {
        return resident_charge_;
    }

    [[nodiscard]] auto take_entry() -> HotRecordEntry {
        release_stage();
        active_ = false;
        return std::move(entry_);
    }

  private:
    void release_stage() noexcept {
        if (staged_bytes_ == nullptr || staged_entries_ == nullptr) {
            return;
        }
        *staged_bytes_ -= staged_charge_;
        --*staged_entries_;
        staged_bytes_ = nullptr;
        staged_entries_ = nullptr;
        staged_charge_ = 0;
    }

    std::string key_;
    std::uint64_t key_hash_{};
    HotRecordEntry entry_{};
    std::uint64_t* staged_bytes_{};
    std::size_t* staged_entries_{};
    std::uint64_t staged_charge_{};
    std::uint64_t resident_charge_{};
    bool active_{};
};

[[nodiscard]] auto hot_cache_table_bytes(std::size_t capacity) noexcept -> std::uint64_t;

[[nodiscard]] auto hot_record_accounted_bytes(std::size_t key_bytes, std::size_t value_bytes)
    -> Result<std::uint64_t>;

void subtract_hot_record_accounting(std::uint64_t& total, std::string_view key,
                                    const HotRecordEntry& entry) noexcept;

[[nodiscard]] auto hot_cache_worker_budget(std::size_t worker_index, std::size_t worker_count,
                                           const DurableResourceLimits& limits) noexcept -> std::uint64_t;

[[nodiscard]] auto hot_record_matches(const HotRecordEntry& entry, const RecordRef& reference) noexcept
    -> bool;

[[nodiscard]] auto owned_value_from_hot(const HotRecordSnapshot& entry) -> OwnedValue;

auto rotation_manifest(const Manifest& current, const ManifestSegmentEntry& old_active,
                       const DurableResourceLimits& limits) -> Result<Manifest>;

auto complete_interrupted_rotation(DataDirectory& directory, const DurableResourceLimits& limits) -> Status;

auto resolve_interrupted_compaction(DataDirectory& directory, std::uint64_t recovery_now_ns,
                                    const DurableResourceLimits& limits)
    -> Result<std::optional<DurableRecoveryState>>;

auto rollback_prepared_compaction(DataDirectory& directory, const Manifest& old_manifest,
                                  std::span<const ManifestSegmentEntry> replacements,
                                  const DurableResourceLimits& limits) -> Result<NamespaceAuditReport>;

} // namespace glifistore::runtime_catalog_detail

namespace glifistore {

// Private DurableRuntimeCatalog nested types shared across runtime_catalog*.cpp TUs.
using runtime_catalog_detail::HotRecordEntry;
using runtime_catalog_detail::HotRecordTable;
using runtime_catalog_detail::PreparedHotRecord;

struct DurableRuntimeCatalog::PendingGroupMutation {
    std::string key;
    PreparedHotRecord hot_record;
    RecordRef reference{};
    Opcode opcode{Opcode::put};
    std::uint64_t key_hash{};
    std::uint64_t expire_at_ns{};
};

// One immutable handle for one exact on-disk Segment generation. A GET may
// carry a RecordRef outside the Worker mutex only as part of a shared ownership
// of this object. POSIX unlink then retires the catalog name without
// invalidating an already linearized reader's descriptor.
struct DurableRuntimeCatalog::RuntimeSegmentGeneration {
    SegmentHeaderIdentity identity;
    SelectedSegmentCommit selected;
    DurableSegmentFile file;
};

struct DeferredTtlReclaim {
    std::string key;
    std::uint64_t key_hash{};
    RecordRef reference{};
};

struct DurableRuntimeCatalog::RuntimeWorker {
    struct alignas(64) BatchMetrics final {
        std::atomic_size_t pending_records{};
        std::atomic_uint64_t pending_bytes{};
        std::atomic_size_t current_record_target{1};
        std::atomic_uint64_t flush_attempts{};
        std::atomic_uint64_t committed_batches{};
        std::atomic_uint64_t failed_batches{};
        std::atomic_uint64_t committed_records{};
        std::atomic_uint64_t committed_bytes{};
        std::atomic_size_t maximum_batch_records{};
        std::atomic_uint64_t maximum_batch_bytes{};
        std::atomic_uint64_t total_commit_duration_ns{};
        std::atomic_uint64_t maximum_commit_duration_ns{};
        std::atomic_uint64_t record_limit_closes{};
        std::atomic_uint64_t byte_limit_closes{};
        std::atomic_uint64_t adaptive_target_closes{};
        std::atomic_uint64_t deadline_closes{};
    };

    // GET path timing + bookkeeping. Timing atomics are only updated when
    // kGetPathTimingEnabled (Debug / explicit define); counters always use
    // relaxed atomics published outside or at the edge of the critical section.
    struct alignas(64) GetPathMetrics final {
        std::atomic_uint64_t prepare_calls{};
        std::atomic_uint64_t complete_calls{};
        std::atomic_uint64_t mutex_wait_ns{};
        std::atomic_uint64_t prepare_hold_ns{};
        std::atomic_uint64_t complete_revalidate_hold_ns{};
        std::atomic_uint64_t index_lookup_ns{};
        std::atomic_uint64_t hot_cache_lookup_ns{};
        std::atomic_uint64_t generation_pin_lookup_ns{};
        std::atomic_uint64_t cold_read_ns{};
        std::atomic_uint64_t crc_value_copy_ns{};
        std::atomic_uint64_t relinearization_retries{};
        std::atomic_uint64_t hot_hits{};
        std::atomic_uint64_t hot_misses{};
        std::atomic_uint64_t hot_stale_hits{};
        std::atomic_uint64_t hot_evictions{};
        std::atomic_uint64_t admission_bypasses{};
        std::atomic_uint64_t size_rejected{};
        std::atomic_uint64_t expired_ttl_gets{};
    };

    explicit RuntimeWorker(RecoveredWorkerState recovered)
        : worker_id(recovered.worker_id), index(std::move(recovered.index)),
          next_sequence(recovered.next_sequence),
          durable_through(
              SequenceNumber{recovered.next_sequence.value == 0 ? 0 : recovered.next_sequence.value - 1U}),
          active_segment(recovered.active_segment),
          active_live_record_bytes(recovered.active_live_record_bytes),
          sealed_live_record_bytes(recovered.sealed_live_record_bytes) {}

    [[nodiscard]] auto update_live_record_bytes(const std::optional<RecordRef>& previous,
                                                const std::optional<RecordRef>& current) -> Status {
        auto active = active_live_record_bytes.load(std::memory_order_relaxed);
        auto sealed = sealed_live_record_bytes.load(std::memory_order_relaxed);
        if (previous) {
            auto& bytes = previous->segment_id == active_segment ? active : sealed;
            if (bytes < previous->size.value) {
                return fail(ErrorCode::corrupted_data, "durable Worker live Record byte counter underflow");
            }
            bytes -= previous->size.value;
        }
        if (current) {
            auto& bytes = current->segment_id == active_segment ? active : sealed;
            if (current->size.value > std::numeric_limits<std::uint64_t>::max() - bytes) {
                return fail(ErrorCode::arithmetic_overflow,
                            "durable Worker live Record byte counter overflow");
            }
            bytes += current->size.value;
        }
        active_live_record_bytes.store(active, std::memory_order_release);
        sealed_live_record_bytes.store(sealed, std::memory_order_release);
        return {};
    }

    WorkerId worker_id;
    Index index;
    SequenceNumber next_sequence;
    SequenceNumber durable_through;
    SegmentId active_segment;
    std::atomic_uint64_t active_live_record_bytes{};
    std::atomic_uint64_t sealed_live_record_bytes{};
    // Per-shard generation-pin authority. An unrelated shard publication must
    // not force this shard to rebuild its immutable Reader index.
    std::atomic_uint64_t read_catalog_revision{1};
    mutable std::mutex mutex;
    std::optional<DurableSegmentFile> cached_file;
    bool cached_writable{};
    std::vector<std::byte> encode_scratch;
    HotRecordTable hot_records;
    std::uint64_t hot_record_resident_bytes{};
    std::uint64_t hot_record_staged_bytes{};
    std::size_t hot_record_staged_entries{};
    std::vector<DeferredTtlReclaim> deferred_ttl_reclaims;
    std::uint64_t deferred_ttl_enqueued{};
    std::uint64_t deferred_ttl_applied{};
    std::uint64_t deferred_ttl_skipped{};
    std::vector<PendingGroupMutation> pending_group_mutations;
    std::size_t pending_group_insertions{};
    std::size_t pending_group_heap_key_bytes{};
    std::chrono::steady_clock::time_point batch_started{};
    std::atomic_size_t active_group_mutations{};
    detail::AdaptiveBatchSizer batch_sizer;
    BatchMetrics batch_metrics;
    GetPathMetrics get_path_metrics;
    bool batch_closing{};
    std::condition_variable batch_closed;
    // Exactly one mutation/rotation may own the writable Segment handle outside
    // `mutex`. Readers never wait on this gate: they use immutable generation
    // pins and relinearize against the Index. Writers, flush, and compaction do.
    bool mutation_io_active{};
    std::condition_variable mutation_io_finished;
    // Set while compaction, unread-TTL probe, or exclusive Index readers
    // (prepare_get / snapshot_live_keys) hold logical Index ownership. Under
    // exclusive_writer the mutate hot path observes this atomically without the mutex.
    // Holders nest (GET + Phase A/C): only the last disarm clears the bool.
    std::atomic_bool compaction_commit_active{};
    std::atomic_uint32_t compaction_commit_holders{};
    std::condition_variable compaction_commit_finished;

    void arm_index_quiesce_gate() noexcept {
        if (compaction_commit_holders.fetch_add(1U, std::memory_order_acq_rel) == 0U) {
            compaction_commit_active.store(true, std::memory_order_release);
        }
    }

    void disarm_index_quiesce_gate() noexcept {
        if (compaction_commit_holders.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
            compaction_commit_active.store(false, std::memory_order_release);
            compaction_commit_finished.notify_all();
        }
    }

    // Nested exclusive-Writer hot-path depth. Compaction / exclusive Index readers
    // wait for zero before touching the Index when exclusive_writer elides the
    // Worker mutex (`exclusive_writer &&` no background flusher). Flusher paths
    // never bump this.
    std::atomic_uint32_t hot_path_depth{};

    [[nodiscard]] auto hot_cache_total_bytes() const noexcept -> std::uint64_t;
    void erase_hot_record(std::string_view key, std::uint64_t key_hash) noexcept;

    void erase_hot_record(const HashedKey& key) noexcept {
        erase_hot_record(key.key, key.hash);
    }

    // Drain up to `limit` deferred TTL reclamations. Verifies exact RecordRef
    // before erase so a reinsert/update is never deleted. Caller holds mutex.
    [[nodiscard]] auto drain_deferred_ttl(std::size_t limit) -> Status;
    // Queue Index reclaim for an expired GET. May drain a bounded prefix when
    // the backlog is full. Caller holds mutex. Zero backlog limit reclaim sync.
    [[nodiscard]] auto defer_or_reclaim_expired(const HashedKey& key, const RecordRef& reference,
                                                std::size_t backlog_limit) -> Status;
    [[nodiscard]] auto prepare_hot_record(std::size_t worker_index, std::size_t worker_count,
                                          const DurableResourceLimits& limits, std::string_view key,
                                          std::uint64_t key_hash, std::span<const std::byte> value,
                                          std::uint64_t expire_at_ns, std::uint64_t publication_staging_bytes)
        -> Result<PreparedHotRecord>;
    // Caller holds the Worker mutex after Index publication. `reference` is
    // the sole sequence/Segment identity stored in the resident cache entry.
    [[nodiscard]] auto publish_hot_record(PreparedHotRecord& prepared, const RecordRef& reference) -> Status;
};

// Arms compaction_commit_active and drains hot_path_depth before Index touch on the
// mutex-elided exclusive Writer path. Mutex alone does not serialize with mutate.
// Used by prepare_get, snapshot_live_keys, snapshot_published_reads,
// capture_published_read, compaction Phase A, unread TTL probe, and
// flush_dirty_segments (exclusive durable_sync, no flusher).
struct DurableRuntimeCatalog::ExclusiveIndexQuiesce final {
    RuntimeWorker& worker;
    bool armed{};

    explicit ExclusiveIndexQuiesce(RuntimeWorker& owner, const bool enable) noexcept : worker(owner) {
        if (!enable) {
            return;
        }
        worker.arm_index_quiesce_gate();
        armed = true;
        for (;;) {
            const auto depth = worker.hot_path_depth.load(std::memory_order_acquire);
            if (depth == 0) {
                break;
            }
            worker.hot_path_depth.wait(depth, std::memory_order_acquire);
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

} // namespace glifistore
