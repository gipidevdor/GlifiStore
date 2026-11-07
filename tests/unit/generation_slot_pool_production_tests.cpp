#include "glifistore/store/paired/generation_slot_pool.hpp"
#include "test.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

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

struct ProductionPoolShutdown final {
    glifistore::store::paired::GenerationSlotPool* pool{};

    ~ProductionPoolShutdown() {
        if (pool == nullptr) {
            return;
        }
        pool->stop_admission();
        pool->revoke_publication();
        static_cast<void>(pool->mark_reader_quiescent());
        static_cast<void>(pool->try_finish_shutdown());
    }
};

} // namespace

GLIFI_TEST("ADR 0036 production slot V1 token adopt and reincarnation") {
    using Pool = glifistore::store::paired::GenerationSlotPool;
    using Status = glifistore::store::paired::GenerationSlotPublishStatus;
    const glifistore::WorkerRoutingState routing{};
    auto pool_result = Pool::create(routing);
    GLIFI_REQUIRE(pool_result.has_value());
    auto& pool = **pool_result;
    ProductionPoolShutdown guard{&pool};
    GLIFI_REQUIRE(pool.adopt() != nullptr);
    GLIFI_REQUIRE(Pool::kCapacity == 65U);

    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{201});
    const std::string key{"prod-slot-v1"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
    constexpr std::uint64_t kCycles = 10'000;
    for (std::uint64_t sequence = 1; sequence <= kCycles; ++sequence) {
        const glifistore::store::paired::ReadMutation mutation{
            .key = hashed,
            .record = append(*segment, routing, key, "value", sequence),
            .segment = segment,
            .opcode = glifistore::Opcode::put,
        };
        for (;;) {
            auto reservation = pool.try_reserve();
            if (!reservation) {
                static_cast<void>(pool.adopt());
                pool.reclaim(pool.stats().reader_safe_epoch == 0 ? 1 : pool.stats().reader_safe_epoch);
                continue;
            }
            reservation->mark_store_linearized();
            GLIFI_REQUIRE(pool.publish_incremental(*reservation, std::span{&mutation, 1}) ==
                          Status::published);
            break;
        }
        const auto token = pool.publication_token();
        GLIFI_REQUIRE(!token.empty());
        GLIFI_REQUIRE(token.epoch() == sequence);
        const auto* adopted = pool.adopt();
        GLIFI_REQUIRE(adopted != nullptr);
        GLIFI_REQUIRE(adopted->epoch() == sequence);
        GLIFI_REQUIRE(pool.decode_published(token) == adopted);
        pool.reclaim(sequence);
    }
    GLIFI_REQUIRE(pool.stats().slot_reuses > 0);
    GLIFI_REQUIRE(pool.stats().writer_epoch == kCycles);
}

GLIFI_TEST("ADR 0036 production slot V6 reserve-before-mutate fail-closed") {
    using Pool = glifistore::store::paired::GenerationSlotPool;
    using Status = glifistore::store::paired::GenerationSlotPublishStatus;
    std::atomic_uint64_t fail_closed_calls{};
    auto pool_result = Pool::create(
        {}, {}, {.context = &fail_closed_calls, .fail_closed = [](void* context) noexcept {
                     static_cast<std::atomic_uint64_t*>(context)->fetch_add(1U, std::memory_order_relaxed);
                 }});
    GLIFI_REQUIRE(pool_result.has_value());
    auto& pool = **pool_result;
    ProductionPoolShutdown guard{&pool};
    GLIFI_REQUIRE(pool.adopt() != nullptr);

    const std::string key{"prod-slot-v6"};
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
                  Status::invalid_generation);
    GLIFI_REQUIRE(fail_closed_calls.load(std::memory_order_relaxed) == 1);
    GLIFI_REQUIRE(pool.stats().unpublished_linearizations == 1);
}

GLIFI_TEST("ADR 0036 production slot V9 exhaustion backpressure") {
    using Pool = glifistore::store::paired::GenerationSlotPool;
    using Status = glifistore::store::paired::GenerationSlotPublishStatus;
    const glifistore::WorkerRoutingState routing{};
    auto pool_result = Pool::create(routing);
    GLIFI_REQUIRE(pool_result.has_value());
    auto& pool = **pool_result;
    ProductionPoolShutdown guard{&pool};
    GLIFI_REQUIRE(pool.adopt() != nullptr);

    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{202});
    const std::string key{"prod-slot-v9"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
    // Fill to capacity: 1 current + 64 retired = 65 live.
    for (std::uint64_t sequence = 1; sequence <= 64; ++sequence) {
        const glifistore::store::paired::ReadMutation mutation{
            .key = hashed,
            .record = append(*segment, routing, key, "value", sequence),
            .segment = segment,
            .opcode = glifistore::Opcode::put,
        };
        auto reservation = pool.try_reserve();
        GLIFI_REQUIRE(reservation.has_value());
        reservation->mark_store_linearized();
        GLIFI_REQUIRE(pool.publish_incremental(*reservation, std::span{&mutation, 1}) == Status::published);
    }
    GLIFI_REQUIRE(pool.stats().live_slots == 65);
    GLIFI_REQUIRE(!pool.try_reserve().has_value());
    GLIFI_REQUIRE(pool.stats().pool_exhaustions >= 1);
    const auto before_epoch = pool.stats().writer_epoch;
    GLIFI_REQUIRE(pool.adopt()->epoch() == before_epoch);
    pool.reclaim(before_epoch);
    GLIFI_REQUIRE(pool.stats().live_slots == 1);
    auto recovered = pool.try_reserve();
    GLIFI_REQUIRE(recovered.has_value());
    recovered->reset();
}

GLIFI_TEST("ADR 0036 production slot epoch overflow and token width") {
    using Token = glifistore::store::paired::GenerationPublicationToken;
    static_assert(Token::kSlotBits == 16U);
    static_assert(Token::kMaximumEpoch == (std::numeric_limits<std::uint64_t>::max() >> 16U));

    const auto token = Token::encode(Token::kMaximumEpoch, 64);
    GLIFI_REQUIRE(token.epoch() == Token::kMaximumEpoch);
    GLIFI_REQUIRE(token.slot_index() == 64U);
    GLIFI_REQUIRE(!token.empty());
    GLIFI_REQUIRE(Token{}.empty());
}
