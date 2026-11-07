#include "experimental/generation_slot_pool.hpp"
#include "experimental/pair_read_generation_shell.hpp"
#include "glifistore/store/paired/shard_pair_runtime.hpp"
#include "test.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

struct MockGeneration final {
    std::uint64_t generation_epoch{};
    std::uint64_t visible{};
    std::uint64_t marker{};

    [[nodiscard]] auto epoch() const noexcept -> std::uint64_t {
        return generation_epoch;
    }

    [[nodiscard]] auto visible_through() const noexcept -> std::uint64_t {
        return visible;
    }
};

[[nodiscard]] auto generation(const std::uint64_t epoch) -> std::shared_ptr<const MockGeneration> {
    return std::make_shared<const MockGeneration>(
        MockGeneration{.generation_epoch = epoch, .visible = epoch * 10U, .marker = epoch});
}

[[nodiscard]] auto bytes(const std::string_view text) noexcept -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

[[nodiscard]] auto append(glifistore::Segment& segment, const glifistore::WorkerRoutingState routing,
                          const std::string_view key, const std::string_view value,
                          const std::uint64_t sequence) -> glifistore::RecordRef {
    auto record = segment.append({.sequence = glifistore::SequenceNumber{sequence},
                                  .opcode = glifistore::Opcode::put,
                                  .key_hash = glifistore::hash_key_routing(key, routing),
                                  .key = bytes(key),
                                  .value = bytes(value)});
    GLIFI_REQUIRE(record.has_value());
    return *record;
}

template <typename Pool> struct DirectPoolShutdown final {
    Pool* pool{};

    ~DirectPoolShutdown() {
        if (pool != nullptr) {
            pool->stop_admission();
            static_cast<void>(pool->mark_reader_quiescent());
            static_cast<void>(pool->try_finish_shutdown());
        }
    }
};

} // namespace

