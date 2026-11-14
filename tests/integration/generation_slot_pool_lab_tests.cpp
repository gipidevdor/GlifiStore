#include "experimental/generation_slot_pool.hpp"
#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/persistence/store_backup.hpp"
#include "glifistore/server/daemon_log.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/store/store.hpp"
#include "server/reactor_detail.hpp"
#include "server_reactor_test_support.hpp"
#include "store/store_internal.hpp"
#include "test.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <mutex>
#include <netinet/in.h>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

using namespace glifistore::test::server_reactor_support;

GLIFI_TEST("ADR 0036 V8 candidate preserves durable cold pin across compacted slot refresh") {
    using Generation = glifistore::server::PairReadGeneration;
    using Pool = glifistore::experimental::GenerationSlotPool<Generation, 4>;

    ServerTemporaryDirectory temporary;
    BlockingCompactionIntent blocker;
    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = kTestMutationArenaBytes,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         // Cold-pin lifetime test: resident images (ADR 0046) would complete reads at prepare.
         .durable_limits = {.resident_reads_enabled = false},
         .filesystem_hooks = {.context = &blocker, .before = &BlockingCompactionIntent::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    // A live anchor in the oldest Segment makes ADR 0044 select both sealed
    // Segments, so the refreshed Record moves to a different Segment.
    GLIFI_REQUIRE(store.put("slot-refresh-anchor", bytes("anchor")).has_value());
    GLIFI_REQUIRE(store.put("slot-refresh", bytes("old")).has_value());
    blocker.force_next_record_write_full();
    GLIFI_REQUIRE(store.put("slot-refresh", bytes("current")).has_value());
    blocker.force_next_record_write_full();
    GLIFI_REQUIRE(store.put("slot-active", bytes("active")).has_value());

    auto initial_snapshot = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0);
    GLIFI_REQUIRE(initial_snapshot.has_value());
    auto initial_result = Generation::from_durable_snapshot(
        glifistore::detail::StoreAccess::worker_routing(store), initial_snapshot->records);
    GLIFI_REQUIRE(initial_result.has_value());
    auto initial = std::move(*initial_result);
    auto replacement_parent = initial;
    std::weak_ptr<const Generation> initial_lifetime = initial;
    auto pool = Pool::create(std::move(initial));
    GLIFI_REQUIRE(pool.has_value());

    const auto* adopted_initial = (*pool)->adopt();
    GLIFI_REQUIRE(adopted_initial != nullptr);
    const auto initial_epoch = adopted_initial->epoch();
    const std::string key{"slot-refresh"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key(key)};
    auto initial_record = adopted_initial->prepare_durable(hashed);
    GLIFI_REQUIRE(initial_record.has_value());
    const auto initial_reference = initial_record->reference();
    auto pending =
        glifistore::detail::StoreAccess::prepare_published_durable_get(store, 0, *initial_record, 0);
    GLIFI_REQUIRE(pending.has_value());
    GLIFI_REQUIRE(!pending->value.has_value());
    GLIFI_REQUIRE(pending->cold.has_value());

    std::optional<glifistore::Result<glifistore::CompactionResult>> compacted;
    std::thread compactor{[&] { compacted = store.compact(); }};
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    blocker.release();
    compactor.join();
    GLIFI_REQUIRE(compacted.has_value());
    GLIFI_REQUIRE(compacted->has_value());
    GLIFI_REQUIRE((*compacted)->compacted);

    auto refreshed_snapshot = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0);
    GLIFI_REQUIRE(refreshed_snapshot.has_value());
    auto refreshed_result =
        Generation::replace_durable_snapshot(replacement_parent, refreshed_snapshot->records);
    GLIFI_REQUIRE(refreshed_result.has_value());
    replacement_parent.reset();
    initial_snapshot->records.clear();
    GLIFI_REQUIRE((*pool)->try_publish(std::move(*refreshed_result)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);

    const auto* refreshed = (*pool)->adopt(initial_epoch);
    GLIFI_REQUIRE(refreshed != nullptr);
    GLIFI_REQUIRE(refreshed->epoch() > initial_epoch);
    auto refreshed_record = refreshed->prepare_durable(hashed);
    GLIFI_REQUIRE(refreshed_record.has_value());
    GLIFI_REQUIRE(refreshed_record->reference().sequence == initial_reference.sequence);
    GLIFI_REQUIRE(refreshed_record->reference().segment_id != initial_reference.segment_id);
    (*pool)->reclaim();
    GLIFI_REQUIRE(!initial_lifetime.expired());
    GLIFI_REQUIRE((*pool)->stats().reader_safe_epoch == initial_epoch);

    auto value = glifistore::detail::StoreAccess::complete_get_owned(store, 0, std::move(*pending->cold));
    GLIFI_REQUIRE(value.has_value());
    GLIFI_REQUIRE(text(value->view()) == "current");
    GLIFI_REQUIRE((*pool)->adopt() == refreshed);
    (*pool)->reclaim();
    GLIFI_REQUIRE(initial_lifetime.expired());
    GLIFI_REQUIRE((*pool)->stats().live_slots == 1);

    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("ADR 0036 V8 candidate publishes a Writer-owned rotation as one slot generation") {
    using Generation = glifistore::server::PairReadGeneration;
    using Pool = glifistore::experimental::GenerationSlotPool<Generation, 4>;

    ServerTemporaryDirectory temporary;
    BlockingCompactionIntent blocker;
    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 4,
                    .async_lane_payload_bytes = kTestMutationArenaBytes,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.context = &blocker, .before = &BlockingCompactionIntent::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("slot-rotation-base", bytes("base")).has_value());

    auto initial_snapshot = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0);
    GLIFI_REQUIRE(initial_snapshot.has_value());
    auto initial_result = Generation::from_durable_snapshot(
        glifistore::detail::StoreAccess::worker_routing(store), initial_snapshot->records);
    GLIFI_REQUIRE(initial_result.has_value());
    auto initial = std::move(*initial_result);
    auto replacement_parent = initial;
    std::weak_ptr<const Generation> initial_lifetime = initial;
    auto pool = Pool::create(std::move(initial));
    GLIFI_REQUIRE(pool.has_value());
    const auto* initial_read = (*pool)->adopt();
    GLIFI_REQUIRE(initial_read != nullptr);
    const auto initial_epoch = initial_read->epoch();

    blocker.force_next_record_write_full();
    GLIFI_REQUIRE(store.put("slot-rotation-new", bytes("rotated")).has_value());
    auto rotated_snapshot = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0);
    GLIFI_REQUIRE(rotated_snapshot.has_value());
    auto rotated_result = Generation::replace_durable_snapshot(replacement_parent, rotated_snapshot->records);
    GLIFI_REQUIRE(rotated_result.has_value());
    replacement_parent.reset();
    initial_snapshot->records.clear();
    GLIFI_REQUIRE((*pool)->try_publish(std::move(*rotated_result)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);

    const auto* rotated = (*pool)->adopt(initial_epoch);
    GLIFI_REQUIRE(rotated != nullptr);
    GLIFI_REQUIRE(rotated->epoch() == initial_epoch + 1U);
    GLIFI_REQUIRE(rotated->delta_entries() == 0);
    GLIFI_REQUIRE(rotated->base_entries() == 2);
    const glifistore::HashedKey base{"slot-rotation-base", glifistore::hash_key("slot-rotation-base")};
    const glifistore::HashedKey added{"slot-rotation-new", glifistore::hash_key("slot-rotation-new")};
    GLIFI_REQUIRE(rotated->prepare_durable(base).has_value());
    GLIFI_REQUIRE(rotated->prepare_durable(added).has_value());
    (*pool)->reclaim();
    GLIFI_REQUIRE(!initial_lifetime.expired());

    GLIFI_REQUIRE((*pool)->adopt() == rotated);
    (*pool)->reclaim();
    GLIFI_REQUIRE(initial_lifetime.expired());
    GLIFI_REQUIRE((*pool)->stats().live_slots == 1);
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("ADR 0036 V5 candidate shutdown retires a real durable generation after Reader drain") {
    using Generation = glifistore::server::PairReadGeneration;
    using Pool = glifistore::experimental::GenerationSlotPool<Generation, 4>;

    ServerTemporaryDirectory temporary;
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .concurrency = glifistore::StoreConcurrencyMode::paired,
                                           .paired = {.async_lane_capacity = 4,
                                                      .async_lane_payload_bytes = kTestMutationArenaBytes,
                                                      .reader_epoch_lease = true},
                                           .storage_mode = glifistore::StorageMode::durable_sync,
                                           .data_directory = temporary.store_path(),
                                           .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("slot-v5-initial", bytes("initial")).has_value());

    auto initial_snapshot = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0);
    GLIFI_REQUIRE(initial_snapshot.has_value());
    auto initial_result = Generation::from_durable_snapshot(
        glifistore::detail::StoreAccess::worker_routing(store), initial_snapshot->records);
    GLIFI_REQUIRE(initial_result.has_value());
    auto initial = std::move(*initial_result);
    auto replacement_parent = initial;
    std::weak_ptr<const Generation> initial_lifetime = initial;
    auto pool = Pool::create(std::move(initial));
    GLIFI_REQUIRE(pool.has_value());
    const auto* adopted_initial = (*pool)->adopt();
    GLIFI_REQUIRE(adopted_initial != nullptr);
    const auto borrowed_epoch = adopted_initial->epoch();

    GLIFI_REQUIRE(store.put("slot-v5-final", bytes("final")).has_value());
    auto final_snapshot = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0);
    GLIFI_REQUIRE(final_snapshot.has_value());
    auto final_result = Generation::replace_durable_snapshot(replacement_parent, final_snapshot->records);
    GLIFI_REQUIRE(final_result.has_value());
    replacement_parent.reset();
    initial_snapshot->records.clear();
    GLIFI_REQUIRE((*pool)->try_publish(std::move(*final_result)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    GLIFI_REQUIRE((*pool)->adopt(borrowed_epoch) != nullptr);

    (*pool)->stop_admission();
    (*pool)->reclaim();
    GLIFI_REQUIRE(!initial_lifetime.expired());
    GLIFI_REQUIRE(!(*pool)->try_finish_shutdown());

    // The owner completes all output/cold borrows before this terminal edge.
    GLIFI_REQUIRE((*pool)->mark_reader_quiescent());
    GLIFI_REQUIRE((*pool)->try_finish_shutdown());
    GLIFI_REQUIRE(initial_lifetime.expired());
    GLIFI_REQUIRE((*pool)->stats().live_slots == 1);
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("ADR 0036 V6 candidate fail-closes a committed mutation then snapshot-drains authority") {
    using Generation = glifistore::server::PairReadGeneration;
    using Pool = glifistore::experimental::GenerationSlotPool<Generation, 4>;

    ServerTemporaryDirectory temporary;
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .concurrency = glifistore::StoreConcurrencyMode::paired,
                                           .paired = {.async_lane_capacity = 4,
                                                      .async_lane_payload_bytes = kTestMutationArenaBytes,
                                                      .reader_epoch_lease = true},
                                           .storage_mode = glifistore::StorageMode::durable_sync,
                                           .data_directory = temporary.store_path(),
                                           .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("slot-v6-seed", bytes("seed")).has_value());

    auto initial_snapshot = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0);
    GLIFI_REQUIRE(initial_snapshot.has_value());
    auto initial_result = Generation::from_durable_snapshot(
        glifistore::detail::StoreAccess::worker_routing(store), initial_snapshot->records);
    GLIFI_REQUIRE(initial_result.has_value());
    auto initial = std::move(*initial_result);
    auto replacement_parent = initial;
    auto pool =
        Pool::create(std::move(initial), {.context = &store, .fail_closed = [](void* context) noexcept {
                                              glifistore::detail::StoreAccess::mark_fail_closed(
                                                  *static_cast<glifistore::Store*>(context));
                                          }});
    GLIFI_REQUIRE(pool.has_value());
    GLIFI_REQUIRE((*pool)->adopt() != nullptr);

    {
        auto reservation = (*pool)->try_reserve();
        GLIFI_REQUIRE(reservation.has_value());
        // This is the candidate ordering: capacity first, then Store entry.
        GLIFI_REQUIRE(store.put("slot-v6-committed", bytes("authority")).has_value());
        reservation->mark_store_linearized();
        // Deterministically model generation construction/publication failure.
        GLIFI_REQUIRE((*pool)->commit(*reservation, {}) ==
                      glifistore::experimental::GenerationSlotPublishStatus::invalid_generation);
    }
    GLIFI_REQUIRE(!glifistore::detail::StoreAccess::operational(store));
    GLIFI_REQUIRE((*pool)->stats().unpublished_linearizations == 1);
    GLIFI_REQUIRE((*pool)->stats().reserved_slots == 0);

    // Same recovery authority used by the production fail-closed epilogue:
    // snapshot is explicitly allowed after the durable catalog becomes sticky.
    auto drain = glifistore::detail::StoreAccess::snapshot_durable_reads(store, 0, true);
    GLIFI_REQUIRE(drain.has_value());
    auto drained_generation = Generation::replace_durable_snapshot(replacement_parent, drain->records);
    GLIFI_REQUIRE(drained_generation.has_value());
    replacement_parent.reset();
    initial_snapshot->records.clear();
    GLIFI_REQUIRE((*pool)->try_publish(std::move(*drained_generation)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    const auto* adopted = (*pool)->adopt();
    GLIFI_REQUIRE(adopted != nullptr);
    GLIFI_REQUIRE(
        adopted->prepare_durable({.key = "slot-v6-seed", .hash = glifistore::hash_key("slot-v6-seed")})
            .has_value());
    GLIFI_REQUIRE(
        adopted
            ->prepare_durable({.key = "slot-v6-committed", .hash = glifistore::hash_key("slot-v6-committed")})
            .has_value());
    GLIFI_REQUIRE(adopted->base_entries() == 2);
    GLIFI_REQUIRE((*pool)->stats().publications == 1);

    GLIFI_REQUIRE(store.close().has_value());
}
