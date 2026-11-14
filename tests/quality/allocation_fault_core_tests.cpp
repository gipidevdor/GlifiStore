#include "allocation_fault_test_support.hpp"
#include "allocation_fault_tests_decl.hpp"
#include "glifistore/index/index.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/global_manager.hpp"
#include "glifistore/segment/record.hpp"

namespace allocation_fault_test {
void run_exhaustive_allocation_failures(const Scenario& scenario) {
    constexpr std::size_t kMaximumExpectedAllocations = 128;
    bool completed{};
    for (std::size_t fail_at = 0; fail_at < kMaximumExpectedAllocations; ++fail_at) {
        TemporaryDirectory temporary;
        initialize_store(temporary.path(), scenario.seed);
        WriteBoundaryObserver observer{.force_rotation = scenario.force_rotation};
        auto options = scenario.options;
        options.limits.resident_reads_enabled = scenario.optional_image_allocations;
        auto runtime = open_runtime(temporary.path(), options, &observer);

        glifistore::DurableMutationResult result;
        allocation_fault::arm(fail_at);
        try {
            result = mutate(*runtime, scenario.kind);
        } catch (...) {
            static_cast<void>(allocation_fault::disarm());
            throw;
        }
        const auto allocation = allocation_fault::disarm();
        if (!allocation.fired) {
            require(result.committed(), "allocation baseline mutation did not commit");
            require(fail_at == allocation.observed,
                    "allocation enumeration changed before reaching its terminal count");
            completed = true;
            break;
        }

        if (scenario.optional_image_allocations && result.committed()) {
            // An image allocation failed and the Segment stayed cold: correctness-neutral.
            require(runtime->healthy(), "optional image allocation failure failed the runtime closed");
            const auto read = runtime->get(kLongKey);
            require(read.has_value() && value_text(*read) == kReplacementValue,
                    "optional image allocation failure lost the committed value");
            continue;
        }
        require(result.error.has_value(), "injected allocation failure returned no error");
        require(result.error->code == glifistore::ErrorCode::resource_exhausted,
                "injected allocation failure was not translated to resource_exhausted");
        const bool write_started = observer.reached.load(std::memory_order_acquire);
        if (!write_started) {
            require(result.outcome == glifistore::DurableMutationOutcome::not_committed ||
                        result.outcome == glifistore::DurableMutationOutcome::indeterminate,
                    "pre-write allocation failure returned an invalid outcome");
            require(runtime->healthy() ==
                        (result.outcome == glifistore::DurableMutationOutcome::not_committed),
                    "pre-write allocation failure health disagrees with its outcome");
        } else {
            if (result.outcome != glifistore::DurableMutationOutcome::indeterminate) {
                throw std::runtime_error(std::string{scenario.name} + " allocation " +
                                         std::to_string(fail_at) +
                                         " crossed the write boundary without an indeterminate outcome");
            }
            require(!runtime->healthy(), "post-write allocation failure did not fail closed");
            const auto blocked = mutate(*runtime, scenario.kind);
            // Pre-I/O reject when already fail-closed is not_committed (no write boundary).
            require(blocked.outcome == glifistore::DurableMutationOutcome::not_committed,
                    "failed-closed runtime accepted another mutation");
            require(blocked.error.has_value() && blocked.error->code == glifistore::ErrorCode::unavailable,
                    "failed-closed reject was not unavailable");
        }
        runtime.reset();
        if (!write_started || scenario.force_rotation) {
            require_recovered_prewrite_state(temporary.path(), scenario.seed);
        }
    }
    require(completed, std::string{scenario.name} + " exceeded allocation enumeration limit");
}

void run_no_post_write_allocation(const glifistore::DurableRuntimeOptions options) {
    TemporaryDirectory temporary;
    initialize_store(temporary.path(), false);
    WriteBoundaryObserver observer{.forbid_allocations = true};
    auto runtime = open_runtime(temporary.path(), options, &observer);

    glifistore::DurableMutationResult result;
    try {
        result = mutate(*runtime, MutationKind::put_new);
    } catch (...) {
        static_cast<void>(allocation_fault::end_forbid_all());
        throw;
    }
    const bool forbidden_allocation = allocation_fault::end_forbid_all();
    require(observer.reached.load(std::memory_order_acquire),
            "post-write allocation guard never reached the persistent write boundary");
    require(!forbidden_allocation, "durable mutation allocated after its persistent write boundary");
    require(result.committed(), "post-write allocation guard prevented a durable commit");
    require(runtime->healthy(), "post-write allocation guard failed the runtime closed");
}

// Wave 2: steady-state paired volatile GET ≤ OwnedBytes::kInlineBytes (64) must not
// heap-allocate. Warm the path first (lease / Index / SSO), then forbid operator new.
void run_paired_volatile_get_inline_zero_heap() {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    require(opened.has_value(), "failed to open paired volatile zero-heap GET Store");
    auto& store = **opened;
    constexpr std::string_view kKey = "wave2-inline-get";
    // Exactly OwnedBytes::kInlineBytes — SSO ceiling, no heap on value_copy.
    constexpr char kValueBytes[64] = {
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    const std::string_view kValue{kValueBytes, 64};
    require(store.put(kKey, bytes(kValue)).has_value(), "failed to seed inline GET value");
    {
        const auto warm = store.get(kKey);
        require(warm.has_value(), "warm GET failed before zero-heap window");
        require(warm->bytes.size() == kValue.size(), "warm GET size mismatch");
    }
    allocation_fault::begin_forbid_all();
    std::optional<glifistore::Result<glifistore::OwnedValue>> got;
    try {
        got = store.get(kKey);
    } catch (...) {
        static_cast<void>(allocation_fault::end_forbid_all());
        throw;
    }
    const bool allocated = allocation_fault::end_forbid_all();
    require(!allocated, "paired volatile GET ≤64 B allocated on the steady-state path");
    require(got.has_value() && got->has_value(), "zero-heap GET failed");
    require((*got)->bytes.size() == kValue.size(), "zero-heap GET size mismatch");
    require(std::string_view(reinterpret_cast<const char*>((*got)->bytes.data()), (*got)->bytes.size()) ==
                kValue,
            "zero-heap GET value mismatch");
    require(store.close().has_value(), "failed to close zero-heap GET Store");
}

void run_exhaustive_read_failures() {
    constexpr std::size_t kMaximumExpectedAllocations = 32;
    bool completed{};
    for (std::size_t fail_at = 0; fail_at < kMaximumExpectedAllocations; ++fail_at) {
        TemporaryDirectory temporary;
        initialize_store(temporary.path(), true);
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = temporary.path(),
            .durable_open_mode = glifistore::DurableOpenMode::open_existing,
        });
        require(opened.has_value(), "failed to open durable read allocation test Store");

        glifistore::Result<glifistore::OwnedValue> result =
            glifistore::unexpected(glifistore::Error{glifistore::ErrorCode::internal_error, {}});
        allocation_fault::arm(fail_at);
        try {
            result = (*opened)->get(kLongKey);
        } catch (...) {
            static_cast<void>(allocation_fault::disarm());
            throw;
        }
        const auto allocation = allocation_fault::disarm();
        if (!allocation.fired) {
            require(result.has_value(), "allocation baseline read failed");
            require(value_text(*result) == kOriginalValue, "allocation baseline read returned wrong value");
            require(fail_at == allocation.observed,
                    "read allocation enumeration changed before its terminal count");
            completed = true;
            break;
        }
        require(!result.has_value(), "injected read allocation failure returned a value");
        require(result.error().code == glifistore::ErrorCode::resource_exhausted,
                "injected read allocation failure was not translated to resource_exhausted");
        const auto repeated = (*opened)->get(kLongKey);
        require(repeated.has_value(), "read allocation failure poisoned an otherwise healthy Store");
        require(value_text(*repeated) == kOriginalValue, "read allocation failure changed the stored value");
    }
    require(completed, "durable read exceeded allocation enumeration limit");
}

