#include "glifistore/store/store.hpp"
#include "persistence_recovery_test_support.hpp"

#include <array>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

void seed_remap_store(const std::filesystem::path& path) {
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

auto remap_put(glifistore::DurableRuntimeCatalog& runtime, const std::string_view key,
               const std::string_view value) -> glifistore::DurableMutationResult {
    return runtime.put(std::as_bytes(std::span{key}), std::as_bytes(std::span{value}));
}

auto remap_erase(glifistore::DurableRuntimeCatalog& runtime, const std::string_view key)
    -> glifistore::DurableMutationResult {
    return runtime.erase(std::as_bytes(std::span{key}));
}

void require_remap_value(glifistore::DurableRuntimeCatalog& runtime, const std::string_view key,
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
struct RemapCheckpoint {
    glifistore::DurableRuntimeCatalog* runtime{};
    glifistore::FilesystemOperation target{};
    bool injected{};
    bool fired{};
    bool crash{};

    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& self = *static_cast<RemapCheckpoint*>(opaque);
        if (!self.injected && operation == glifistore::FilesystemOperation::write_compaction_intent) {
            self.injected = true;
            if (!remap_put(*self.runtime, "changing", "acknowledged").committed() ||
                !remap_erase(*self.runtime, "erased").committed()) {
                return glifistore::fail(glifistore::ErrorCode::io_error, "remap fixture mutation failed");
            }
        }
        if (!self.crash && self.injected && !self.fired && operation == self.target) {
            self.fired = true;
            return glifistore::fail(glifistore::ErrorCode::io_error, "remap checkpoint fault");
        }
        return {};
    }

    static void after(void* opaque, const glifistore::FilesystemOperation operation) {
        auto& self = *static_cast<RemapCheckpoint*>(opaque);
        if (self.crash && self.injected && operation == self.target) {
            static_cast<void>(::kill(::getpid(), SIGKILL));
        }
    }
};

} // namespace

GLIFI_TEST("compaction remap preserves overwrite erase restore active and TTL across durable modes") {
    for (unsigned mode = 0; mode != 4; ++mode) {
        RecoveryTemporaryDirectory temporary;
        seed_remap_store(temporary.path());
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
            mutations_ok = remap_put(**runtime, "changing", "new").committed() &&
                           remap_erase(**runtime, "erased").committed() &&
                           remap_erase(**runtime, "restored").committed() &&
                           remap_put(**runtime, "restored", "restored").committed() &&
                           remap_put(**runtime, fresh_key, "fresh").committed() &&
                           remap_put(**runtime, "active", "active").committed();
            const auto expired = (*runtime)->get("expiry", 100);
            mutations_ok =
                mutations_ok && !expired && expired.error().code == glifistore::ErrorCode::not_found;
        }
        blocker.release();
        compactor.join();
        GLIFI_REQUIRE(mutations_ok);
        GLIFI_REQUIRE(compacted.compacted());
        // ADR 0044: five sealed Records were copied. Only "stable" kept its
        // snapshot reference; the four others were superseded during the build
        // (overwrite, erase, erase + reinsert, lazy TTL reclaim).
        GLIFI_REQUIRE(compacted.stats.source_segments == 2);
        GLIFI_REQUIRE(compacted.stats.records_copied == 5);
        GLIFI_REQUIRE(compacted.stats.remapped_records == 1);
        GLIFI_REQUIRE(compacted.stats.superseded_copies == 4);
        GLIFI_REQUIRE(compacted.stats.delta_records == 0);
        GLIFI_REQUIRE(compacted.stats.snapshot_duration_ns > 0);
        GLIFI_REQUIRE(compacted.stats.commit_gate_duration_ns > 0);
        require_remap_value(**runtime, "changing", "new");
        require_remap_value(**runtime, "stable", "stable");
        require_remap_value(**runtime, "restored", "restored");
        require_remap_value(**runtime, fresh_key, "fresh");
        require_remap_value(**runtime, "active", "active");
        require_missing(**runtime, "erased");
        require_missing(**runtime, "expiry");
        GLIFI_REQUIRE((*runtime)->verify_index().has_value());
        const auto metrics = (*runtime)->compaction_stats();
        GLIFI_REQUIRE(metrics.attempts == 1 && metrics.completed == 1 && metrics.remapped_records == 1 &&
                      metrics.superseded_copies == 4 && metrics.replayed_records == 0);
        GLIFI_REQUIRE((*runtime)->close().has_value());
        runtime->reset();
        auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), 100);
        GLIFI_REQUIRE(reopened.has_value());
        require_remap_value(**reopened, "changing", "new");
        require_remap_value(**reopened, "restored", "restored");
        require_remap_value(**reopened, fresh_key, "fresh");
        require_remap_value(**reopened, "active", "active");
        require_missing(**reopened, "erased");
        require_missing(**reopened, "expiry");
        GLIFI_REQUIRE((*reopened)->verify_index().has_value());
    }
}

