#include "glifistore/server/connection_handoff.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <utility>
#include <vector>

GLIFI_TEST("connection handoff mesh is bounded and preserves binding state") {
    glifistore::server::ConnectionHandoffMesh mesh{2, 2};
    glifistore::server::ConnectionHandoff first{.bound_worker = 1};
    glifistore::server::ConnectionHandoff second{.bound_worker = 1};
    glifistore::server::ConnectionHandoff rejected{.bound_worker = 1, .initialized = true};

    GLIFI_REQUIRE(mesh.try_handoff(1, std::move(first)));
    GLIFI_REQUIRE(mesh.has_pending(1));
    GLIFI_REQUIRE(mesh.try_handoff(1, std::move(second)));
    GLIFI_REQUIRE(!mesh.try_handoff(1, std::move(rejected)));
    // Full-queue rejection must not consume the handoff: the producer still owns it.
    GLIFI_REQUIRE(rejected.bound_worker == 1);
    GLIFI_REQUIRE(rejected.initialized);

    const auto first_received = mesh.try_pop(1);
    const auto second_received = mesh.try_pop(1);
    GLIFI_REQUIRE(first_received.has_value());
    GLIFI_REQUIRE(second_received.has_value());
    GLIFI_REQUIRE(first_received->bound_worker == 1);
    GLIFI_REQUIRE(second_received->bound_worker == 1);
    GLIFI_REQUIRE(!mesh.try_pop(1).has_value());
    GLIFI_REQUIRE(!mesh.has_pending(1));
}

GLIFI_TEST("connection handoff mesh stop_accepting refuses new transfers") {
    glifistore::server::ConnectionHandoffMesh mesh{2, 2};
    glifistore::server::ConnectionHandoff queued{.bound_worker = 1, .initialized = true};
    GLIFI_REQUIRE(mesh.try_handoff(1, std::move(queued)));
    GLIFI_REQUIRE(mesh.has_pending(1));

    mesh.stop_accepting();
    GLIFI_REQUIRE(!mesh.accepting());
    glifistore::server::ConnectionHandoff late{.bound_worker = 1, .initialized = true};
    GLIFI_REQUIRE(!mesh.try_handoff(1, std::move(late)));
    GLIFI_REQUIRE(late.bound_worker == 1);
    GLIFI_REQUIRE(late.initialized);

    // Already-queued cells remain poppable for drain / reject_orphaned.
    const auto received = mesh.try_pop(1);
    GLIFI_REQUIRE(received.has_value());
    GLIFI_REQUIRE(received->bound_worker == 1);
    GLIFI_REQUIRE(!mesh.has_pending(1));
}