struct ThrowingSyncHook {
    std::atomic_bool fired{};

    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& hook = *static_cast<ThrowingSyncHook*>(opaque);
        if (operation == glifistore::FilesystemOperation::sync_record &&
            !hook.fired.exchange(true, std::memory_order_acq_rel)) {
            throw std::bad_alloc{};
        }
        return {};
    }
};

void run_background_allocation_failure_waiters() {
    TemporaryDirectory temporary;
    initialize_store(temporary.path(), false);
    ThrowingSyncHook hook;
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), glifistore::FilesystemHooks{.context = &hook, .before = &ThrowingSyncHook::before});
    require(directory.has_value(), "failed to lock background allocation test Store");
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(
        std::move(*directory), 0,
        {.commit_sync = glifistore::SegmentCommitSync::immediate,
         .sync_interval_ms = 60'000,
         .batch =
             glifistore::DurableGroupConfig{.max_records = 2, .max_bytes = 65'536, .max_wait_ms = 60'000},
         .strict_ack = true});
    require(runtime.has_value(), "failed to recover background allocation test Store");

    const std::array<std::string, 2> keys{"first-allocation-waiter", "second-allocation-waiter"};
    const std::string value{"value"};
    std::array<glifistore::DurableMutationResult, 2> results;
    std::array<std::thread, 2> producers{
        std::thread{[&] { results[0] = (*runtime)->put(bytes(keys[0]), bytes(value)); }},
        std::thread{[&] { results[1] = (*runtime)->put(bytes(keys[1]), bytes(value)); }},
    };
    for (auto& producer : producers) {
        producer.join();
    }

    require(hook.fired.load(std::memory_order_acquire), "background allocation failure hook did not fire");
    for (const auto& result : results) {
        require(result.outcome == glifistore::DurableMutationOutcome::indeterminate,
                "background allocation failure did not release a waiter as indeterminate");
    }
    require(!(*runtime)->healthy(), "background allocation failure did not fail closed");
}

