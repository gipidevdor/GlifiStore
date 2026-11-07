#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/persistence/manifest.hpp"
#include "glifistore/store/store.hpp"
#include "test.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

[[nodiscard]] auto bytes(const std::string_view value) -> std::span<const std::byte> {
    return std::as_bytes(std::span{value.data(), value.size()});
}

[[nodiscard]] auto value_string(const glifistore::OwnedValue& value) -> std::string_view {
    return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

struct RoutingGuard final {
    explicit RoutingGuard(const glifistore::WorkerRoutingState state)
        : previous_(glifistore::get_worker_routing()) {
        glifistore::set_worker_routing(state);
    }

    ~RoutingGuard() {
        glifistore::set_worker_routing(previous_);
    }

    RoutingGuard(const RoutingGuard&) = delete;
    auto operator=(const RoutingGuard&) -> RoutingGuard& = delete;

    glifistore::WorkerRoutingState previous_;
};

[[nodiscard]] auto temp_dir(const std::string_view name) -> std::filesystem::path {
    const auto parent = std::filesystem::temp_directory_path() / "glifistore-worker-routing";
    std::filesystem::create_directories(parent);
    const auto path = parent / name;
    std::filesystem::remove_all(path);
    return path;
}

} // namespace

GLIFI_TEST("worker routing defaults to fnv1a64-v1 with zero seed") {
    RoutingGuard guard{{}};
    GLIFI_REQUIRE(glifistore::get_worker_routing().algorithm == glifistore::RoutingAlgorithm::fnv1a64_v1);
    GLIFI_REQUIRE(glifistore::get_worker_routing().seed == 0);
    GLIFI_REQUIRE(glifistore::hash_key_routing("alpha") == glifistore::hash_key("alpha"));
}

GLIFI_TEST("worker routing seed is stable within a process") {
    RoutingGuard guard{{glifistore::RoutingAlgorithm::siphash24_v1, 0x1111222233334444ULL}};
    const auto first = glifistore::hash_key_routing("tenant-a/orders/1");
    const auto second = glifistore::hash_key_routing("tenant-a/orders/1");
    GLIFI_REQUIRE(first == second);
    GLIFI_REQUIRE(first != glifistore::hash_key("tenant-a/orders/1"));
}

