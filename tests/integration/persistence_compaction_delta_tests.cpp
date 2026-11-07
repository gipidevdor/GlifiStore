#include "glifistore/store/store.hpp"
#include "persistence/compaction_delta.hpp"
#include "persistence_recovery_test_support.hpp"

#include <array>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

void seed_delta_store(const std::filesystem::path& path) {
    const auto store_id = recovery_store_id();
    std::vector<glifistore::ManifestSegmentEntry> entries;
    for (std::uint64_t id = 1; id <= 3; ++id) {
        entries.push_back({.segment_id = glifistore::SegmentId{id},
                           .generation = glifistore::GenerationId{1},
                           .owner_worker = glifistore::WorkerId{0},
                           .role = id == 3 ? glifistore::ManifestSegmentRole::active
                                           : glifistore::ManifestSegmentRole::sealed});
    }
    auto directory = glifistore::DataDirectory::open_and_lock(path);
    GLIFI_REQUIRE(directory.has_value());
    auto first = create_segment(*directory, store_id, entries[0]);
    append_record(first, 1, "changing", "old");
    append_record(first, 2, "erased", "old");
    append_record(first, 3, "expiry", "old", glifistore::Opcode::put, 100);
    append_record(first, 4, "restored", "old");
    GLIFI_REQUIRE(first.seal().committed());
    auto second = create_segment(*directory, store_id, entries[1]);
    append_record(second, 5, "stable", "stable");
    GLIFI_REQUIRE(second.seal().committed());
    auto active = create_segment(*directory, store_id, entries[2]);
    append_record(active, 6, "active", "old");
    GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, entries)).durable());
}

auto delta_put(glifistore::DurableRuntimeCatalog& runtime, const std::string_view key,
               const std::string_view value) -> glifistore::DurableMutationResult {
    return runtime.put(std::as_bytes(std::span{key}), std::as_bytes(std::span{value}));
}

auto delta_erase(glifistore::DurableRuntimeCatalog& runtime, const std::string_view key)
    -> glifistore::DurableMutationResult {
    return runtime.erase(std::as_bytes(std::span{key}));
}

void require_delta_value(glifistore::DurableRuntimeCatalog& runtime, const std::string_view key,
                         const std::string_view expected) {
    const auto value = runtime.get(key);
    GLIFI_REQUIRE(value.has_value());
    GLIFI_REQUIRE(owned_text(*value) == expected);
}

void require_missing(glifistore::DurableRuntimeCatalog& runtime, const std::string_view key) {
    const auto value = runtime.get(key, 99);
    GLIFI_REQUIRE(!value.has_value());
    GLIFI_REQUIRE(value.error().code == glifistore::ErrorCode::not_found);
}

// Inject acknowledged mutations after output staging but before intent write.
// The hook runs without Worker/catalog locks; active appends do not rotate.
struct DeltaCheckpoint {
    glifistore::DurableRuntimeCatalog* runtime{};
    glifistore::FilesystemOperation target{};
    bool injected{};
    bool fired{};
    bool crash{};

    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& self = *static_cast<DeltaCheckpoint*>(opaque);
        if (!self.injected && operation == glifistore::FilesystemOperation::write_compaction_intent) {
            self.injected = true;
            if (!delta_put(*self.runtime, "changing", "acknowledged").committed() ||
                !delta_erase(*self.runtime, "erased").committed()) {
                return glifistore::fail(glifistore::ErrorCode::io_error, "delta fixture mutation failed");
            }
        }
        if (!self.crash && self.injected && !self.fired && operation == self.target) {
            self.fired = true;
            return glifistore::fail(glifistore::ErrorCode::io_error, "delta checkpoint fault");
        }
        return {};
    }

    static void after(void* opaque, const glifistore::FilesystemOperation operation) {
        auto& self = *static_cast<DeltaCheckpoint*>(opaque);
        if (self.crash && self.injected && operation == self.target) {
            static_cast<void>(::kill(::getpid(), SIGKILL));
        }
    }
};

} // namespace