void require_recovered_compaction_state(const std::filesystem::path& path) {
    auto runtime = open_runtime(path, {});
    const auto first = runtime->get("first");
    const auto second = runtime->get("second");
    require(first.has_value() && value_text(*first) == "first-value",
            "allocation compaction recovery lost the first value");
    require(second.has_value() && value_text(*second) == "second-value",
            "allocation compaction recovery lost the second value");
    require(runtime->namespace_audit().clean(), "allocation compaction recovery left a dirty namespace");
}

// With resident images disabled every enumerated allocation is mandatory. With them enabled, an
// output image allocation is optional (ADR 0046): its failure may leave the compaction committed
// with that output cold, which must stay healthy and readable.
void run_exhaustive_compaction_allocation_failures(const bool resident_images) {
    constexpr std::size_t kMaximumExpectedAllocations = 512;
    glifistore::DurableRuntimeOptions options{};
    options.limits.resident_reads_enabled = resident_images;
    bool completed{};
    for (std::size_t fail_at = 0; fail_at < kMaximumExpectedAllocations; ++fail_at) {
        TemporaryDirectory temporary;
        initialize_compaction_store(temporary.path());
        auto runtime = open_runtime(temporary.path(), options);

        glifistore::DurableCompactionResult result;
        allocation_fault::arm(fail_at);
        try {
            result = runtime->compact_worker(0, 0);
        } catch (...) {
            static_cast<void>(allocation_fault::disarm());
            throw;
        }
        const auto allocation = allocation_fault::disarm();
        if (!allocation.fired) {
            require(result.compacted(), "allocation compaction baseline did not compact");
            require(fail_at == allocation.observed,
                    "compaction allocation enumeration changed before its terminal count");
            runtime.reset();
            require_recovered_compaction_state(temporary.path());
            completed = true;
            break;
        }

        if (resident_images && result.compacted()) {
            require(runtime->healthy(), "optional image allocation failure failed compaction closed");
            runtime.reset();
            require_recovered_compaction_state(temporary.path());
            continue;
        }
        require(result.error.has_value(), "injected compaction allocation failure returned no error");
        require(result.error->code == glifistore::ErrorCode::resource_exhausted,
                "injected compaction allocation failure was not resource_exhausted");
        require(result.outcome == glifistore::DurableCompactionOutcome::not_compacted ||
                    result.outcome == glifistore::DurableCompactionOutcome::recovery_required,
                "injected compaction allocation failure returned an invalid outcome");
        require(runtime->healthy() == (result.outcome == glifistore::DurableCompactionOutcome::not_compacted),
                "compaction allocation failure health disagrees with recovery requirement");
        runtime.reset();
        require_recovered_compaction_state(temporary.path());
    }
    require(completed, "durable compaction exceeded its allocation enumeration limit");
}