GLIFI_TEST("worker routing publishes algorithm and seed as one coherent revision") {
    constexpr glifistore::WorkerRoutingState kFNV{};
    constexpr glifistore::WorkerRoutingState kSip{glifistore::RoutingAlgorithm::siphash24_v1,
                                                  0xA55A'1234'9876'FEDCULL};
    RoutingGuard guard{kFNV};
    std::atomic_bool start{};
    std::atomic_bool done{};
    std::atomic_bool torn{};

    std::thread writer{[&] {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (std::size_t iteration = 0; iteration < 100'000U; ++iteration) {
            glifistore::set_worker_routing((iteration & 1U) == 0U ? kSip : kFNV);
        }
        done.store(true, std::memory_order_release);
    }};
    std::array<std::thread, 4> readers;
    for (auto& reader : readers) {
        reader = std::thread{[&] {
            while (!start.load(std::memory_order_acquire)) {
            }
            while (!done.load(std::memory_order_acquire)) {
                const auto observed = glifistore::get_worker_routing();
                if (observed != kFNV && observed != kSip) {
                    torn.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        }};
    }
    start.store(true, std::memory_order_release);
    writer.join();
    for (auto& reader : readers) {
        reader.join();
    }
    GLIFI_REQUIRE(!torn.load(std::memory_order_relaxed));
}

GLIFI_TEST("different worker routing seeds select different owners") {
    constexpr std::string_view kKey = "flood-candidate-key";
    constexpr std::size_t kWorkers = 8;
    const auto fnv_owner = glifistore::route_worker(glifistore::hash_key(kKey), kWorkers);

    std::size_t matches = 0;
    for (std::uint64_t seed = 1; seed <= 64; ++seed) {
        const glifistore::WorkerRoutingState state{glifistore::RoutingAlgorithm::siphash24_v1, seed};
        const auto owner = glifistore::route_worker(glifistore::hash_key_routing(kKey, state), kWorkers);
        if (owner == fnv_owner) {
            ++matches;
        }
    }
    GLIFI_REQUIRE(matches < 64);
}

GLIFI_TEST("INIT identity stays plain for FNV and extends for SipHash") {
    const auto plain = glifistore::encode_init_identity_value({});
    GLIFI_REQUIRE(plain.size() == glifistore::kWireProtocolIdentity.size());
    auto decoded_plain = glifistore::decode_init_identity_value(plain);
    GLIFI_REQUIRE(decoded_plain.has_value());
    GLIFI_REQUIRE(!decoded_plain->keyed());

    const glifistore::WorkerRoutingState keyed{glifistore::RoutingAlgorithm::siphash24_v1,
                                               0xABCDEF0123456789ULL};
    const auto extended = glifistore::encode_init_identity_value(keyed);
    GLIFI_REQUIRE(extended.size() == glifistore::kWireInitIdentityExtendedBytes);
    auto decoded = glifistore::decode_init_identity_value(extended);
    GLIFI_REQUIRE(decoded.has_value());
    GLIFI_REQUIRE(*decoded == keyed);
}

GLIFI_TEST("durable Store persists worker hash seed and refuses mismatch") {
    RoutingGuard guard{{}};
    const auto dir = temp_dir("persist-seed");
    {
        auto created = glifistore::Store::open({
            .worker_config = {.explicit_count = 2},
            .worker_routing = {.algorithm = glifistore::RoutingAlgorithm::siphash24_v1,
                               .seed = 42,
                               .seed_explicit = true},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = dir,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
            .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
        });
        GLIFI_REQUIRE(created.has_value());
        GLIFI_REQUIRE((*created)->put("k", bytes("v")).has_value());
        GLIFI_REQUIRE((*created)->close().has_value());
    }

    {
        auto reopened = glifistore::Store::open({
            .worker_config = {.explicit_count = 2},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = dir,
            .durable_open_mode = glifistore::DurableOpenMode::open_existing,
            .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
        });
        GLIFI_REQUIRE(reopened.has_value());
        // Opening a Store must not mutate process-global defaults used by
        // unrelated standalone Index/test contexts.
        GLIFI_REQUIRE(glifistore::get_worker_routing() == glifistore::WorkerRoutingState{});
        auto value = (*reopened)->get("k");
        GLIFI_REQUIRE(value.has_value());
        GLIFI_REQUIRE(value_string(*value) == "v");
        GLIFI_REQUIRE((*reopened)->close().has_value());
    }

    {
        auto mismatched = glifistore::Store::open({
            .worker_config = {.explicit_count = 2},
            .worker_routing = {.algorithm = glifistore::RoutingAlgorithm::siphash24_v1,
                               .seed = 99,
                               .seed_explicit = true},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = dir,
            .durable_open_mode = glifistore::DurableOpenMode::open_existing,
            .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
        });
        GLIFI_REQUIRE(!mismatched.has_value());
        GLIFI_REQUIRE(mismatched.error().code == glifistore::ErrorCode::invalid_argument);
    }
}

GLIFI_TEST("manifest round-trip preserves siphash worker hash seed") {
    glifistore::StoreId store_id{};
    store_id[0] = std::byte{1};
    glifistore::Manifest manifest{
        .store_id = store_id,
        .manifest_generation = 1,
        .routing_algorithm = glifistore::RoutingAlgorithm::siphash24_v1,
        .worker_count = 1,
        .routing_epoch = 1,
        .worker_hash_seed = 0xDEADBEEFCAFEBABEULL,
        .next_segment_id = glifistore::SegmentId{2},
        .next_segment_generation = glifistore::GenerationId{1},
        .segments = {{.segment_id = glifistore::SegmentId{1},
                      .generation = glifistore::GenerationId{1},
                      .owner_worker = glifistore::WorkerId{0},
                      .role = glifistore::ManifestSegmentRole::active}},
    };
    auto encoded = glifistore::encode_manifest(manifest);
    GLIFI_REQUIRE(encoded.has_value());
    auto decoded = glifistore::decode_manifest(*encoded);
    GLIFI_REQUIRE(decoded.has_value());
    GLIFI_REQUIRE(decoded->worker_hash_seed == 0xDEADBEEFCAFEBABEULL);
    GLIFI_REQUIRE(decoded->routing_algorithm == glifistore::RoutingAlgorithm::siphash24_v1);
}