GLIFI_TEST("ADR 0036 V8 candidate bounds slots and recovers after Reader adoption") {
    using Pool = glifistore::experimental::GenerationSlotPool<MockGeneration, 3>;
    auto pool = Pool::create(generation(0));
    GLIFI_REQUIRE(pool.has_value());
    GLIFI_REQUIRE((*pool)->adopt() != nullptr);

    GLIFI_REQUIRE((*pool)->try_publish(generation(1)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    GLIFI_REQUIRE((*pool)->try_publish(generation(2)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    GLIFI_REQUIRE((*pool)->try_publish(generation(3)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::pool_exhausted);
    GLIFI_REQUIRE((*pool)->stats().live_slots == 3);

    const auto* adopted = (*pool)->adopt();
    GLIFI_REQUIRE(adopted != nullptr);
    GLIFI_REQUIRE(adopted->epoch() == 2);
    (*pool)->reclaim();
    GLIFI_REQUIRE((*pool)->stats().live_slots == 1);
    GLIFI_REQUIRE((*pool)->try_publish(generation(3)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    GLIFI_REQUIRE((*pool)->adopt()->marker == 3);
    GLIFI_REQUIRE((*pool)->stats().slot_reuses > 0);
}

GLIFI_TEST("ADR 0036 V9 candidate applies the official retire-debt capacity formula") {
    constexpr auto kMaximumRetired =
        glifistore::store::paired::ShardPairRuntime::kMaximumRetiredReadGenerations;
    constexpr auto kCapacity = glifistore::experimental::GenerationSlotCapacity<kMaximumRetired>::value;
    static_assert(kMaximumRetired == 64U);
    static_assert(kCapacity == 65U);
    using Pool = glifistore::experimental::GenerationSlotPool<MockGeneration, kCapacity>;

    auto pool = Pool::create(generation(0));
    GLIFI_REQUIRE(pool.has_value());
    GLIFI_REQUIRE((*pool)->adopt() != nullptr);

    // Hold the initial Reader frontier while the Writer consumes the complete
    // normative retire-debt bound.
    for (std::uint64_t epoch = 1; epoch < kCapacity; ++epoch) {
        GLIFI_REQUIRE((*pool)->try_publish(generation(epoch)) ==
                      glifistore::experimental::GenerationSlotPublishStatus::published);
    }
    const auto saturated = (*pool)->stats();
    GLIFI_REQUIRE(saturated.live_slots == kCapacity);
    GLIFI_REQUIRE(saturated.live_high_watermark == kCapacity);
    GLIFI_REQUIRE(!(*pool)->try_reserve().has_value());
    GLIFI_REQUIRE((*pool)->stats().pool_exhaustions == 1);

    // No mutation crossed Store while full. Once the Reader advances, all old
    // epochs become reclaimable and the next reservation/publication succeeds.
    GLIFI_REQUIRE((*pool)->adopt()->epoch() == kCapacity - 1U);
    (*pool)->reclaim();
    GLIFI_REQUIRE((*pool)->stats().live_slots == 1);
    GLIFI_REQUIRE((*pool)->try_publish(generation(kCapacity)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    GLIFI_REQUIRE((*pool)->adopt()->epoch() == kCapacity);
}

GLIFI_TEST("ADR 0036 V8 candidate cold borrow holds the exact retired epoch") {
    using Pool = glifistore::experimental::GenerationSlotPool<MockGeneration, 4>;
    auto initial = generation(0);
    auto pool = Pool::create(initial);
    GLIFI_REQUIRE(pool.has_value());
    GLIFI_REQUIRE((*pool)->adopt() != nullptr);

    auto borrowed = generation(1);
    std::weak_ptr<const MockGeneration> borrowed_lifetime = borrowed;
    GLIFI_REQUIRE((*pool)->try_publish(borrowed) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    GLIFI_REQUIRE((*pool)->adopt() != nullptr);
    borrowed.reset();

    GLIFI_REQUIRE((*pool)->try_publish(generation(2)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    GLIFI_REQUIRE((*pool)->adopt(1)->epoch() == 2);
    (*pool)->reclaim();
    GLIFI_REQUIRE(!borrowed_lifetime.expired());
    GLIFI_REQUIRE((*pool)->stats().reader_safe_epoch == 1);

    GLIFI_REQUIRE((*pool)->adopt()->epoch() == 2);
    (*pool)->reclaim();
    GLIFI_REQUIRE(borrowed_lifetime.expired());
    GLIFI_REQUIRE((*pool)->stats().reader_safe_epoch == 2);
}

GLIFI_TEST("ADR 0036 V8 candidate rejects a late regressing borrow frontier") {
    using Pool = glifistore::experimental::GenerationSlotPool<MockGeneration, 4>;
    auto pool = Pool::create(generation(0));
    GLIFI_REQUIRE(pool.has_value());
    GLIFI_REQUIRE((*pool)->adopt() != nullptr);
    GLIFI_REQUIRE((*pool)->try_publish(generation(1)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    GLIFI_REQUIRE((*pool)->adopt()->epoch() == 1);
    GLIFI_REQUIRE((*pool)->try_publish(generation(2)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    GLIFI_REQUIRE((*pool)->adopt()->epoch() == 2);
    GLIFI_REQUIRE((*pool)->stats().reader_safe_epoch == 2);

    GLIFI_REQUIRE((*pool)->adopt(1) == nullptr);
    GLIFI_REQUIRE((*pool)->stats().reader_safe_epoch == 2);
    GLIFI_REQUIRE((*pool)->stats().reader_epoch == 2);
    GLIFI_REQUIRE((*pool)->stats().invalid_adoptions == 1);
}

GLIFI_TEST("ADR 0036 V6 candidate reserves before mutation and fail-closes abandoned authority") {
    using Pool = glifistore::experimental::GenerationSlotPool<MockGeneration, 2>;
    std::atomic_uint64_t fail_closed_calls{};
    auto pool = Pool::create(generation(0),
                             {.context = &fail_closed_calls, .fail_closed = [](void* context) noexcept {
                                  static_cast<std::atomic_uint64_t*>(context)->fetch_add(
                                      1U, std::memory_order_relaxed);
                              }});
    GLIFI_REQUIRE(pool.has_value());
    GLIFI_REQUIRE((*pool)->adopt() != nullptr);
    GLIFI_REQUIRE((*pool)->try_publish(generation(1)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);

    // Full before Store entry: reject without a fail-closed transition.
    GLIFI_REQUIRE(!(*pool)->try_reserve().has_value());
    GLIFI_REQUIRE(fail_closed_calls.load(std::memory_order_relaxed) == 0);
    GLIFI_REQUIRE((*pool)->stats().unpublished_linearizations == 0);

    GLIFI_REQUIRE((*pool)->adopt()->epoch() == 1);
    (*pool)->reclaim();
    {
        auto reservation = (*pool)->try_reserve();
        GLIFI_REQUIRE(reservation.has_value());
        // Cancellation before Store linearization is an ordinary safe abort.
    }
    GLIFI_REQUIRE(fail_closed_calls.load(std::memory_order_relaxed) == 0);

    {
        auto reservation = (*pool)->try_reserve();
        GLIFI_REQUIRE(reservation.has_value());
        reservation->mark_store_linearized();
        auto moved = std::move(*reservation);
        GLIFI_REQUIRE(moved.store_linearized());
        GLIFI_REQUIRE((*pool)->commit(moved, {}) ==
                      glifistore::experimental::GenerationSlotPublishStatus::invalid_generation);
        // The moved-to guard owns the transition; both destructors together
        // must invoke fail-closed exactly once.
    }
    const auto failed = (*pool)->stats();
    GLIFI_REQUIRE(fail_closed_calls.load(std::memory_order_relaxed) == 1);
    GLIFI_REQUIRE(failed.unpublished_linearizations == 1);
    GLIFI_REQUIRE(failed.reserved_slots == 0);

    auto recovery = (*pool)->try_reserve();
    GLIFI_REQUIRE(recovery.has_value());
    recovery->mark_store_linearized();
    GLIFI_REQUIRE((*pool)->commit(*recovery, generation(2)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    GLIFI_REQUIRE((*pool)->adopt()->epoch() == 2);
    GLIFI_REQUIRE(fail_closed_calls.load(std::memory_order_relaxed) == 1);
}

GLIFI_TEST("ADR 0036 V5 candidate drains admitted reservations and rejects late admission") {
    using Pool = glifistore::experimental::GenerationSlotPool<MockGeneration, 4>;
    auto pool = Pool::create(generation(0));
    GLIFI_REQUIRE(pool.has_value());
    GLIFI_REQUIRE((*pool)->adopt() != nullptr);

    auto cancelled = (*pool)->try_reserve();
    auto committed = (*pool)->try_reserve();
    GLIFI_REQUIRE(cancelled.has_value());
    GLIFI_REQUIRE(committed.has_value());

    (*pool)->stop_admission();
    GLIFI_REQUIRE(!(*pool)->accepting());
    GLIFI_REQUIRE(!(*pool)->try_reserve().has_value());
    GLIFI_REQUIRE(!(*pool)->mark_reader_quiescent());
    GLIFI_REQUIRE(!(*pool)->try_finish_shutdown());

    committed->mark_store_linearized();
    GLIFI_REQUIRE((*pool)->commit(*committed, generation(1)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    cancelled.reset();
    GLIFI_REQUIRE((*pool)->adopt()->epoch() == 1);
    GLIFI_REQUIRE(!(*pool)->try_finish_shutdown());
    GLIFI_REQUIRE((*pool)->mark_reader_quiescent());
    GLIFI_REQUIRE((*pool)->try_finish_shutdown());
    GLIFI_REQUIRE((*pool)->adopt() == nullptr);

    const auto stats = (*pool)->stats();
    GLIFI_REQUIRE(stats.shutdown_starts == 1);
    GLIFI_REQUIRE(stats.shutdown_reservation_rejections == 1);
    GLIFI_REQUIRE(stats.reserved_slots == 0);
    GLIFI_REQUIRE(stats.live_slots == 1);
    GLIFI_REQUIRE(!stats.accepting);
    GLIFI_REQUIRE(stats.reader_quiescent);
}

GLIFI_TEST("ADR 0036 V5 candidate holds a slow borrow until terminal Reader quiescence") {
    using Pool = glifistore::experimental::GenerationSlotPool<MockGeneration, 3>;
    auto initial = generation(0);
    std::weak_ptr<const MockGeneration> initial_lifetime = initial;
    auto pool = Pool::create(std::move(initial));
    GLIFI_REQUIRE(pool.has_value());
    GLIFI_REQUIRE((*pool)->adopt() != nullptr);
    GLIFI_REQUIRE((*pool)->try_publish(generation(1)) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    GLIFI_REQUIRE((*pool)->adopt(0)->epoch() == 1);

    (*pool)->stop_admission();
    (*pool)->reclaim();
    GLIFI_REQUIRE(!initial_lifetime.expired());
    GLIFI_REQUIRE(!(*pool)->try_finish_shutdown());

    // The caller reaches this transition only after the slow output/cold I/O
    // represented by epoch zero has completed.
    GLIFI_REQUIRE((*pool)->mark_reader_quiescent());
    GLIFI_REQUIRE((*pool)->try_finish_shutdown());
    GLIFI_REQUIRE(initial_lifetime.expired());
    GLIFI_REQUIRE((*pool)->stats().live_slots == 1);
}

GLIFI_TEST("ADR 0036 V5 candidate fail-closes a linearized reservation abandoned by shutdown") {
    using Pool = glifistore::experimental::GenerationSlotPool<MockGeneration, 2>;
    std::atomic_uint64_t fail_closed_calls{};
    auto pool = Pool::create(generation(0),
                             {.context = &fail_closed_calls, .fail_closed = [](void* context) noexcept {
                                  static_cast<std::atomic_uint64_t*>(context)->fetch_add(
                                      1U, std::memory_order_relaxed);
                              }});
    GLIFI_REQUIRE(pool.has_value());
    GLIFI_REQUIRE((*pool)->adopt() != nullptr);

    auto reservation = (*pool)->try_reserve();
    GLIFI_REQUIRE(reservation.has_value());
    reservation->mark_store_linearized();
    (*pool)->stop_admission();
    GLIFI_REQUIRE(!(*pool)->mark_reader_quiescent());
    reservation.reset();

    GLIFI_REQUIRE(fail_closed_calls.load(std::memory_order_relaxed) == 1);
    GLIFI_REQUIRE((*pool)->stats().unpublished_linearizations == 1);
    GLIFI_REQUIRE((*pool)->mark_reader_quiescent());
    GLIFI_REQUIRE((*pool)->try_finish_shutdown());
}

GLIFI_TEST("ADR 0036 V13 candidate publish adopt reclaim stress is race free") {
    using Pool = glifistore::experimental::GenerationSlotPool<MockGeneration, 8>;
    auto pool = Pool::create(generation(0));
    GLIFI_REQUIRE(pool.has_value());

    constexpr std::uint64_t kPublications = 20'000;
    std::atomic_bool writer_done{};
    std::atomic_bool failed{};
    std::thread writer([&] {
        for (std::uint64_t epoch = 1; epoch <= kPublications; ++epoch) {
            const auto next = generation(epoch);
            for (;;) {
                const auto status = (*pool)->try_publish(next);
                if (status == glifistore::experimental::GenerationSlotPublishStatus::published) {
                    break;
                }
                if (status != glifistore::experimental::GenerationSlotPublishStatus::pool_exhausted) {
                    failed.store(true, std::memory_order_relaxed);
                    writer_done.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
            }
        }
        writer_done.store(true, std::memory_order_release);
    });

    while (!writer_done.load(std::memory_order_acquire) || (*pool)->stats().reader_epoch < kPublications) {
        const auto* adopted = (*pool)->adopt();
        if (adopted == nullptr || adopted->marker != adopted->epoch() ||
            adopted->visible_through() != adopted->epoch() * 10U) {
            failed.store(true, std::memory_order_relaxed);
        }
        std::this_thread::yield();
    }
    writer.join();
    (*pool)->reclaim();

    GLIFI_REQUIRE(!failed.load(std::memory_order_relaxed));
    GLIFI_REQUIRE((*pool)->stats().writer_epoch == kPublications);
    GLIFI_REQUIRE((*pool)->stats().reader_epoch == kPublications);
    GLIFI_REQUIRE((*pool)->stats().slot_reuses > 0);
    GLIFI_REQUIRE((*pool)->stats().live_high_watermark <= 8);
}

GLIFI_TEST("ADR 0036 shell slot reuses one real generation allocation after weak retirement") {
    using Access = glifistore::experimental::PairReadGenerationShellAccess;
    using Generation = glifistore::store::paired::PairReadGeneration;
    const glifistore::WorkerRoutingState routing{};
    auto initial_result = Generation::empty(routing);
    auto storage_result = glifistore::experimental::PairReadGenerationShellStorage::create();
    GLIFI_REQUIRE(initial_result.has_value());
    GLIFI_REQUIRE(storage_result.has_value());
    auto initial = *initial_result;
    auto storage = *storage_result;
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{91});
    const std::string key{"fixed-shell"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
    const glifistore::store::paired::ReadMutation mutation{
        .key = hashed,
        .record = append(*segment, routing, key, "one", 1),
        .segment = segment,
        .opcode = glifistore::Opcode::put,
    };

    const Generation* first_address{};
    std::weak_ptr<const Generation> weak_generation;
    {
        auto first = Access::publish_incremental(initial, std::span{&mutation, 1}, storage);
        GLIFI_REQUIRE(first.has_value());
        first_address = first->get();
        weak_generation = *first;
        GLIFI_REQUIRE(storage->occupied());
        GLIFI_REQUIRE(storage->allocation_count() == 1);
        GLIFI_REQUIRE((*first)->get(hashed, 0).has_value());

        auto rejected = Access::publish_incremental(*first, std::span{&mutation, 1}, storage);
        GLIFI_REQUIRE(!rejected.has_value());
        GLIFI_REQUIRE(rejected.error().code == glifistore::ErrorCode::resource_exhausted);
        GLIFI_REQUIRE(storage->allocation_count() == 1);
    }

    // allocate_shared retains the block until the final weak owner releases
    // its control block, not merely until the final strong owner disappears.
    GLIFI_REQUIRE(weak_generation.expired());
    GLIFI_REQUIRE(storage->occupied());
    weak_generation.reset();
    GLIFI_REQUIRE(!storage->occupied());

    auto second = Access::publish_incremental(initial, std::span{&mutation, 1}, storage);
    GLIFI_REQUIRE(second.has_value());
    GLIFI_REQUIRE(second->get() == first_address);
    GLIFI_REQUIRE(storage->allocation_count() == 2);
    GLIFI_REQUIRE(storage->reuse_count() == 1);
}

GLIFI_TEST("ADR 0036 shell slot backing storage outlives its external owner") {
    using Access = glifistore::experimental::PairReadGenerationShellAccess;
    using Generation = glifistore::store::paired::PairReadGeneration;
    const glifistore::WorkerRoutingState routing{};
    auto initial_result = Generation::empty(routing);
    auto storage_result = glifistore::experimental::PairReadGenerationShellStorage::create();
    GLIFI_REQUIRE(initial_result.has_value());
    GLIFI_REQUIRE(storage_result.has_value());
    auto storage = *storage_result;
    std::weak_ptr<glifistore::experimental::PairReadGenerationShellStorage> lifetime = storage;
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{92});
    const std::string key{"owned-shell"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
    const glifistore::store::paired::ReadMutation mutation{
        .key = hashed,
        .record = append(*segment, routing, key, "value", 1),
        .segment = segment,
        .opcode = glifistore::Opcode::put,
    };

    auto published = Access::publish_incremental(*initial_result, std::span{&mutation, 1}, storage);
    GLIFI_REQUIRE(published.has_value());
    storage_result = glifistore::fail(glifistore::ErrorCode::internal_error, "released");
    storage.reset();
    GLIFI_REQUIRE(!lifetime.expired());
    GLIFI_REQUIRE((*published)->get(hashed, 0).has_value());

    published = glifistore::fail(glifistore::ErrorCode::internal_error, "released");
    GLIFI_REQUIRE(lifetime.expired());
}

GLIFI_TEST("ADR 0036 real generation pool reserves and reuses its matching shell slot") {
    using Access = glifistore::experimental::PairReadGenerationShellAccess;
    using Generation = glifistore::store::paired::PairReadGeneration;
    using Pool = glifistore::experimental::GenerationSlotPool<Generation, 3>;
    using Bank = glifistore::experimental::PairReadGenerationShellBank<3>;
    const glifistore::WorkerRoutingState routing{};
    auto initial_result = Generation::empty(routing);
    auto bank_result = Bank::create();
    GLIFI_REQUIRE(initial_result.has_value());
    GLIFI_REQUIRE(bank_result.has_value());
    auto pool_result = Pool::create(*initial_result);
    GLIFI_REQUIRE(pool_result.has_value());
    auto& pool = **pool_result;
    auto& bank = **bank_result;
    GLIFI_REQUIRE(pool.adopt() != nullptr);

    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{93});
    const std::string key{"pooled-shell"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
    const glifistore::store::paired::ReadMutation first_mutation{
        .key = hashed,
        .record = append(*segment, routing, key, "one", 1),
        .segment = segment,
        .opcode = glifistore::Opcode::put,
    };

    auto first_reservation = pool.try_reserve();
    GLIFI_REQUIRE(first_reservation.has_value());
    const auto first_slot = first_reservation->slot_index();
    auto first_storage = bank.at(first_slot);
    auto first = Access::publish_incremental(*initial_result, std::span{&first_mutation, 1}, first_storage);
    GLIFI_REQUIRE(first.has_value());
    auto writer_current = *first;
    first_reservation->mark_store_linearized();
    GLIFI_REQUIRE(pool.commit(*first_reservation, writer_current) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    first = glifistore::fail(glifistore::ErrorCode::internal_error, "moved to pool");
    GLIFI_REQUIRE(first_storage->allocation_count() == 1);
    GLIFI_REQUIRE(pool.adopt()->epoch() == 1);
    pool.reclaim();

    const glifistore::store::paired::ReadMutation second_mutation{
        .key = hashed,
        .record = append(*segment, routing, key, "two", 2),
        .segment = segment,
        .opcode = glifistore::Opcode::put,
    };
    auto second_reservation = pool.try_reserve();
    GLIFI_REQUIRE(second_reservation.has_value());
    auto second_storage = bank.at(second_reservation->slot_index());
    auto second = Access::publish_incremental(writer_current, std::span{&second_mutation, 1}, second_storage);
    GLIFI_REQUIRE(second.has_value());
    writer_current = *second;
    second_reservation->mark_store_linearized();
    GLIFI_REQUIRE(pool.commit(*second_reservation, writer_current) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    second = glifistore::fail(glifistore::ErrorCode::internal_error, "moved to pool");
    GLIFI_REQUIRE(pool.adopt()->epoch() == 2);
    auto found = pool.reader_generation()->get(hashed, 0);
    GLIFI_REQUIRE(found.has_value());

    pool.reclaim();
    auto reuse_reservation = pool.try_reserve();
    GLIFI_REQUIRE(reuse_reservation.has_value());
    const auto reuse_slot = reuse_reservation->slot_index();
    auto reuse_storage = bank.at(reuse_slot);
    GLIFI_REQUIRE(reuse_slot == first_slot);
    GLIFI_REQUIRE(!reuse_storage->occupied());
    const glifistore::store::paired::ReadMutation third_mutation{
        .key = hashed,
        .record = append(*segment, routing, key, "three", 3),
        .segment = segment,
        .opcode = glifistore::Opcode::put,
    };
    auto third = Access::publish_incremental(writer_current, std::span{&third_mutation, 1}, reuse_storage);
    GLIFI_REQUIRE(third.has_value());
    GLIFI_REQUIRE(reuse_storage->allocation_count() == 2);
    GLIFI_REQUIRE(reuse_storage->reuse_count() == 1);
}

GLIFI_TEST("ADR 0036 inline slot owner publishes without shared backing ownership") {
    using Generation = glifistore::store::paired::PairReadGeneration;
    using Pool = glifistore::experimental::PairReadGenerationInlineSlotPool<2>;
    const glifistore::WorkerRoutingState routing{};
    auto initial = Generation::empty(routing);
    GLIFI_REQUIRE(initial.has_value());
    auto pool_result = Pool::create(*initial);
    GLIFI_REQUIRE(pool_result.has_value());
    auto& pool = **pool_result;
    GLIFI_REQUIRE(pool.adopt() != nullptr);

    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{94});
    const std::string key{"inline-shell"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
    for (std::uint64_t sequence = 1; sequence <= 128; ++sequence) {
        const glifistore::store::paired::ReadMutation mutation{
            .key = hashed,
            .record = append(*segment, routing, key, "value", sequence),
            .segment = segment,
            .opcode = glifistore::Opcode::put,
        };
        auto reservation = pool.try_reserve();
        GLIFI_REQUIRE(reservation.has_value());
        reservation->mark_store_linearized();
        GLIFI_REQUIRE(pool.publish_incremental(*reservation, std::span{&mutation, 1}) ==
                      glifistore::experimental::GenerationSlotPublishStatus::published);
        const auto* adopted = pool.adopt();
        GLIFI_REQUIRE(adopted != nullptr);
        GLIFI_REQUIRE(adopted->epoch() == sequence);
        pool.reclaim();
    }

    const auto stats = pool.stats();
    GLIFI_REQUIRE(stats.publications == 128);
    GLIFI_REQUIRE(stats.live_slots == 1);
    GLIFI_REQUIRE(pool.shell_allocation_count(0) == 64);
    GLIFI_REQUIRE(pool.shell_allocation_count(1) == 64);
    GLIFI_REQUIRE(pool.shell_reuse_count(0) == 63);
    GLIFI_REQUIRE(pool.shell_reuse_count(1) == 63);
    const auto found = pool.adopt()->get(hashed, 0);
    GLIFI_REQUIRE(found.has_value());
}

GLIFI_TEST("ADR 0036 inline slot owner fail-closes a rejected post-linearization build") {
    using Generation = glifistore::store::paired::PairReadGeneration;
    using Pool = glifistore::experimental::PairReadGenerationInlineSlotPool<2>;
    std::atomic_uint64_t fail_closed_calls{};
    auto initial = Generation::empty({});
    GLIFI_REQUIRE(initial.has_value());
    auto pool_result = Pool::create(
        *initial, {.context = &fail_closed_calls, .fail_closed = [](void* context) noexcept {
                       static_cast<std::atomic_uint64_t*>(context)->fetch_add(1U, std::memory_order_relaxed);
                   }});
    GLIFI_REQUIRE(pool_result.has_value());
    auto& pool = **pool_result;
    GLIFI_REQUIRE(pool.adopt() != nullptr);

    const std::string key{"invalid-inline-shell"};
    const glifistore::store::paired::ReadMutation invalid{
        .key = {key, glifistore::hash_key_routing(key, {})},
        .record = {},
        .segment = {},
        .opcode = glifistore::Opcode::put,
    };
    auto reservation = pool.try_reserve();
    GLIFI_REQUIRE(reservation.has_value());
    reservation->mark_store_linearized();
    GLIFI_REQUIRE(pool.publish_incremental(*reservation, std::span{&invalid, 1}) ==
                  glifistore::experimental::GenerationSlotPublishStatus::invalid_generation);
    GLIFI_REQUIRE(fail_closed_calls.load(std::memory_order_relaxed) == 1);
    GLIFI_REQUIRE(pool.stats().unpublished_linearizations == 1);
    GLIFI_REQUIRE(pool.stats().reserved_slots == 0);
    GLIFI_REQUIRE(pool.shell_allocation_count(1) == 0);
}

GLIFI_TEST("ADR 0036 inline slot owner publish adopt reclaim stress is race free") {
    using Generation = glifistore::store::paired::PairReadGeneration;
    using Pool = glifistore::experimental::PairReadGenerationInlineSlotPool<8>;
    const glifistore::WorkerRoutingState routing{};
    auto initial = Generation::empty(routing);
    GLIFI_REQUIRE(initial.has_value());
    auto pool_result = Pool::create(*initial);
    GLIFI_REQUIRE(pool_result.has_value());
    auto& pool = **pool_result;
    GLIFI_REQUIRE(pool.adopt() != nullptr);

    constexpr std::uint64_t kPublications = 10'000;
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{95});
    const std::string key{"inline-shell-stress"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
    std::vector<glifistore::RecordRef> records;
    records.reserve(kPublications);
    for (std::uint64_t sequence = 1; sequence <= kPublications; ++sequence) {
        records.push_back(append(*segment, routing, key, "value", sequence));
    }

    std::atomic_bool writer_done{};
    std::atomic_bool failed{};
    std::thread writer([&] {
        for (std::uint64_t sequence = 1; sequence <= kPublications; ++sequence) {
            const glifistore::store::paired::ReadMutation mutation{
                .key = hashed,
                .record = records[sequence - 1U],
                .segment = segment,
                .opcode = glifistore::Opcode::put,
            };
            for (;;) {
                auto reservation = pool.try_reserve();
                if (!reservation) {
                    std::this_thread::yield();
                    continue;
                }
                reservation->mark_store_linearized();
                if (pool.publish_incremental(*reservation, std::span{&mutation, 1}) !=
                    glifistore::experimental::GenerationSlotPublishStatus::published) {
                    failed.store(true, std::memory_order_relaxed);
                    writer_done.store(true, std::memory_order_release);
                    return;
                }
                break;
            }
        }
        writer_done.store(true, std::memory_order_release);
    });

    while (!writer_done.load(std::memory_order_acquire) || pool.stats().reader_epoch < kPublications) {
        const auto* adopted = pool.adopt();
        if (adopted == nullptr || adopted->visible_through() != adopted->epoch()) {
            failed.store(true, std::memory_order_relaxed);
        }
        std::this_thread::yield();
    }
    writer.join();

    GLIFI_REQUIRE(!failed.load(std::memory_order_relaxed));
    GLIFI_REQUIRE(pool.stats().writer_epoch == kPublications);
    GLIFI_REQUIRE(pool.stats().reader_epoch == kPublications);
    const auto found = pool.adopt()->get(hashed, 0);
    GLIFI_REQUIRE(found.has_value());
}

GLIFI_TEST("ADR 0036 direct generation ring matches official incremental publication") {
    using Generation = glifistore::store::paired::PairReadGeneration;
    using Ring = glifistore::experimental::PairReadGenerationDirectRing<2>;
    const glifistore::WorkerRoutingState routing{};
    auto initial = Generation::empty(routing);
    GLIFI_REQUIRE(initial.has_value());
    auto official = *initial;
    Ring ring{*initial};
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{96});
    const std::string key{"direct-generation"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};

    for (std::uint64_t sequence = 1; sequence <= 256; ++sequence) {
        const auto value = std::to_string(sequence);
        const glifistore::store::paired::ReadMutation mutation{
            .key = hashed,
            .record = append(*segment, routing, key, value, sequence),
            .segment = segment,
            .opcode = glifistore::Opcode::put,
        };
        auto next_official = Generation::publish_incremental(official, std::span{&mutation, 1});
        auto next_direct = ring.publish(std::span{&mutation, 1});
        GLIFI_REQUIRE(next_official.has_value());
        GLIFI_REQUIRE(next_direct.has_value());
        official = *next_official;
        GLIFI_REQUIRE((*next_direct)->epoch() == official->epoch());
        GLIFI_REQUIRE((*next_direct)->visible_through() == official->visible_through());
        GLIFI_REQUIRE((*next_direct)->delta_entries() == official->delta_entries());
        GLIFI_REQUIRE((*next_direct)->delta_record_versions() == official->delta_record_versions());
        const auto official_value = official->get(hashed, 0);
        const auto direct_value = (*next_direct)->get(hashed, 0);
        GLIFI_REQUIRE(official_value.has_value());
        GLIFI_REQUIRE(direct_value.has_value());
        GLIFI_REQUIRE(official_value->bytes == direct_value->bytes);
    }

    GLIFI_REQUIRE(ring.allocation_count(0) == 128);
    GLIFI_REQUIRE(ring.allocation_count(1) == 128);
    GLIFI_REQUIRE(ring.reuse_count(0) == 127);
    GLIFI_REQUIRE(ring.reuse_count(1) == 127);
}

GLIFI_TEST("ADR 0036 direct generation ring preserves current after rejected build") {
    using Generation = glifistore::store::paired::PairReadGeneration;
    using Ring = glifistore::experimental::PairReadGenerationDirectRing<2>;
    const glifistore::WorkerRoutingState routing{};
    auto initial = Generation::empty(routing);
    GLIFI_REQUIRE(initial.has_value());
    Ring ring{*initial};
    const auto* before = ring.current();
    const std::string key{"direct-invalid"};
    const glifistore::store::paired::ReadMutation invalid{
        .key = {key, glifistore::hash_key_routing(key, routing)},
        .record = {},
        .segment = {},
        .opcode = glifistore::Opcode::put,
    };
    auto rejected = ring.publish(std::span{&invalid, 1});
    GLIFI_REQUIRE(!rejected.has_value());
    GLIFI_REQUIRE(rejected.error().code == glifistore::ErrorCode::invalid_reference);
    GLIFI_REQUIRE(ring.current() == before);
    GLIFI_REQUIRE(ring.allocation_count(0) == 0);

    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{97});
    const glifistore::store::paired::ReadMutation valid{
        .key = invalid.key,
        .record = append(*segment, routing, key, "valid", 1),
        .segment = segment,
        .opcode = glifistore::Opcode::put,
    };
    auto published = ring.publish(std::span{&valid, 1});
    GLIFI_REQUIRE(published.has_value());
    GLIFI_REQUIRE((*published)->get(invalid.key, 0).has_value());
    GLIFI_REQUIRE(ring.allocation_count(0) == 1);
}

GLIFI_TEST("ADR 0036 direct slot pool bounds debt and reuses reclaimed object storage") {
    using Pool = glifistore::experimental::PairReadGenerationDirectSlotPool<3>;
    const glifistore::WorkerRoutingState routing{};
    auto pool_result = Pool::create(routing);
    GLIFI_REQUIRE(pool_result.has_value());
    auto& pool = **pool_result;
    DirectPoolShutdown guard{&pool};
    GLIFI_REQUIRE(pool.adopt() != nullptr);

    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{98});
    const std::string key{"direct-pool-bounded"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
        const glifistore::store::paired::ReadMutation mutation{
            .key = hashed,
            .record = append(*segment, routing, key, "value", sequence),
            .segment = segment,
            .opcode = glifistore::Opcode::put,
        };
        auto reservation = pool.try_reserve();
        GLIFI_REQUIRE(reservation.has_value());
        reservation->mark_store_linearized();
        GLIFI_REQUIRE(pool.publish_incremental(*reservation, std::span{&mutation, 1}) ==
                      glifistore::experimental::GenerationSlotPublishStatus::published);
    }
    GLIFI_REQUIRE(!pool.try_reserve().has_value());
    GLIFI_REQUIRE(pool.stats().live_slots == 3);
    GLIFI_REQUIRE(pool.stats().live_high_watermark == 3);

    GLIFI_REQUIRE(pool.adopt()->epoch() == 2);
    pool.reclaim();
    GLIFI_REQUIRE(pool.stats().live_slots == 1);
    const glifistore::store::paired::ReadMutation third{
        .key = hashed,
        .record = append(*segment, routing, key, "three", 3),
        .segment = segment,
        .opcode = glifistore::Opcode::put,
    };
    auto reservation = pool.try_reserve();
    GLIFI_REQUIRE(reservation.has_value());
    const auto reused_slot = reservation->slot_index();
    reservation->mark_store_linearized();
    GLIFI_REQUIRE(pool.publish_incremental(*reservation, std::span{&third, 1}) ==
                  glifistore::experimental::GenerationSlotPublishStatus::published);
    const auto* adopted = pool.adopt();
    GLIFI_REQUIRE(adopted != nullptr);
    GLIFI_REQUIRE(adopted->epoch() == 3);
    GLIFI_REQUIRE(adopted->get(hashed, 0).has_value());
    GLIFI_REQUIRE(pool.shell_reuse_count(reused_slot) > 0);
    GLIFI_REQUIRE(pool.stats().slot_reuses > 0);
}

GLIFI_TEST("ADR 0036 direct slot pool holds a cold borrow frontier") {
    using Pool = glifistore::experimental::PairReadGenerationDirectSlotPool<4>;
    const glifistore::WorkerRoutingState routing{};
    auto pool_result = Pool::create(routing);
    GLIFI_REQUIRE(pool_result.has_value());
    auto& pool = **pool_result;
    DirectPoolShutdown guard{&pool};
    GLIFI_REQUIRE(pool.adopt() != nullptr);
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{99});
    const std::string key{"direct-pool-borrow"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};

    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
        const glifistore::store::paired::ReadMutation mutation{
            .key = hashed,
            .record = append(*segment, routing, key, "value", sequence),
            .segment = segment,
            .opcode = glifistore::Opcode::put,
        };
        auto reservation = pool.try_reserve();
        GLIFI_REQUIRE(reservation.has_value());
        reservation->mark_store_linearized();
        GLIFI_REQUIRE(pool.publish_incremental(*reservation, std::span{&mutation, 1}) ==
                      glifistore::experimental::GenerationSlotPublishStatus::published);
        if (sequence == 1) {
            GLIFI_REQUIRE(pool.adopt()->epoch() == 1);
        }
    }
    GLIFI_REQUIRE(pool.adopt(1)->epoch() == 2);
    pool.reclaim();
    GLIFI_REQUIRE(pool.stats().reader_safe_epoch == 1);
    GLIFI_REQUIRE(pool.stats().live_slots == 2);

    GLIFI_REQUIRE(pool.adopt()->epoch() == 2);
    pool.reclaim();
    GLIFI_REQUIRE(pool.stats().reader_safe_epoch == 2);
    GLIFI_REQUIRE(pool.stats().live_slots == 1);
    GLIFI_REQUIRE(pool.adopt(1) == nullptr);
}

GLIFI_TEST("ADR 0036 direct slot pool fail-closes a rejected linearized build") {
    using Pool = glifistore::experimental::PairReadGenerationDirectSlotPool<2>;
    std::atomic_uint64_t fail_closed_calls{};
    auto pool_result = Pool::create(
        {}, {.context = &fail_closed_calls, .fail_closed = [](void* context) noexcept {
                 static_cast<std::atomic_uint64_t*>(context)->fetch_add(1U, std::memory_order_relaxed);
             }});
    GLIFI_REQUIRE(pool_result.has_value());
    auto& pool = **pool_result;
    DirectPoolShutdown guard{&pool};
    GLIFI_REQUIRE(pool.adopt() != nullptr);

    const std::string key{"direct-pool-invalid"};
    const glifistore::store::paired::ReadMutation invalid{
        .key = {key, glifistore::hash_key_routing(key, {})},
        .record = {},
        .segment = {},
        .opcode = glifistore::Opcode::put,
    };
    auto reservation = pool.try_reserve();
    GLIFI_REQUIRE(reservation.has_value());
    const auto slot = reservation->slot_index();
    reservation->mark_store_linearized();
    GLIFI_REQUIRE(pool.publish_incremental(*reservation, std::span{&invalid, 1}) ==
                  glifistore::experimental::GenerationSlotPublishStatus::invalid_generation);
    GLIFI_REQUIRE(fail_closed_calls.load(std::memory_order_relaxed) == 1);
    GLIFI_REQUIRE(pool.stats().unpublished_linearizations == 1);
    GLIFI_REQUIRE(pool.stats().reserved_slots == 0);
    GLIFI_REQUIRE(pool.shell_allocation_count(slot) == 0);
}

GLIFI_TEST("ADR 0036 direct slot pool shutdown rejects late admission and revokes adoption") {
    using Pool = glifistore::experimental::PairReadGenerationDirectSlotPool<3>;
    auto pool_result = Pool::create({});
    GLIFI_REQUIRE(pool_result.has_value());
    auto& pool = **pool_result;
    DirectPoolShutdown guard{&pool};
    GLIFI_REQUIRE(pool.adopt() != nullptr);

    auto reservation = pool.try_reserve();
    GLIFI_REQUIRE(reservation.has_value());
    pool.stop_admission();
    GLIFI_REQUIRE(!pool.accepting());
    GLIFI_REQUIRE(!pool.try_reserve().has_value());
    GLIFI_REQUIRE(!pool.mark_reader_quiescent());
    GLIFI_REQUIRE(!pool.try_finish_shutdown());
    reservation.reset();
    GLIFI_REQUIRE(pool.mark_reader_quiescent());
    GLIFI_REQUIRE(pool.try_finish_shutdown());
    GLIFI_REQUIRE(pool.adopt() == nullptr);

    const auto stats = pool.stats();
    GLIFI_REQUIRE(stats.shutdown_starts == 1);
    GLIFI_REQUIRE(stats.shutdown_reservation_rejections == 1);
    GLIFI_REQUIRE(stats.reservation_cancellations == 1);
    GLIFI_REQUIRE(stats.live_slots == 1);
    GLIFI_REQUIRE(stats.reader_quiescent);
}

GLIFI_TEST("ADR 0036 direct slot pool publish adopt reclaim stress is race free") {
    using Pool = glifistore::experimental::PairReadGenerationDirectSlotPool<8>;
    const glifistore::WorkerRoutingState routing{};
    auto pool_result = Pool::create(routing);
    GLIFI_REQUIRE(pool_result.has_value());
    auto& pool = **pool_result;
    DirectPoolShutdown guard{&pool};
    GLIFI_REQUIRE(pool.adopt() != nullptr);

    constexpr std::uint64_t kPublications = 10'000;
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{100});
    const std::string key{"direct-pool-stress"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
    std::vector<glifistore::RecordRef> records;
    records.reserve(kPublications);
    for (std::uint64_t sequence = 1; sequence <= kPublications; ++sequence) {
        records.push_back(append(*segment, routing, key, "value", sequence));
    }

    std::atomic_bool writer_done{};
    std::atomic_bool failed{};
    std::thread writer([&] {
        for (std::uint64_t sequence = 1; sequence <= kPublications; ++sequence) {
            const glifistore::store::paired::ReadMutation mutation{
                .key = hashed,
                .record = records[sequence - 1U],
                .segment = segment,
                .opcode = glifistore::Opcode::put,
            };
            for (;;) {
                auto reservation = pool.try_reserve();
                if (!reservation) {
                    std::this_thread::yield();
                    continue;
                }
                reservation->mark_store_linearized();
                if (pool.publish_incremental(*reservation, std::span{&mutation, 1}) !=
                    glifistore::experimental::GenerationSlotPublishStatus::published) {
                    failed.store(true, std::memory_order_relaxed);
                    writer_done.store(true, std::memory_order_release);
                    return;
                }
                break;
            }
        }
        writer_done.store(true, std::memory_order_release);
    });

    while (!writer_done.load(std::memory_order_acquire) || pool.stats().reader_epoch < kPublications) {
        const auto* adopted = pool.adopt();
        if (adopted == nullptr || adopted->visible_through() != adopted->epoch()) {
            failed.store(true, std::memory_order_relaxed);
        }
        std::this_thread::yield();
    }
    writer.join();
    pool.reclaim();

    GLIFI_REQUIRE(!failed.load(std::memory_order_relaxed));
    GLIFI_REQUIRE(pool.stats().writer_epoch == kPublications);
    GLIFI_REQUIRE(pool.stats().reader_epoch == kPublications);
    GLIFI_REQUIRE(pool.stats().live_high_watermark <= 8);
    GLIFI_REQUIRE(pool.stats().slot_reuses > 0);
    GLIFI_REQUIRE(pool.adopt()->get(hashed, 0).has_value());
}