void run_volatile_rotation_allocation_failures() {
    glifistore::GlobalSegmentManager manager;
    const auto active = manager.allocate_active(glifistore::WorkerId{0});
    glifistore::SegmentPtr replacement;

    constexpr std::size_t kMaximumExpectedAllocations = 8;
    for (std::size_t fail_at = 0; fail_at < kMaximumExpectedAllocations; ++fail_at) {
        bool threw{};
        allocation_fault::arm(fail_at);
        try {
            const auto prepared = manager.prepare_rotation(active, glifistore::WorkerId{0});
            require(prepared.has_value(), "volatile rotation preparation returned an error");
            replacement = *prepared;
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const auto allocation = allocation_fault::disarm();
        require(active->state() == glifistore::SegmentState::active,
                "failed rotation preparation sealed the old active Segment");
        require(manager.segments().size() == 1,
                "failed rotation preparation published a replacement Segment");
        if (!threw) {
            require(!allocation.fired, "rotation preparation swallowed an allocation failure");
            require(fail_at == allocation.observed,
                    "rotation preparation allocation enumeration changed unexpectedly");
            break;
        }
        require(allocation.fired, "rotation preparation threw before the injected allocation");
    }
    require(replacement != nullptr, "rotation preparation exceeded its allocation bound");
    require(manager.find(replacement->id()) == nullptr,
            "prepared replacement became visible before rotation commit");

    bool commit_threw{};
    allocation_fault::arm(0);
    try {
        static_cast<void>(manager.commit_rotation(active, replacement));
    } catch (const std::bad_alloc&) {
        commit_threw = true;
    }
    const auto commit_allocation = allocation_fault::disarm();
    require(commit_threw && commit_allocation.fired,
            "rotation commit did not expose its first catalog allocation");
    require(active->state() == glifistore::SegmentState::active,
            "failed rotation commit sealed the old active Segment");
    require(manager.find(replacement->id()) == nullptr,
            "failed rotation commit published the replacement Segment");

    require(manager.commit_rotation(active, replacement).has_value(),
            "rotation commit did not recover after allocation failure");
    require(active->state() == glifistore::SegmentState::sealed,
            "successful rotation did not seal the previous active Segment");
    require(manager.find(replacement->id()) == replacement,
            "successful rotation did not publish the replacement Segment");
}

void run_volatile_vacuum_publication_allocation_failures() {
    constexpr std::size_t kMaximumExpectedAllocations = 8;
    bool completed{};
    for (std::size_t fail_at = 0; fail_at < kMaximumExpectedAllocations; ++fail_at) {
        glifistore::GlobalSegmentManager manager;
        const auto first = manager.allocate_active(glifistore::WorkerId{0});
        const auto second = manager.prepare_rotation(first, glifistore::WorkerId{0});
        require(second.has_value(), "failed to prepare the second vacuum source Segment");
        require(manager.commit_rotation(first, *second).has_value(),
                "failed to publish the second vacuum source Segment");
        const auto active = manager.prepare_rotation(*second, glifistore::WorkerId{0});
        require(active.has_value(), "failed to prepare the post-vacuum active Segment");
        require(manager.commit_rotation(*second, *active).has_value(),
                "failed to publish the post-vacuum active Segment");

        const auto first_replacement = manager.prepare_segment(glifistore::WorkerId{0});
        const auto second_replacement = manager.prepare_segment(glifistore::WorkerId{0});
        require(first_replacement.has_value() && second_replacement.has_value(),
                "failed to prepare vacuum replacement Segments");
        require((*first_replacement)->seal().has_value() && (*second_replacement)->seal().has_value(),
                "failed to seal vacuum replacement Segments");
        const std::array sources{first->id(), (*second)->id()};
        const std::array replacements{*first_replacement, *second_replacement};

        bool threw{};
        glifistore::Status result;
        allocation_fault::arm(fail_at);
        try {
            result = manager.replace_sealed(sources, replacements);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const auto allocation = allocation_fault::disarm();
        if (threw) {
            require(allocation.fired, "vacuum publication threw before the injected allocation");
            require(manager.find(first->id()) == first && manager.find((*second)->id()) == *second,
                    "failed vacuum publication retired a source Segment");
            require(manager.find((*first_replacement)->id()) == nullptr &&
                        manager.find((*second_replacement)->id()) == nullptr,
                    "failed vacuum publication left a partial replacement catalog");
            continue;
        }

        require(result.has_value(), "vacuum publication baseline returned an error");
        require(!allocation.fired, "vacuum publication swallowed an allocation failure");
        require(fail_at == allocation.observed,
                "vacuum publication allocation enumeration changed unexpectedly");
        require(manager.find(first->id()) == nullptr && manager.find((*second)->id()) == nullptr,
                "successful vacuum publication retained a source Segment");
        require(manager.find((*first_replacement)->id()) == *first_replacement &&
                    manager.find((*second_replacement)->id()) == *second_replacement,
                "successful vacuum publication omitted a replacement Segment");
        completed = true;
        break;
    }
    require(completed, "vacuum publication exceeded its allocation bound");
}

void run_index_tombstone_rebuild_allocation_failures() {
    bool completed{};
    for (std::size_t fail_at = 0; fail_at < 128; ++fail_at) {
        glifistore::Index index;
        std::vector<std::string> keys;
        keys.reserve(80);
        for (std::uint64_t value = 0; value < 80; ++value) {
            auto key = std::string(64, 't');
            key.replace(key.size() - std::to_string(value).size(), std::to_string(value).size(),
                        std::to_string(value));
            const glifistore::RecordRef ref{glifistore::SegmentId{1}, glifistore::RecordOffset{4096},
                                            glifistore::RecordSize{64}, glifistore::SequenceNumber{value + 1},
                                            glifistore::GenerationId{1}};
            require(index.insert_or_assign(key, ref).has_value(), "failed to seed tombstone rebuild");
            keys.push_back(std::move(key));
        }
        for (std::size_t value = 0; value < 60; ++value) {
            const glifistore::HashedKey key{keys[value], glifistore::hash_key(keys[value])};
            require(index.erase_no_compact(key).previous.has_value(), "failed to seed deleted slot");
        }
        const auto before = index.stats();
        const glifistore::HashedKey next{"inline-next", glifistore::hash_key("inline-next")};

        bool threw{};
        glifistore::Status prepared;
        allocation_fault::arm(fail_at);
        try {
            prepared = index.prepare_insert(next);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const auto allocation = allocation_fault::disarm();
        if (allocation.fired) {
            require(threw, "tombstone rebuild swallowed an injected allocation failure");
            const auto after = index.stats();
            require(after.size == before.size && after.deleted_count == before.deleted_count &&
                        after.bucket_count == before.bucket_count &&
                        after.arena_allocated_bytes == before.arena_allocated_bytes &&
                        after.arena_live_bytes == before.arena_live_bytes,
                    "failed tombstone rebuild changed authoritative table state");
            for (std::size_t value = 60; value < keys.size(); ++value) {
                require(index.find(keys[value]).has_value(), "failed tombstone rebuild lost a live key");
            }
            continue;
        }
        require(!threw && prepared.has_value(), "tombstone rebuild baseline failed");
        require(fail_at == allocation.observed,
                "tombstone rebuild allocation enumeration changed unexpectedly");
        require(index.stats().deleted_count == 0, "successful tombstone rebuild retained deleted slots");
        completed = true;
        break;
    }
    require(completed, "tombstone rebuild exceeded its allocation bound");
}
} // namespace allocation_fault_test