GLIFI_TEST("compaction remap retargets only the exact source reference and never inserts") {
    glifistore::Index index;
    std::string binary_key(512, 'k');
    binary_key[64] = '\0';
    const auto key = glifistore::HashedKey::compute(binary_key);
    const glifistore::RecordRef source{.segment_id = glifistore::SegmentId{1},
                                       .offset = glifistore::RecordOffset{4096},
                                       .size = glifistore::RecordSize{1024},
                                       .sequence = glifistore::SequenceNumber{7},
                                       .generation = glifistore::GenerationId{1}};
    auto output = source;
    output.generation = glifistore::GenerationId{2};
    output.offset = glifistore::RecordOffset{8192};
    auto newer = source;
    newer.segment_id = glifistore::SegmentId{3};
    newer.sequence = glifistore::SequenceNumber{9};

    const auto absent = glifistore::HashedKey::compute("absent");
    GLIFI_REQUIRE(!index.replace_if_current(absent, source, output));
    GLIFI_REQUIRE(index.stats().size == 0);

    GLIFI_REQUIRE(index.insert_or_assign(key, newer).has_value());
    GLIFI_REQUIRE(!index.replace_if_current(key, source, output));
    GLIFI_REQUIRE(index.find(key) == newer);

    GLIFI_REQUIRE(index.insert_or_assign(key, source).has_value());
    GLIFI_REQUIRE(index.replace_if_current(key, source, output));
    GLIFI_REQUIRE(index.find(key) == output);
    GLIFI_REQUIRE(!index.replace_if_current(key, source, output));
    GLIFI_REQUIRE(index.stats().size == 1);
}

GLIFI_TEST("compaction remap publishes concurrent acknowledged writes through paired Store generations") {
    RecoveryTemporaryDirectory temporary;
    seed_remap_store(temporary.path());
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
    GLIFI_REQUIRE((**compacted).superseded_copies == 2);
    GLIFI_REQUIRE((**compacted).delta_records == 0);
    const auto changed = (*store)->get("changing");
    GLIFI_REQUIRE(changed.has_value() && owned_text(*changed) == value);
    const auto erased = (*store)->get("erased");
    GLIFI_REQUIRE(!erased && erased.error().code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE((*store)->verify_index().has_value());
    GLIFI_REQUIRE((*store)->maintenance_snapshot().compaction.completed == 1);
}

GLIFI_TEST("compaction completes under concurrent writes for every retired delta limit") {
    // ADR 0043 aborted these transactions on journal overflow. ADR 0044 keeps no
    // journal, so the retired limits must not matter and no copy is abandoned.
    for (unsigned budget = 0; budget != 3; ++budget) {
        RecoveryTemporaryDirectory temporary;
        seed_remap_store(temporary.path());
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
            for (unsigned index = 0; index != 64; ++index) {
                committed =
                    remap_put(**runtime, "changing", "new-" + std::to_string(index)).committed() && committed;
            }
        }
        blocker.release();
        compactor.join();
        GLIFI_REQUIRE(committed);
        GLIFI_REQUIRE(compacted.compacted());
        GLIFI_REQUIRE(compacted.stats.delta_overflow_aborts == 0);
        GLIFI_REQUIRE(compacted.stats.abandoned_copy_bytes == 0);
        GLIFI_REQUIRE(compacted.stats.superseded_copies == 1);
        GLIFI_REQUIRE((*runtime)->healthy());
        GLIFI_REQUIRE((*runtime)->manifest().manifest_generation == 2);
        require_remap_value(**runtime, "changing", "new-63");
        GLIFI_REQUIRE((*runtime)->verify_index().has_value());
        runtime->reset();
        auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(reopened.has_value());
        require_remap_value(**reopened, "changing", "new-63");
        GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
    }
}

GLIFI_TEST("compaction remap acknowledged mutations survive publication and retirement I/O faults") {
    using Op = glifistore::FilesystemOperation;
    for (const auto operation :
         {Op::write_compaction_intent, Op::sync_compaction_intent, Op::write_manifest, Op::sync_manifest,
          Op::rename_manifest, Op::remove_compaction_segment, Op::remove_compaction_intent}) {
        RecoveryTemporaryDirectory temporary;
        seed_remap_store(temporary.path());
        RemapCheckpoint hook{.target = operation};
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), {.context = &hook, .before = &RemapCheckpoint::before});
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
        require_remap_value(**reopened, "changing", "acknowledged");
        require_missing(**reopened, "erased");
        require_remap_value(**reopened, "stable", "stable");
        GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
        GLIFI_REQUIRE((*reopened)->verify_index().has_value());
    }
}

GLIFI_TEST("compaction remap acknowledged mutations survive process kill at persistent transitions") {
    using Op = glifistore::FilesystemOperation;
    for (const auto operation : {Op::sync_compaction_intent, Op::rename_compaction_intent, Op::rename_segment,
                                 Op::write_manifest, Op::sync_manifest, Op::rename_manifest,
                                 Op::remove_compaction_segment, Op::remove_compaction_intent}) {
        RecoveryTemporaryDirectory temporary;
        seed_remap_store(temporary.path());
        const auto child = ::fork();
        GLIFI_REQUIRE(child >= 0);
        if (child == 0) {
            try {
                RemapCheckpoint hook{.target = operation, .crash = true};
                auto directory = glifistore::DataDirectory::open_and_lock(
                    temporary.path(),
                    {.context = &hook, .before = &RemapCheckpoint::before, .after = &RemapCheckpoint::after});
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
        require_remap_value(**reopened, "changing", "acknowledged");
        require_missing(**reopened, "erased");
        require_remap_value(**reopened, "stable", "stable");
        GLIFI_REQUIRE((*reopened)->namespace_audit().recovery_safe());
        GLIFI_REQUIRE((*reopened)->verify_index().has_value());
        // Intent recovery permits recognized private Manifest temporaries. The
        // next ordinary recovery must remove them without changing live state.
        reopened->reset();
        auto clean = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(clean.has_value());
        GLIFI_REQUIRE((*clean)->namespace_audit().clean());
        require_remap_value(**clean, "changing", "acknowledged");
        require_missing(**clean, "erased");
    }
}