GLIFI_TEST("connection handoff mesh preserves buffered state exactly once") {
    // Exactly-once ownership: successful push transfers buffered fields; full-queue
    // rejection leaves the producer owning the ConnectionHandoff unchanged.
    // BoundedMpscQueue bit-ceils capacity to at least 2, so request 2 and fill both slots.
    glifistore::server::ConnectionHandoffMesh mesh{1, 2};
    const auto now = std::chrono::steady_clock::now();
    glifistore::server::ConnectionHandoff first{
        .principal = "alice.example",
        .capabilities = glifistore::server::Capability::read | glifistore::server::Capability::write,
        .key_prefix = "tenant/",
        .input = {std::byte{0x01}, std::byte{0x02}},
        .output = {std::byte{0x0a}},
        .bound_worker = 0,
        .initialized = true,
        .peer_read_closed = true,
        .last_activity = now,
        .partial_request_since = now,
        .connection_rate_window_start_ns = 42,
        .connection_rate_used = 7,
    };
    GLIFI_REQUIRE(mesh.try_handoff(0, std::move(first)));
    glifistore::server::ConnectionHandoff filler{.bound_worker = 0, .initialized = true};
    GLIFI_REQUIRE(mesh.try_handoff(0, std::move(filler)));

    glifistore::server::ConnectionHandoff rejected{
        .principal = "bob.example",
        .capabilities = glifistore::server::Capability::admin,
        .key_prefix = "other/",
        .input = {std::byte{0xff}},
        .output = {std::byte{0xee}},
        .bound_worker = 0,
        .initialized = true,
        .peer_read_closed = false,
        .connection_rate_used = 3,
    };
    GLIFI_REQUIRE(!mesh.try_handoff(0, std::move(rejected)));
    GLIFI_REQUIRE(rejected.principal == "bob.example");
    GLIFI_REQUIRE(rejected.capabilities == glifistore::server::Capability::admin);
    GLIFI_REQUIRE(rejected.key_prefix == "other/");
    GLIFI_REQUIRE(rejected.input.size() == 1);
    GLIFI_REQUIRE(rejected.output.size() == 1);
    GLIFI_REQUIRE(rejected.initialized);
    GLIFI_REQUIRE(rejected.connection_rate_used == 3);

    const auto received = mesh.try_pop(0);
    GLIFI_REQUIRE(received.has_value());
    GLIFI_REQUIRE(received->principal == "alice.example");
    GLIFI_REQUIRE(received->capabilities ==
                  (glifistore::server::Capability::read | glifistore::server::Capability::write));
    GLIFI_REQUIRE(received->key_prefix == "tenant/");
    GLIFI_REQUIRE(received->input.size() == 2);
    GLIFI_REQUIRE(received->output.size() == 1);
    GLIFI_REQUIRE(received->bound_worker == 0);
    GLIFI_REQUIRE(received->initialized);
    GLIFI_REQUIRE(received->peer_read_closed);
    GLIFI_REQUIRE(received->connection_rate_window_start_ns == 42);
    GLIFI_REQUIRE(received->connection_rate_used == 7);
    GLIFI_REQUIRE(mesh.try_pop(0).has_value());
    GLIFI_REQUIRE(!mesh.try_pop(0).has_value());
}

GLIFI_TEST("connection handoff mesh concurrent producers preserve exactly-once delivery") {
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kPerProducer = 64;
    constexpr std::size_t kCapacity = 8;
    glifistore::server::ConnectionHandoffMesh mesh{1, kCapacity};
    std::atomic<std::size_t> accepted{0};
    std::atomic<std::size_t> rejected{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (std::size_t producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&, producer] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::size_t index = 0; index < kPerProducer; ++index) {
                glifistore::server::ConnectionHandoff cell{
                    .principal = "p" + std::to_string(producer),
                    .bound_worker = 0,
                    .initialized = true,
                    .connection_rate_used = static_cast<std::uint32_t>((producer << 16U) | index),
                };
                for (;;) {
                    if (mesh.try_handoff(0, std::move(cell))) {
                        accepted.fetch_add(1U, std::memory_order_relaxed);
                        break;
                    }
                    rejected.fetch_add(1U, std::memory_order_relaxed);
                    // Producer still owns `cell` after full-queue rejection.
                    GLIFI_REQUIRE(cell.initialized);
                    GLIFI_REQUIRE(cell.bound_worker == 0);
                    std::this_thread::yield();
                }
            }
        });
    }

    std::size_t popped = 0;
    std::thread consumer{[&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (popped < kProducers * kPerProducer && std::chrono::steady_clock::now() < deadline) {
            if (auto cell = mesh.try_pop(0); cell.has_value()) {
                GLIFI_REQUIRE(cell->initialized);
                GLIFI_REQUIRE(cell->bound_worker == 0);
                ++popped;
            } else {
                std::this_thread::yield();
            }
        }
    }};

    start.store(true, std::memory_order_release);
    for (auto& thread : producers) {
        thread.join();
    }
    consumer.join();

    GLIFI_REQUIRE(accepted.load(std::memory_order_relaxed) == kProducers * kPerProducer);
    GLIFI_REQUIRE(popped == kProducers * kPerProducer);
    GLIFI_REQUIRE(!mesh.has_pending(0));
    GLIFI_REQUIRE(rejected.load(std::memory_order_relaxed) > 0);
}
