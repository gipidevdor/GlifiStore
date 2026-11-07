/**
 * @file
 * @brief Store ownership, durability, resource bounds, clocks, and maintenance configuration.
 */
#pragma once

#include "glifistore/core/worker_routing.hpp"
#include "glifistore/persistence/filesystem_hooks.hpp"
#include "glifistore/store/blob.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace glifistore {

/**
 * @brief Shared absolute-time source for reads, expiration, and recovery; implementations must be thread-safe
 * and noexcept.
 */
class StoreClock {
  public:
    virtual ~StoreClock() = default;

    /// Implementations must be thread-safe and must not throw. Values are
    /// absolute nanoseconds since the Unix epoch; zero represents the epoch.
    [[nodiscard]] virtual auto now_ns() const noexcept -> std::uint64_t = 0;
};

inline constexpr std::size_t kMaximumWorkerCount = 256;
inline constexpr std::size_t kDefaultMinimumMemoryPerWorker = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kDefaultMaximumDurableStoreBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kDefaultReservedFreeBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kDefaultMaximumDurableSegments = 127;
inline constexpr std::size_t kDefaultMaximumManifestBytes = 1024ULL * 1024ULL;
inline constexpr std::size_t kDefaultMaximumDurableOpenFiles = 512;
inline constexpr std::uint64_t kDefaultMaximumRecoveryMemoryBytes = 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kDefaultMaximumLiveKeys = 10'000'000;
inline constexpr std::uint64_t kDefaultMaximumTemporaryCompactionBytes = 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kDefaultMaximumHotCacheBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kDefaultMaximumHotCacheBytesPerWorker = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kDefaultMaximumHotCacheStagingBytesPerWorker = 16ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kDefaultMaximumHotCacheEntriesPerWorker = 1'000'000;
inline constexpr std::uint64_t kDefaultMaximumHotCacheValueBytes = 64ULL * 1024ULL;
inline constexpr std::size_t kDefaultMaximumDeferredTtlReclaimsPerWorker = 1'024;
inline constexpr std::uint64_t kDefaultMaintenanceMaxCopyBytesPerCycle = 128ULL * 1024ULL * 1024ULL;

/**
 * @brief Admission bounds for durable bytes, descriptors, live keys, recovery, caching, and compaction.
 */
struct DurableResourceLimits {
    std::uint64_t max_store_bytes{
        kDefaultMaximumDurableStoreBytes}; ///< Maximum admitted durable Store bytes.
    std::uint64_t reserved_free_bytes{
        kDefaultReservedFreeBytes}; ///< Free filesystem bytes preserved by admission.
    std::size_t max_segment_count{kDefaultMaximumDurableSegments}; ///< Maximum catalog Segment count.
    std::size_t max_manifest_bytes{kDefaultMaximumManifestBytes}; ///< Maximum accepted/encoded Manifest size.
    std::size_t max_open_files{kDefaultMaximumDurableOpenFiles};  ///< Durable file-descriptor budget.
    std::uint64_t max_recovery_memory_bytes{
        kDefaultMaximumRecoveryMemoryBytes};            ///< Recovery working-memory policy budget.
    std::size_t max_live_keys{kDefaultMaximumLiveKeys}; ///< Maximum admitted live-key count.
    std::uint64_t max_temporary_compaction_bytes{
        kDefaultMaximumTemporaryCompactionBytes}; ///< Private replacement-output byte budget.
    std::uint32_t max_write_amplification{4};     ///< Maximum planned compaction write-amplification factor.
    /// Zero disables hot admission without affecting correctness. The global
    /// budget is partitioned across Workers, then capped by the per-Worker limit.
    /// hot_cache_enabled=false is an explicit kill switch equivalent to a zero
    /// byte budget for admission (cold pinned reads remain correct).
    bool hot_cache_enabled{true};
    std::uint64_t max_hot_cache_bytes{
        kDefaultMaximumHotCacheBytes}; ///< Global hot-cache byte budget, partitioned by Worker.
    std::uint64_t max_hot_cache_bytes_per_worker{
        kDefaultMaximumHotCacheBytesPerWorker}; ///< Additional cap on one Worker hot cache.
    std::uint64_t max_hot_cache_staging_bytes_per_worker{
        kDefaultMaximumHotCacheStagingBytesPerWorker}; ///< Per-Worker hot admission staging budget.
    std::size_t max_hot_cache_entries_per_worker{
        kDefaultMaximumHotCacheEntriesPerWorker}; ///< Per-Worker hot-cache entry limit.
    /// Values larger than this are never admitted; they always take the cold path.
    std::uint64_t max_hot_cache_value_bytes{kDefaultMaximumHotCacheValueBytes};
    /// Bounded Index/hot-cache TTL reclaim backlog drained by Worker maintenance
    /// paths (mutate, prepare_get). Zero forces synchronous reclaim on expire.
    std::size_t max_deferred_ttl_reclaims_per_worker{kDefaultMaximumDeferredTtlReclaimsPerWorker};
    /// Per-compaction metadata journal. Zero disables tolerance of concurrent
    /// Index changes. Exhaustion aborts compaction without rejecting mutations.
    std::size_t max_compaction_delta_entries{
        4096}; ///< Maximum concurrent-mutation journal entries per compaction.
    std::size_t max_compaction_delta_key_bytes{1024 * 1024}; ///< Maximum key bytes retained by that journal.