GLIFI_TEST("compaction delta preserves overwrite erase restore active and TTL across durable modes") {
    for (unsigned mode = 0; mode != 4; ++mode) {
        RecoveryTemporaryDirectory temporary;
        seed_delta_store(temporary.path());
        BlockingRecordRead blocker;
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(),
            {.file_io = {.context = &blocker, .read_some_at = &BlockingRecordRead::read_some_at}});
        GLIFI_REQUIRE(directory.has_value());
        glifistore::DurableRuntimeOptions options;
        options.exclusive_writer = mode == 1;
        options.limits.max_deferred_ttl_reclaims_per_worker = 0;
        if (mode == 2) {
            options.batch = glifistore::DurableGroupConfig{.max_records = 1};
            options.strict_ack = true;
        }
        if (mode == 3) {
            options.commit_sync = glifistore::SegmentCommitSync::deferred;
            options.sync_interval_ms = 10;
        }
        auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), 0, options);
        GLIFI_REQUIRE(runtime.has_value());
        blocker.arm();
        glifistore::DurableCompactionResult compacted;
        std::thread compactor{[&] { compacted = (*runtime)->compact_worker(0, 0); }};
        const bool blocked = blocker.wait_until_blocked();
        bool mutations_ok = blocked;
        std::string fresh_key(512, 'f');
        fresh_key[128] = '\0';
        if (blocked) {
            mutations_ok = delta_put(**runtime, "changing", "new").committed() &&
                           delta_erase(**runtime, "erased").committed() &&
                           delta_erase(**runtime, "restored").committed() &&
                           delta_put(**runtime, "restored", "restored").committed() &&
                           delta_put(**runtime, fresh_key, "fresh").committed() &&
                           delta_put(**runtime, "active", "active").committed();
            const auto expired = (*runtime)->get("expiry", 100);
            mutations_ok =
                mutations_ok && !expired && expired.error().code == glifistore::ErrorCode::not_found;
        }
        blocker.release();
        compactor.join();
        GLIFI_REQUIRE(mutations_ok);
        GLIFI_REQUIRE(compacted.compacted());
        GLIFI_REQUIRE(compacted.stats.delta_records == 7);
        GLIFI_REQUIRE(compacted.stats.snapshot_duration_ns > 0);
        GLIFI_REQUIRE(compacted.stats.commit_gate_duration_ns > 0);
        GLIFI_REQUIRE(compacted.stats.delta_replay_duration_ns > 0);
        require_delta_value(**runtime, "changing", "new");
        require_delta_value(**runtime, "stable", "stable");
        require_delta_value(**runtime, "restored", "restored");
        require_delta_value(**runtime, fresh_key, "fresh");
        require_delta_value(**runtime, "active", "active");
        require_missing(**runtime, "erased");
        require_missing(**runtime, "expiry");
        GLIFI_REQUIRE((*runtime)->verify_index().has_value());
        const auto metrics = (*runtime)->compaction_stats();
        GLIFI_REQUIRE(metrics.attempts == 1 && metrics.completed == 1 && metrics.replayed_records == 7);
        GLIFI_REQUIRE((*runtime)->close().has_value());
        runtime->reset();
        auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), 100);
        GLIFI_REQUIRE(reopened.has_value());
        require_delta_value(**reopened, "changing", "new");
        require_delta_value(**reopened, "restored", "restored");
        require_delta_value(**reopened, fresh_key, "fresh");
        require_delta_value(**reopened, "active", "active");
        require_missing(**reopened, "erased");
        require_missing(**reopened, "expiry");
        GLIFI_REQUIRE((*reopened)->verify_index().has_value());
    }
}

