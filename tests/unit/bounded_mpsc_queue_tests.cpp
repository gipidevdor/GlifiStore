#include "glifistore/server/bounded_mpsc_queue.hpp"
#include "glifistore/server/bounded_spsc_queue.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

GLIFI_TEST("bounded MPSC queue reports capacity without losing values") {
    glifistore::server::BoundedMpscQueue<std::size_t> queue{2};
    GLIFI_REQUIRE(queue.capacity() == 2);
    GLIFI_REQUIRE(queue.try_push(std::size_t{10}));
    GLIFI_REQUIRE(queue.try_push(std::size_t{20}));
    GLIFI_REQUIRE(!queue.try_push(std::size_t{30}));
    const auto first = queue.try_pop();
    const auto second = queue.try_pop();
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE(second.has_value());
    GLIFI_REQUIRE(*first == 10);
    GLIFI_REQUIRE(*second == 20);
    GLIFI_REQUIRE(!queue.try_pop().has_value());
}

GLIFI_TEST("bounded MPSC modular sequence ordering is defined across size_t wrap") {
    using glifistore::server::mpsc_detail::sequence_precedes_position;
    constexpr auto kMax = std::numeric_limits<std::size_t>::max();
    GLIFI_REQUIRE(sequence_precedes_position(kMax, 0U));
    GLIFI_REQUIRE(sequence_precedes_position(kMax - 3U, 2U));
    GLIFI_REQUIRE(!sequence_precedes_position(0U, kMax));
    GLIFI_REQUIRE(!sequence_precedes_position(3U, kMax));
    GLIFI_REQUIRE(!sequence_precedes_position(42U, 42U));

    bool rejected{};
    try {
        glifistore::server::BoundedMpscQueue<std::size_t> impossible{
            glifistore::server::mpsc_detail::kMaximumCapacity + 1U};
        static_cast<void>(impossible);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    GLIFI_REQUIRE(rejected);
}

GLIFI_TEST("bounded MPSC queue serializes concurrent producers") {
    constexpr std::size_t producer_count = 4;
    constexpr std::size_t values_per_producer = 1000;
    constexpr std::size_t value_count = producer_count * values_per_producer;
    glifistore::server::BoundedMpscQueue<std::size_t> queue{64};
    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            for (std::size_t offset = 0; offset < values_per_producer; ++offset) {
                const auto value = producer * values_per_producer + offset;
                while (!queue.try_push(std::size_t{value})) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::vector<bool> observed(value_count);
    std::size_t received{};
    while (received < value_count) {
        auto value = queue.try_pop();
        if (!value) {
            std::this_thread::yield();
            continue;
        }
        GLIFI_REQUIRE(*value < observed.size());
        GLIFI_REQUIRE(!observed[*value]);
        observed[*value] = true;
        ++received;
    }
    for (auto& producer : producers) {
        producer.join();
    }
    GLIFI_REQUIRE(received == value_count);
}

GLIFI_TEST("bounded SPSC queue preserves FIFO order across concurrent wraparound") {
    constexpr std::size_t value_count = 100'000;
    glifistore::server::BoundedSpscQueue<std::size_t> queue{17};
    GLIFI_REQUIRE(queue.capacity() == 32);

    std::thread producer{[&] {
        for (std::size_t value = 0; value < value_count; ++value) {
            while (!queue.try_push(std::size_t{value})) {
                std::this_thread::yield();
            }
        }
    }};

    for (std::size_t expected = 0; expected < value_count; ++expected) {
        std::optional<std::size_t> value;
        while (!(value = queue.try_pop())) {
            std::this_thread::yield();
        }
        GLIFI_REQUIRE(*value == expected);
    }
    producer.join();
    GLIFI_REQUIRE(queue.empty());
}

GLIFI_TEST("bounded SPSC queue rejects a capacity that makes modular cursors ambiguous") {
    bool rejected{};
    try {
        glifistore::server::BoundedSpscQueue<std::size_t> impossible{
            glifistore::store::paired::spsc_detail::kMaximumCapacity + 1U};
        static_cast<void>(impossible);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    GLIFI_REQUIRE(rejected);
}

GLIFI_TEST("bounded SPSC queue applies backpressure when full under hot producer") {
    glifistore::server::BoundedSpscQueue<std::size_t> queue{4};
    GLIFI_REQUIRE(queue.capacity() == 4);
    std::size_t accepted{};
    for (std::size_t value = 0; value < 16; ++value) {
        if (queue.try_push(std::size_t{value})) {
            ++accepted;
        }
    }
    GLIFI_REQUIRE(accepted == queue.capacity());
    GLIFI_REQUIRE(!queue.try_push(std::size_t{99}));
    GLIFI_REQUIRE(queue.size() == queue.capacity());

    std::size_t drained{};
    while (auto value = queue.try_pop()) {
        GLIFI_REQUIRE(*value == drained);
        ++drained;
    }
    GLIFI_REQUIRE(drained == accepted);
    GLIFI_REQUIRE(queue.empty());
    GLIFI_REQUIRE(queue.try_push(std::size_t{7}));
    GLIFI_REQUIRE(*queue.try_pop() == 7);
}

GLIFI_TEST("bounded SPSC queue stays fair under slow consumer with bursty hot key") {
    // Models Writer-side completion enqueue under a slow Reader drain: the
    // producer must observe full-queue backpressure rather than overwrite, and
    // FIFO order must survive intermittent consumer stalls.
    constexpr std::size_t kCapacity = 8;
    constexpr std::size_t kTotal = 4'000;
    glifistore::server::BoundedSpscQueue<std::size_t> queue{kCapacity};
    std::atomic_bool producer_done{false};
    std::atomic<std::size_t> rejected{0};
    std::atomic<std::size_t> produced{0};

    std::thread producer{[&] {
        for (std::size_t value = 0; value < kTotal; ++value) {
            while (!queue.try_push(std::size_t{value})) {
                rejected.fetch_add(1U, std::memory_order_relaxed);
                std::this_thread::yield();
            }
            produced.fetch_add(1U, std::memory_order_relaxed);
            if ((value & 0x3FU) == 0U) {
                // Hot-key burst: keep pressing while the consumer is stalled.
                for (unsigned spin = 0; spin < 32U; ++spin) {
                    std::this_thread::yield();
                }
            }
        }
        producer_done.store(true, std::memory_order_release);
    }};

    std::size_t expected{};
    while (expected < kTotal) {
        if (auto value = queue.try_pop()) {
            GLIFI_REQUIRE(*value == expected);
            ++expected;
            continue;
        }
        if (producer_done.load(std::memory_order_acquire) && queue.empty()) {
            break;
        }
        // Slow consumer: park longer than a pause instruction.
        std::this_thread::sleep_for(std::chrono::microseconds{20});
    }
    producer.join();
    GLIFI_REQUIRE(expected == kTotal);
    GLIFI_REQUIRE(produced.load() == kTotal);
    GLIFI_REQUIRE(rejected.load() > 0);
    GLIFI_REQUIRE(queue.empty());
}