    auto operator==(const DurableResourceLimits&) const -> bool = default;
};

/**
 * @brief Writer group-commit limits in records, bytes, and milliseconds.
 */
struct DurableGroupConfig {
    std::uint32_t max_records{32};  ///< Maximum Records in one group.
    std::uint32_t max_bytes{65536}; ///< Maximum encoded bytes in one group.
    std::uint32_t max_wait_ms{10};  ///< Maximum group formation wait in milliseconds.
    std::uint32_t min_records{1};   ///< Minimum target group size before the wait bound closes it.
};

/**
 * @brief Acknowledgement/flush policy selected for this Store; wire and file format versions are independent.
 */
enum class StorageMode : std::uint8_t {
    volatile_memory,
    durable_sync,
    durable_periodic,
    durable_group,
};

/**
 * @brief Whether maintenance is driven cooperatively, by a background controller, or disabled.
 */
enum class MaintenanceMode : std::uint8_t {
    cooperative,
    background,
    disabled,
};

/**
 * @brief Bounded maintenance policy; pressure/emergency bypass explicitly documented normal-mode guards.
 */
struct MaintenanceConfig {
    MaintenanceMode mode{MaintenanceMode::cooperative};
    /// Preflight limit for one normal-mode evaluation/compaction. Zero explicitly
    /// disables this limit; pressure and emergency always bypass it.
    std::uint64_t max_copy_bytes_per_cycle{kDefaultMaintenanceMaxCopyBytesPerCycle};
    /// Private pre-intent replacement-write rate under normal pressure. The
    /// limiter spaces <=1 MiB physical writes with a bounded initial burst. The
    /// one-second controller window prevents adjacent jobs from each receiving
    /// a fresh burst. Zero disables pacing; pressure/emergency bypass it.
    std::uint64_t max_copy_bytes_per_sec{};
    /// Persistence v1 compacts one complete Worker sealed set in one atomic
    /// transaction. Kept as a source-compatible guard for existing aggregate
    /// initializers; the only supported value is 1.
    std::uint32_t max_segments_per_cycle{1};
    /// Inclusive CPU budget for compact work inside the same one-second window
    /// (milliseconds of wall time charged to compact). Zero disables; pressure
    /// and emergency bypass it.
    std::uint32_t max_cpu_ms_per_window{};
    std::uint32_t min_eval_interval_ms{1'000};  ///< Lower bound on controller evaluation spacing.
    std::uint32_t max_eval_interval_ms{60'000}; ///< Upper bound on controller evaluation/backoff spacing.
    /// Inclusive daemon foreground mutation p99 threshold for deferring normal
    /// compaction. Zero disables; pressure/emergency bypass. Embedded Stores do
    /// not produce samples unless an internal host reports them.
    std::uint32_t suspend_on_p99_latency_ms{};
    /// A latency decision requires this many completed foreground mutations in
    /// the consumed window. Low-traffic windows are reclaim opportunities.
    std::uint32_t suspend_on_p99_min_samples{32};
    /// Maximum continuous normal-mode deferral before one reclaim attempt is
    /// admitted despite the latency guard. Zero permits indefinite deferral
    /// until pressure; pressure/emergency always bypass independently.
    std::uint32_t max_latency_deferral_ms{30'000};
    /// Exact no-gain results are deterministic for an unchanged physical
    /// candidate. One attempt is therefore sufficient before bounded memoized
    /// backoff; zero disables that backoff.
    std::uint32_t max_no_gain_attempts{1};
    std::uint32_t dead_byte_ratio_bp_normal{
        5'000}; ///< Inclusive normal-mode dead-byte threshold in basis points.
    std::uint32_t segment_count_pressure_pct{80}; ///< Catalog occupancy percentage that triggers pressure.
    std::uint64_t free_bytes_pressure_margin{}; ///< Additional free-space margin used for pressure decisions.
    /// When true (default), pressure/emergency evaluations probe the candidate
    /// Worker's sealed Index for unread expired puts and populate observation
    /// counters.
    bool unread_ttl_pressure_probe{true};
    /// When false (default), normal scheduling stays conservative: unread expired
    /// sealed puts remain Index-live until GET, recovery, or pressure. When true,
    /// normal evaluations also probe and treat unread expired bytes as reclaimable
    /// dead space for the inclusive dead-byte threshold only (same Store::compact()
    /// path; copy budget still uses exact Index-referenced live bytes).
    bool unread_ttl_normal_scheduling{false};

    auto operator==(const MaintenanceConfig&) const -> bool = default;
};

/**
 * @brief Creation/open policy applied while holding the exclusive data-directory lock.
 */
enum class DurableOpenMode : std::uint8_t {
    open_or_create,
    open_existing,
    create_new,
};

/// Store linearization model (ADR 0032). paired is the product default: every
/// Worker/shard owns one serial mutation executor plus one immutable published
/// read generation, so ordinary get never acquires the Index mutex and concurrent
/// put/erase serialize on the Writer lane. Embedded synchronous calls may execute
/// through the shard combiner; the daemon enables the dedicated Writer thread.
///
/// legacy_mutex restores the historical Worker-mutex path. It is deprecated for
/// 0.1.x and removed in 0.2. The two models must never mutate the same Store
/// instance: mixing a legacy mutex mutator with a paired Writer is refused at
/// open, and any later detection fails the Store closed.
enum class StoreConcurrencyMode : std::uint8_t {
    paired,
    // Deprecated in 0.1.x; removed in 0.2. Prefer paired.
    legacy_mutex,
};

inline constexpr std::size_t kDefaultPairedMergeDeltaEntries = 8'192;
inline constexpr std::size_t kDefaultPairedMergeMaximumPostEntries = 32'736;
inline constexpr std::size_t kDefaultPairedMergeQuantumSlots = 4'096;
inline constexpr std::uint32_t kDefaultPairedWriterBatchMaxRecords = 32;
inline constexpr std::size_t kDefaultPairedWriterBatchMaxBytes = std::size_t{256} * 1024U;

/// Paired runtime tuning. Embedded callers only need the merge quanta: the
/// synchronous put/erase handoff borrows caller memory and needs no payload
/// arena. glifistored configures the asynchronous lane so its Reader can hand
/// off borrowed frame bytes without blocking.
/**
 * @brief Paired lane capacity, publication reclamation, and incremental merge tuning.
 */
struct PairedConcurrencyConfig {
    /// Zero disables the asynchronous lane entirely (embedded default): no queue
    /// cells and no payload arena are allocated for the shard.
    std::size_t async_lane_capacity{};
    /// Total per-shard preallocated asynchronous payload byte budget.
    std::size_t async_lane_payload_bytes{};
    /// Per-request payload ceiling for the asynchronous lane. Zero means the
    /// lane's whole payload budget.
    std::size_t async_maximum_payload_bytes{};
    /// Inclusive queue-wait budget for asynchronous submissions. Zero never
    /// expires queued work. Synchronous embedded mutations always wait.
    std::uint32_t async_queue_wait_ms{};
    /// Volatile Writer micro-batch bounds. Only work already present in the
    /// bounded lane is combined; the Writer never waits to manufacture a batch.
    std::uint32_t async_writer_batch_max_records{kDefaultPairedWriterBatchMaxRecords};
    std::size_t async_writer_batch_max_bytes{kDefaultPairedWriterBatchMaxBytes};
    std::size_t merge_delta_entries{
        kDefaultPairedMergeDeltaEntries}; ///< Delta size that initiates incremental base merging.
    std::size_t merge_maximum_post_entries{
        kDefaultPairedMergeMaximumPostEntries}; ///< Post-cut mutation capacity retained during a merge.
    std::size_t merge_quantum_slots{
        kDefaultPairedMergeQuantumSlots}; ///< Minimum slot-work quantum used to advance a merge.
    /// True when exactly one Reader thread per shard adopts generations with the
    /// epoch handshake (glifistored). Embedded readers use the counted read
    /// lease instead, so the Writer reclaims retired generations at quiescence.
    bool reader_epoch_lease{};
    /// ADR 0036 Wave 1: opt-in fixed generation slot pool. Default false keeps
    /// Alternative A shared_ptr publication bit-identical.
    bool generation_slot_pool{};

    auto operator==(const PairedConcurrencyConfig&) const -> bool = default;
};

/**
 * @brief Periodic sync interval and optional Writer batch bounds.
 */
struct DurablePeriodicConfig {
    std::uint32_t sync_interval_ms{
        1000}; ///< Periodic persistence interval in milliseconds; must be positive.
    std::optional<DurableGroupConfig> batch{
        DurableGroupConfig{.max_records = 4096, .max_bytes = 4U * 1024U * 1024U, .max_wait_ms = 1000}};
};

/**
 * @brief Startup Worker-count constraints; durable reopen retains persisted ownership.
 */
struct WorkerCountConfig {
    /// Requested fixed count; unset selects from topology on creation.
    std::optional<std::size_t> explicit_count{};
    std::size_t reserved_cores{1};                    ///< Cores reserved outside automatic Worker allocation.
    std::size_t maximum_workers{kMaximumWorkerCount}; ///< Upper bound applied by automatic selection.
    std::size_t minimum_memory_per_worker{
        kDefaultMinimumMemoryPerWorker}; ///< Per-Worker memory allowance used by topology policy.
};

/**
 * @brief Creation-time routing algorithm/seed with explicit compatibility intent.
 */
struct WorkerRoutingConfig {
    RoutingAlgorithm algorithm{
        RoutingAlgorithm::fnv1a64_v1};          ///< Versioned persisted owner-routing algorithm.
    std::uint64_t seed{kDefaultWorkerHashSeed}; ///< Owner-routing seed, distinct from Index placement seed.
    bool seed_explicit{}; ///< Whether reopen must validate the requested seed explicitly.

    [[nodiscard]] auto state() const noexcept -> WorkerRoutingState {
        return WorkerRoutingState{.algorithm = algorithm, .seed = seed};
    }
};

/**
 * @brief Open-time configuration; default construction selects a volatile paired Store.
 */
struct StoreConfig {
    WorkerCountConfig worker_config{};    ///< Creation/topology Worker-count policy.
    WorkerRoutingConfig worker_routing{}; ///< Creation routing and explicit reopen compatibility constraints.
    /// Paired Reader/Writer shards are the default. legacy_mutex is deprecated.
    StoreConcurrencyMode concurrency{StoreConcurrencyMode::paired};
    PairedConcurrencyConfig paired{};                       ///< Official paired runtime tuning.
    StorageMode storage_mode{StorageMode::volatile_memory}; ///< Acknowledgement and flush policy.
    /// Required durable directory; absent in volatile mode.
    std::optional<std::filesystem::path> data_directory{};
    DurableOpenMode durable_open_mode{
        DurableOpenMode::open_or_create};     ///< Durable namespace creation/open policy.
    DurablePeriodicConfig durable_periodic{}; ///< Periodic-mode sync/group tuning.
    DurableGroupConfig durable_group{};       ///< Group-mode Writer batch tuning.
    DurableResourceLimits durable_limits{};   ///< Durable admission and recovery budgets.
    /// ADR 0042: explicit persistence-v2 embedded blob profile; never upgrades v1.
    bool enable_blobs{};
    BlobResourceLimits blob_limits{}; ///< Blob profile file/byte/transfer budgets.
    MaintenanceConfig maintenance{};  ///< Maintenance scheduling and pressure policy.
    /// Deprecated compatibility field. Nonzero values are rejected; inject a
    /// StoreClock instead so reads and recovery observe one time source.
    std::uint64_t recovery_now_ns{};
    std::shared_ptr<const StoreClock> clock{}; ///< Shared clock; empty selects the implementation wall clock.
    /// Deterministic filesystem seam used by crash, fault-injection, and
    /// Reactor cold-I/O tests. Production configurations leave this empty.
    FilesystemHooks filesystem_hooks{};
    /// Optional close liveness bound for Writer drain and admission drain.
    /// nullopt waits unbounded (production default). A set value makes close
    /// return unavailable if drain has not finished when the deadline elapses.
    std::optional<std::uint32_t> close_drain_deadline_ms{};
};

} // namespace glifistore
