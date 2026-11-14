#include "persistence/runtime_catalog_quiesce_gate.hpp"
#include "test.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace {

using glifistore::runtime_catalog_detail::IndexQuiesceGate;

} // namespace

GLIFI_TEST("Index quiesce gate is armed exactly while a hold is outstanding") {
    IndexQuiesceGate gate;
    GLIFI_REQUIRE(!gate.armed());
    gate.arm();
    gate.arm();
    GLIFI_REQUIRE(gate.armed());
    GLIFI_REQUIRE(!gate.disarm());
    GLIFI_REQUIRE(gate.armed());
    GLIFI_REQUIRE(gate.disarm());
    GLIFI_REQUIRE(!gate.armed());
    GLIFI_REQUIRE(gate.holders() == 0U);
}

GLIFI_TEST("Index quiesce gate never reads open while a concurrent holder overlaps release") {
    // Regression: the former counter + separate boolean let a final disarm's
    // late `false` store overwrite a concurrent first arm, and let a second
    // holder return before the first holder published `true`.
    IndexQuiesceGate gate;
    std::atomic_bool stop{};
    std::thread contender{[&] {
        while (!stop.load(std::memory_order_relaxed)) {
            gate.arm();
            static_cast<void>(gate.disarm());
        }
    }};
    std::uint64_t open_while_held{};
    for (std::size_t round = 0; round < 200'000; ++round) {
        gate.arm();
        for (int check = 0; check < 4; ++check) {
            if (!gate.armed()) {
                ++open_while_held;
            }
        }
        static_cast<void>(gate.disarm());
    }
    stop.store(true, std::memory_order_relaxed);
    contender.join();
    GLIFI_REQUIRE(open_while_held == 0U);
    GLIFI_REQUIRE(gate.holders() == 0U);
}

GLIFI_TEST("Index quiesce gate excludes a Writer and an owner from the Index together") {
    // Dekker handshake: a Writer enters then checks armed(); an owner arms then
    // drains Writers. At most one side may be inside the protected region.
    IndexQuiesceGate gate;
    std::atomic_uint32_t inside{};
    std::atomic_bool overlap{};
    std::atomic_bool stop{};
    std::thread writer{[&] {
        while (!stop.load(std::memory_order_relaxed)) {
            gate.enter_writer();
            if (!gate.armed()) {
                if (inside.fetch_add(1U, std::memory_order_acq_rel) != 0U) {
                    overlap.store(true, std::memory_order_relaxed);
                }
                inside.fetch_sub(1U, std::memory_order_acq_rel);
            }
            gate.leave_writer();
        }
    }};
    for (std::size_t round = 0; round < 100'000; ++round) {
        gate.arm();
        gate.wait_for_writers();
        if (inside.fetch_add(1U, std::memory_order_acq_rel) != 0U) {
            overlap.store(true, std::memory_order_relaxed);
        }
        inside.fetch_sub(1U, std::memory_order_acq_rel);
        static_cast<void>(gate.disarm());
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    GLIFI_REQUIRE(!overlap.load());
    GLIFI_REQUIRE(gate.writer_depth() == 0U);
}