GLIFI_TEST("compaction delta replay uses reserved tables and key arena across repeated erase reinsert") {
    glifistore::Index index;
    glifistore::runtime_catalog_detail::CompactionDelta delta{200, 256 * 1024};
    std::string binary_key(1024, 'x');
    binary_key[128] = '\0';
    const auto key = glifistore::HashedKey::compute(binary_key);
    const glifistore::RecordRef original{.segment_id = glifistore::SegmentId{1},
                                         .offset = glifistore::RecordOffset{4096},
                                         .size = glifistore::RecordSize{2048},
                                         .sequence = glifistore::SequenceNumber{1},
                                         .generation = glifistore::GenerationId{2}};
    GLIFI_REQUIRE(index.insert_or_assign(key, original).has_value());
    auto replacement = original;
    replacement.segment_id = glifistore::SegmentId{3};
    for (std::uint64_t sequence = 2; sequence < 102; ++sequence) {
        delta.record(key, std::nullopt);
        replacement.sequence = glifistore::SequenceNumber{sequence};
        delta.record(key, replacement);
    }
    GLIFI_REQUIRE(!delta.overflowed());
    GLIFI_REQUIRE(delta.prepare(index).has_value());
    const auto reserved = index.stats();
    std::uint64_t active_bytes{};
    std::uint64_t sealed_bytes{original.size.value};
    GLIFI_REQUIRE(delta.replay(index, replacement.segment_id, active_bytes, sealed_bytes).has_value());
    const auto replayed = index.stats();
    GLIFI_REQUIRE(replayed.table_allocated_bytes == reserved.table_allocated_bytes);
    GLIFI_REQUIRE(replayed.arena_live_bytes == binary_key.size());
    GLIFI_REQUIRE(replayed.arena_allocated_bytes <= reserved.arena_allocated_bytes + delta.key_bytes());
    GLIFI_REQUIRE(index.find(key) == replacement);
    GLIFI_REQUIRE(active_bytes == replacement.size.value && sealed_bytes == 0);
}

GLIFI_TEST("compaction delta publishes concurrent acknowledged writes through paired Store generations") {
    RecoveryTemporaryDirectory temporary;
    seed_delta_store(temporary.path());
    BlockingRecordRead blocker;
    glifistore::StoreConfig config;
    config.worker_config.explicit_count = 1;
    config.storage_mode = glifistore::StorageMode::durable_sync;
    config.data_directory = temporary.path();
    config.durable_open_mode = glifistore::DurableOpenMode::open_existing;
    config.filesystem_hooks.file_io = {.context = &blocker,
                                       .read_some_at = &BlockingRecordRead::read_some_at};
    auto store = glifistore::Store::open(config);
    GLIFI_REQUIRE(store.has_value());
    blocker.arm();
    std::optional<glifistore::Result<glifistore::CompactionResult>> compacted;
    std::thread compactor{[&] { compacted.emplace((*store)->compact()); }};
    const bool blocked = blocker.wait_until_blocked();
    const std::string_view value{"paired-new"};
    bool committed = blocked;
    if (blocked) {
        committed = (*store)->put("changing", std::as_bytes(std::span{value})).has_value() &&
                    (*store)->erase("erased").has_value();
    }
    blocker.release();
    compactor.join();
    GLIFI_REQUIRE(committed);
    GLIFI_REQUIRE(compacted && *compacted && (**compacted).compacted);
    GLIFI_REQUIRE((**compacted).delta_records == 2);
    const auto changed = (*store)->get("changing");
    GLIFI_REQUIRE(changed.has_value() && owned_text(*changed) == value);
    const auto erased = (*store)->get("erased");
    GLIFI_REQUIRE(!erased && erased.error().code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE((*store)->verify_index().has_value());
    GLIFI_REQUIRE((*store)->maintenance_snapshot().compaction.completed == 1);
}

GLIFI_TEST("compaction delta budgets abort maintenance while foreground commits and retry succeeds") {
    for (unsigned budget = 0; budget != 3; ++budget) {
        RecoveryTemporaryDirectory temporary;
        seed_delta_store(temporary.path());
        BlockingRecordRead blocker;
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(),
            {.file_io = {.context = &blocker, .read_some_at = &BlockingRecordRead::read_some_at}});
        GLIFI_REQUIRE(directory.has_value());
        glifistore::DurableRuntimeOptions options;
        options.limits.max_compaction_delta_entries = budget == 0 ? 2 : budget == 1 ? 8 : 0;
        options.limits.max_compaction_delta_key_bytes = budget == 1 ? 1 : 1024;
        auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), 0, options);
        GLIFI_REQUIRE(runtime.has_value());
        blocker.arm();
        glifistore::DurableCompactionResult compacted;
        std::thread compactor{[&] { compacted = (*runtime)->compact_worker(0, 0); }};
        const bool blocked = blocker.wait_until_blocked();
        bool committed = blocked;
        if (blocked) {
            for (unsigned index = 0; index != 3; ++index) {
                committed = delta_put(**runtime, "changing", "new").committed() && committed;
            }
        }
        blocker.release();
        compactor.join();
        GLIFI_REQUIRE(committed);
        GLIFI_REQUIRE(!compacted.compacted());
        GLIFI_REQUIRE(compacted.error && compacted.error->code == glifistore::ErrorCode::sequence_conflict);
        GLIFI_REQUIRE(compacted.stats.delta_overflow_aborts == 1);
        GLIFI_REQUIRE(compacted.stats.abandoned_copy_bytes == compacted.stats.bytes_copied);
        GLIFI_REQUIRE(compacted.stats.bytes_copied > 0);
        GLIFI_REQUIRE((*runtime)->healthy());
        GLIFI_REQUIRE((*runtime)->manifest().manifest_generation == 1);
        GLIFI_REQUIRE((*runtime)->compaction_stats().delta_overflow_aborts == 1);
        require_delta_value(**runtime, "changing", "new");
        GLIFI_REQUIRE((*runtime)->compact_worker(0, 0).compacted());
        GLIFI_REQUIRE((*runtime)->verify_index().has_value());
        runtime->reset();
        auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(reopened.has_value());
        require_delta_value(**reopened, "changing", "new");
        GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
    }
}

GLIFI_TEST("compaction delta acknowledged mutations survive publication and retirement I/O faults") {
    using Op = glifistore::FilesystemOperation;
    for (const auto operation :
         {Op::write_compaction_intent, Op::sync_compaction_intent, Op::write_manifest, Op::sync_manifest,
          Op::rename_manifest, Op::remove_compaction_segment, Op::remove_compaction_intent}) {
        RecoveryTemporaryDirectory temporary;
        seed_delta_store(temporary.path());
        DeltaCheckpoint hook{.target = operation};
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), {.context = &hook, .before = &DeltaCheckpoint::before});
        GLIFI_REQUIRE(directory.has_value());
        auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
        GLIFI_REQUIRE(runtime.has_value());
        hook.runtime = runtime->get();
        const auto result = (*runtime)->compact_worker(0, 0);
        GLIFI_REQUIRE(hook.injected && hook.fired);
        GLIFI_REQUIRE(!result.compacted());
        runtime->reset();
        auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(reopened.has_value());
        require_delta_value(**reopened, "changing", "acknowledged");
        require_missing(**reopened, "erased");
        require_delta_value(**reopened, "stable", "stable");
        GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
        GLIFI_REQUIRE((*reopened)->verify_index().has_value());
    }
}

GLIFI_TEST("compaction delta acknowledged mutations survive process kill at persistent transitions") {
    using Op = glifistore::FilesystemOperation;
    for (const auto operation : {Op::sync_compaction_intent, Op::rename_compaction_intent, Op::rename_segment,
                                 Op::write_manifest, Op::sync_manifest, Op::rename_manifest,
                                 Op::remove_compaction_segment, Op::remove_compaction_intent}) {
        RecoveryTemporaryDirectory temporary;
        seed_delta_store(temporary.path());
        const auto child = ::fork();
        GLIFI_REQUIRE(child >= 0);
        if (child == 0) {
            try {
                DeltaCheckpoint hook{.target = operation, .crash = true};
                auto directory = glifistore::DataDirectory::open_and_lock(
                    temporary.path(),
                    {.context = &hook, .before = &DeltaCheckpoint::before, .after = &DeltaCheckpoint::after});
                if (!directory) {
                    ::_exit(81);
                }
                auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
                if (!runtime) {
                    ::_exit(82);
                }
                hook.runtime = runtime->get();
                static_cast<void>((*runtime)->compact_worker(0, 0));
            } catch (...) {
                ::_exit(83);
            }
            ::_exit(84);
        }
        int status{};
        GLIFI_REQUIRE(::waitpid(child, &status, 0) == child);
        GLIFI_REQUIRE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(reopened.has_value());
        require_delta_value(**reopened, "changing", "acknowledged");
        require_missing(**reopened, "erased");
        require_delta_value(**reopened, "stable", "stable");
        GLIFI_REQUIRE((*reopened)->namespace_audit().recovery_safe());
        GLIFI_REQUIRE((*reopened)->verify_index().has_value());
        // Intent recovery permits recognized private Manifest temporaries. The
        // next ordinary recovery must remove them without changing live state.
        reopened->reset();
        auto clean = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(clean.has_value());
        GLIFI_REQUIRE((*clean)->namespace_audit().clean());
        require_delta_value(**clean, "changing", "acknowledged");
        require_missing(**clean, "erased");
    }
}
