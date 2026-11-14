#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/record.hpp"
#include "glifistore/store/store.hpp"
#include "store/store_internal.hpp"
#include "test.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
auto bytes(std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

auto value_string(const glifistore::OwnedValue& value) -> std::string_view {
    return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

class StoreTemporaryDirectory final {
  public:
    StoreTemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-store-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        root_ = created;
    }

    ~StoreTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] auto store_path() const -> std::filesystem::path {
        return root_ / "store";
    }

  private:
    std::filesystem::path root_;
};

class ManualStoreClock final : public glifistore::StoreClock {
  public:
    explicit ManualStoreClock(const std::uint64_t initial_now_ns) : now_ns_(initial_now_ns) {}

    [[nodiscard]] auto now_ns() const noexcept -> std::uint64_t override {
        return now_ns_.load(std::memory_order_relaxed);
    }

    void set(const std::uint64_t now_ns) noexcept {
        now_ns_.store(now_ns, std::memory_order_relaxed);
    }

  private:
    std::atomic<std::uint64_t> now_ns_;
};

class BlockingRecordRead final {
  public:
    void arm() {
        const std::lock_guard lock{mutex_};
        armed_ = true;
    }

    [[nodiscard]] auto wait_until_blocked() -> bool {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, std::chrono::seconds{5}, [&] { return blocked_; });
    }

    void release() {
        {
            const std::lock_guard lock{mutex_};
            released_ = true;
        }
        condition_.notify_all();
    }

    static auto read_some_at(void* opaque, const int descriptor, const std::span<std::byte> bytes,
                             const std::uint64_t offset) -> std::ptrdiff_t {
        auto& state = *static_cast<BlockingRecordRead*>(opaque);
        if (offset >= glifistore::kSegmentHeaderReservedBytes) {
            std::unique_lock lock{state.mutex_};
            if (state.armed_ && !state.claimed_) {
                state.claimed_ = true;
                state.blocked_ = true;
                state.condition_.notify_all();
                state.condition_.wait(lock, [&] { return state.released_; });
            }
        }
        return ::pread(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset));
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool armed_{};
    bool claimed_{};
    bool blocked_{};
    bool released_{};
};

auto bootstrap_store_id() -> glifistore::StoreId {
    return {std::byte{0x91}, std::byte{0x92}, std::byte{0x93}, std::byte{0x94},
            std::byte{0x95}, std::byte{0x96}, std::byte{0x97}, std::byte{0x98},
            std::byte{0x99}, std::byte{0x9A}, std::byte{0x9B}, std::byte{0x9C},
            std::byte{0x9D}, std::byte{0x9E}, std::byte{0x9F}, std::byte{0xA0}};
}
} // namespace

namespace {
[[nodiscard]] auto legacy_cfg(glifistore::StoreConfig cfg = {}) -> glifistore::StoreConfig {
    cfg.concurrency = glifistore::StoreConcurrencyMode::legacy_mutex;
    return cfg;
}
} // namespace

GLIFI_TEST("key routing is deterministic and stable across worker counts") {
    GLIFI_REQUIRE(glifistore::route_worker("alpha", 4) == glifistore::route_worker("alpha", 4));
    GLIFI_REQUIRE(glifistore::route_worker("alpha", 4) != glifistore::route_worker("beta", 4) ||
                  glifistore::hash_key("alpha") % 4 == glifistore::hash_key("beta") % 4);
}

GLIFI_TEST("store put get round trip preserves value") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 2}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("hello", bytes("world")).has_value());
    const auto record = store.get("hello");
    GLIFI_REQUIRE(record.has_value());
    GLIFI_REQUIRE(value_string(*record) == "world");
}

GLIFI_TEST("simultaneous Stores keep independent Worker routing") {
    constexpr glifistore::WorkerRoutingState fnv{};
    constexpr glifistore::WorkerRoutingState sip{
        .algorithm = glifistore::RoutingAlgorithm::siphash24_v1,
        .seed = 0xA5A5'5A5A'0123'4567ULL,
    };
    std::string key;
    for (std::size_t candidate = 0; candidate < 1024; ++candidate) {
        key = "routing-key-" + std::to_string(candidate);
        if (glifistore::route_worker(glifistore::hash_key_routing(key, fnv), 2) !=
            glifistore::route_worker(glifistore::hash_key_routing(key, sip), 2)) {
            break;
        }
    }
    GLIFI_REQUIRE(!key.empty());

    auto first = glifistore::Store::open({.worker_config = {.explicit_count = 2}});
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE((*first)->put(key, bytes("fnv-before")).has_value());

    auto second = glifistore::Store::open({
        .worker_config = {.explicit_count = 2},
        .worker_routing = {.algorithm = sip.algorithm, .seed = sip.seed, .seed_explicit = true},
    });
    GLIFI_REQUIRE(second.has_value());
    GLIFI_REQUIRE((*second)->put(key, bytes("sip-value")).has_value());
    GLIFI_REQUIRE((*first)->put(key, bytes("fnv-after")).has_value());

    const auto first_value = (*first)->get(key);
    const auto second_value = (*second)->get(key);
    GLIFI_REQUIRE(first_value.has_value());
    GLIFI_REQUIRE(second_value.has_value());
    GLIFI_REQUIRE(value_string(*first_value) == "fnv-after");
    GLIFI_REQUIRE(value_string(*second_value) == "sip-value");
    GLIFI_REQUIRE((*first)->verify_index().has_value());
    GLIFI_REQUIRE((*second)->verify_index().has_value());
}

GLIFI_TEST("store replace updates visible value and sequence") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("key", bytes("old")).has_value());
    const auto first = store.get("key");
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE(store.put("key", bytes("new")).has_value());
    const auto second = store.get("key");
    GLIFI_REQUIRE(second.has_value());
    GLIFI_REQUIRE(second->sequence > first->sequence);
    GLIFI_REQUIRE(value_string(*second) == "new");
}

GLIFI_TEST("store erase removes key and rejects subsequent reads") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("gone", bytes("v")).has_value());
    GLIFI_REQUIRE(store.erase("gone").has_value());
    const auto missing = store.get("gone");
    GLIFI_REQUIRE(!missing.has_value());
    GLIFI_REQUIRE(missing.error().code == glifistore::ErrorCode::not_found);
}

GLIFI_TEST("store get hides expired keys") {
    const auto clock = std::make_shared<ManualStoreClock>(99);
    auto opened =
        glifistore::Store::open(legacy_cfg({.worker_config = {.explicit_count = 1}, .clock = clock}));
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("expired", bytes("v"), 100).has_value());
    const auto visible = store.get("expired");
    GLIFI_REQUIRE(visible.has_value());
    clock->set(100);
    const auto hidden = store.get("expired");
    GLIFI_REQUIRE(!hidden.has_value());
    GLIFI_REQUIRE(hidden.error().code == glifistore::ErrorCode::not_found);
    const auto route = glifistore::route_worker("expired", store.worker_count());
    GLIFI_REQUIRE(!glifistore::detail::StoreAccess::worker(store, route).index().find("expired").has_value());
    GLIFI_REQUIRE(store.verify_index().has_value());
}

GLIFI_TEST("store clock never moves backward within one Store instance") {
    const auto clock = std::make_shared<ManualStoreClock>(100);
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}, .clock = clock});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("past", bytes("v"), 75).has_value());
    clock->set(50);
    const auto hidden = store.get("past");
    GLIFI_REQUIRE(!hidden.has_value());
    GLIFI_REQUIRE(hidden.error().code == glifistore::ErrorCode::not_found);
}

GLIFI_TEST("store clock handles maximum timestamp and no-expiration sentinel") {
    constexpr auto maximum_time = std::numeric_limits<std::uint64_t>::max();
    const auto clock = std::make_shared<ManualStoreClock>(maximum_time - 1U);
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}, .clock = clock});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("maximum", bytes("v"), maximum_time).has_value());
    GLIFI_REQUIRE(store.get("maximum").has_value());
    clock->set(maximum_time);
    const auto expired = store.get("maximum");
    GLIFI_REQUIRE(!expired.has_value());
    GLIFI_REQUIRE(expired.error().code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE(store.put("forever", bytes("v"), 0).has_value());
    GLIFI_REQUIRE(store.get("forever").has_value());
}

GLIFI_TEST("default Store clock expires timestamps in the Unix epoch past") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    GLIFI_REQUIRE(opened.has_value());
    GLIFI_REQUIRE((*opened)->put("past", bytes("v"), 1).has_value());
    const auto hidden = (*opened)->get("past");
    GLIFI_REQUIRE(!hidden.has_value());
    GLIFI_REQUIRE(hidden.error().code == glifistore::ErrorCode::not_found);
}

GLIFI_TEST("store keeps partitioned keys on routed workers only") {
    constexpr std::size_t worker_total = 8;
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = worker_total}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    std::string key_a;
    std::string key_b;
    std::size_t route_a{};
    std::size_t route_b{};
    for (std::uint64_t seed = 0;; ++seed) {
        key_a = "route-key-" + std::to_string(seed);
        key_b = "route-key-" + std::to_string(seed + 100000U);
        route_a = glifistore::route_worker(key_a, worker_total);
        route_b = glifistore::route_worker(key_b, worker_total);
        if (route_a != route_b) {
            break;
        }
    }

    GLIFI_REQUIRE(store.put(key_a, bytes("a")).has_value());
    GLIFI_REQUIRE(store.put(key_b, bytes("b")).has_value());
    const auto& worker_a = glifistore::detail::StoreAccess::worker(store, route_a);
    const auto& worker_b = glifistore::detail::StoreAccess::worker(store, route_b);
    GLIFI_REQUIRE(worker_a.index().find(key_a).has_value());
    GLIFI_REQUIRE(!worker_a.index().find(key_b).has_value());
    GLIFI_REQUIRE(worker_b.index().find(key_b).has_value());
    GLIFI_REQUIRE(!worker_b.index().find(key_a).has_value());
}

GLIFI_TEST("store routes keys to distinct worker partitions") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 4}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("worker-key-a", bytes("a")).has_value());
    GLIFI_REQUIRE(store.put("worker-key-b", bytes("b")).has_value());
    const auto route_a = glifistore::route_worker("worker-key-a", store.worker_count());
    const auto route_b = glifistore::route_worker("worker-key-b", store.worker_count());
    GLIFI_REQUIRE(store.get("worker-key-a").has_value());
    GLIFI_REQUIRE(store.get("worker-key-b").has_value());
    GLIFI_REQUIRE(store.verify_index().has_value());
    GLIFI_REQUIRE(route_a < store.worker_count());
    GLIFI_REQUIRE(route_b < store.worker_count());
}

GLIFI_TEST("store verify index matches segment scan rebuild") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 3}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("one", bytes("1")).has_value());
    GLIFI_REQUIRE(store.put("two", bytes("2")).has_value());
    GLIFI_REQUIRE(store.put("three", bytes("3")).has_value());
    GLIFI_REQUIRE(store.put("two", bytes("22")).has_value());
    GLIFI_REQUIRE(store.erase("one").has_value());
    GLIFI_REQUIRE(store.verify_index().has_value());
    const auto segments = glifistore::detail::StoreAccess::segments(store);
    const auto rebuilt = glifistore::rebuild_index_from_segments(segments);
    GLIFI_REQUIRE(rebuilt.has_value());
    GLIFI_REQUIRE(rebuilt->index.find("one") == std::nullopt);
    GLIFI_REQUIRE(rebuilt->index.find("two").has_value());
    GLIFI_REQUIRE(rebuilt->index.find("three").has_value());
}

GLIFI_TEST("store round trips a key larger than 16-bit lengths") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    const std::string key(70'000, 'k');
    GLIFI_REQUIRE(store.put(key, bytes("value")).has_value());
    const auto record = store.get(key);
    GLIFI_REQUIRE(record.has_value());
    GLIFI_REQUIRE(value_string(*record) == "value");
    GLIFI_REQUIRE(store.verify_index().has_value());
}

GLIFI_TEST("owned store reads survive replacement and store destruction") {
    glifistore::OwnedValue snapshot;
    {
        auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
        GLIFI_REQUIRE(opened.has_value());
        auto& store = **opened;
        GLIFI_REQUIRE(store.put("stable", bytes("first")).has_value());
        auto first = store.get_copy("stable");
        GLIFI_REQUIRE(first.has_value());
        snapshot = std::move(*first);
        GLIFI_REQUIRE(store.put("stable", bytes("second")).has_value());
        GLIFI_REQUIRE(value_string(snapshot) == "first");
    }
    GLIFI_REQUIRE(value_string(snapshot) == "first");
}

GLIFI_TEST("store byte key API preserves embedded zeros and empty keys") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    const std::array binary_key{std::byte{'a'}, std::byte{0}, std::byte{'b'}};
    const std::span<const std::byte> empty_key;
    GLIFI_REQUIRE(store.put(binary_key, bytes("binary")).has_value());
    GLIFI_REQUIRE(store.put(empty_key, bytes("empty")).has_value());
    const auto binary = store.get(binary_key);
    const auto empty = store.get(empty_key);
    GLIFI_REQUIRE(binary.has_value());
    GLIFI_REQUIRE(empty.has_value());
    GLIFI_REQUIRE(value_string(*binary) == "binary");
    GLIFI_REQUIRE(value_string(*empty) == "empty");
}

GLIFI_TEST("durable Store requires an explicit data directory") {
    const auto opened = glifistore::Store::open({.storage_mode = glifistore::StorageMode::durable_sync});
    GLIFI_REQUIRE(!opened.has_value());
    GLIFI_REQUIRE(opened.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("Store validates durable-only resource policy before initialization") {
    auto volatile_limits = glifistore::DurableResourceLimits{};
    volatile_limits.max_live_keys = 1;
    const auto volatile_store = glifistore::Store::open({.durable_limits = volatile_limits});
    GLIFI_REQUIRE(!volatile_store.has_value());
    GLIFI_REQUIRE(volatile_store.error().code == glifistore::ErrorCode::invalid_argument);

    StoreTemporaryDirectory temporary;
    auto invalid_limits = glifistore::DurableResourceLimits{};
    invalid_limits.max_write_amplification = 0;
    const auto invalid = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = temporary.store_path(),
        .durable_limits = invalid_limits,
    });
    GLIFI_REQUIRE(!invalid.has_value());
    GLIFI_REQUIRE(invalid.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("default durable budget rejects 256 Worker reservation before bootstrap") {
    StoreTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    const auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = glifistore::kMaximumWorkerCount},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = path,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
    });
    GLIFI_REQUIRE(!opened.has_value());
    GLIFI_REQUIRE(opened.error().code == glifistore::ErrorCode::storage_exhausted);
    GLIFI_REQUIRE(!std::filesystem::exists(path / glifistore::kBootstrapIntentFilename));
    GLIFI_REQUIRE(!std::filesystem::exists(path / glifistore::kManifestFilename));
}

GLIFI_TEST("durable live-key budget is reusable after erase") {
    StoreTemporaryDirectory temporary;
    auto limits = glifistore::DurableResourceLimits{};
    limits.max_live_keys = 1;
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = temporary.store_path(),
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .durable_limits = limits,
    });
    GLIFI_REQUIRE(opened.has_value());
    GLIFI_REQUIRE((*opened)->put("first", bytes("value")).has_value());
    const auto exhausted = (*opened)->put("second", bytes("value"));
    GLIFI_REQUIRE(!exhausted.has_value());
    GLIFI_REQUIRE(exhausted.error().code == glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE((*opened)->erase("first").has_value());
    GLIFI_REQUIRE((*opened)->put("second", bytes("value")).has_value());
}

GLIFI_TEST("durable recovery memory and live-key budgets fail before service") {
    StoreTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    {
        auto created = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = path,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(created.has_value());
        GLIFI_REQUIRE((*created)->put("recovery-budget", bytes("value")).has_value());
        GLIFI_REQUIRE((*created)->put("recovery-budget-2", bytes("value")).has_value());
        GLIFI_REQUIRE((*created)->close().has_value());
    }
    auto limits = glifistore::DurableResourceLimits{};
    limits.max_recovery_memory_bytes = 1;
    const auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = path,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
        .durable_limits = limits,
    });
    GLIFI_REQUIRE(!reopened.has_value());
    GLIFI_REQUIRE(reopened.error().code == glifistore::ErrorCode::resource_exhausted);

    limits = {};
    limits.max_live_keys = 1;
    const auto too_many_keys = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = path,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
        .durable_limits = limits,
    });
    GLIFI_REQUIRE(!too_many_keys.has_value());
    GLIFI_REQUIRE(too_many_keys.error().code == glifistore::ErrorCode::resource_exhausted);
}

GLIFI_TEST("durable Store recovery and reads share the injected clock") {
    StoreTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    const auto clock = std::make_shared<ManualStoreClock>(99);
    {
        auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                               .storage_mode = glifistore::StorageMode::durable_sync,
                                               .data_directory = path,
                                               .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                               .clock = clock});
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("expires", bytes("v"), 100).has_value());
        GLIFI_REQUIRE((*opened)->get("expires").has_value());
    }

    clock->set(100);
    auto reopened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                             .storage_mode = glifistore::StorageMode::durable_sync,
                                             .data_directory = path,
                                             .durable_open_mode = glifistore::DurableOpenMode::open_existing,
                                             .clock = clock});
    GLIFI_REQUIRE(reopened.has_value());
    const auto expired = (*reopened)->get("expires");
    GLIFI_REQUIRE(!expired.has_value());
    GLIFI_REQUIRE(expired.error().code == glifistore::ErrorCode::not_found);
}

GLIFI_TEST("durable get lazily reclaims expired Index entries on hot and cold paths") {
    StoreTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    const auto clock = std::make_shared<ManualStoreClock>(99);
    auto limits = glifistore::DurableResourceLimits{};
    limits.max_live_keys = 1;

    {
        auto opened =
            glifistore::Store::open(legacy_cfg({.worker_config = {.explicit_count = 1},
                                                .storage_mode = glifistore::StorageMode::durable_sync,
                                                .data_directory = path,
                                                .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                                .durable_limits = limits,
                                                .clock = clock}));
        GLIFI_REQUIRE(opened.has_value());
        auto& store = **opened;
        GLIFI_REQUIRE(store.put("expired-hot", bytes("v"), 100).has_value());
        GLIFI_REQUIRE(store.get("expired-hot").has_value());
        clock->set(100);
        const auto hidden = store.get("expired-hot");
        GLIFI_REQUIRE(!hidden.has_value());
        GLIFI_REQUIRE(hidden.error().code == glifistore::ErrorCode::not_found);
        // Hot-path reclaim must free the live-key budget without a durable erase tombstone.
        GLIFI_REQUIRE(store.put("replacement-hot", bytes("next"), 0).has_value());
        GLIFI_REQUIRE(value_string(*store.get("replacement-hot")) == "next");
        GLIFI_REQUIRE(store.verify_index().has_value());
        GLIFI_REQUIRE(store.erase("replacement-hot").has_value());
        GLIFI_REQUIRE(store.close().has_value());
    }

    {
        // Cold path: reopen with hot cache disabled so GET must validate the Record on disk.
        limits.max_hot_cache_bytes = 0;
        limits.max_hot_cache_bytes_per_worker = 0;
        limits.max_hot_cache_staging_bytes_per_worker = 0;
        limits.max_hot_cache_entries_per_worker = 0;
        clock->set(199);
        auto opened = glifistore::Store::open(
            legacy_cfg({.worker_config = {.explicit_count = 1},
                        .storage_mode = glifistore::StorageMode::durable_sync,
                        .data_directory = path,
                        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
                        .durable_limits = limits,
                        .clock = clock}));
        GLIFI_REQUIRE(opened.has_value());
        auto& store = **opened;
        GLIFI_REQUIRE(store.put("expired-cold", bytes("cold"), 200).has_value());
        GLIFI_REQUIRE(store.get("expired-cold").has_value());
        clock->set(200);
        const auto hidden = store.get("expired-cold");
        GLIFI_REQUIRE(!hidden.has_value());
        GLIFI_REQUIRE(hidden.error().code == glifistore::ErrorCode::not_found);
        GLIFI_REQUIRE(store.put("replacement-cold", bytes("after"), 0).has_value());
        GLIFI_REQUIRE(value_string(*store.get("replacement-cold")) == "after");
        GLIFI_REQUIRE(store.verify_index().has_value());
    }
}

GLIFI_TEST("Store rejects legacy recovery timestamp overrides") {
    const auto opened = glifistore::Store::open({.recovery_now_ns = 1});
    GLIFI_REQUIRE(!opened.has_value());
    GLIFI_REQUIRE(opened.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("public durable Store creates commits reopens and enforces persisted Worker count") {
    StoreTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    {
        auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 2},
                                               .storage_mode = glifistore::StorageMode::durable_sync,
                                               .data_directory = path,
                                               .durable_open_mode = glifistore::DurableOpenMode::create_new});
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->worker_count() == 2);
        GLIFI_REQUIRE((*opened)->put("stable", bytes("first")).has_value());
        GLIFI_REQUIRE(value_string(*(*opened)->get("stable")) == "first");
        const auto server_key = glifistore::HashedKey::compute("server-owned");
        const auto owner = glifistore::route_worker(server_key.hash, (*opened)->worker_count());
        GLIFI_REQUIRE(glifistore::detail::StoreAccess::put(**opened, owner, server_key, bytes("bridge"), 0)
                          .has_value());
        const auto bridged = glifistore::detail::StoreAccess::get_owned(**opened, owner, server_key, 0);
        GLIFI_REQUIRE(bridged.has_value());
        GLIFI_REQUIRE(value_string(*bridged) == "bridge");
        GLIFI_REQUIRE((*opened)->verify_index().has_value());

        const auto locked =
            glifistore::Store::open({.storage_mode = glifistore::StorageMode::durable_sync,
                                     .data_directory = path,
                                     .durable_open_mode = glifistore::DurableOpenMode::open_existing});
        GLIFI_REQUIRE(!locked.has_value());
    }

    {
        auto reopened =
            glifistore::Store::open({.worker_config = {.explicit_count = 2},
                                     .storage_mode = glifistore::StorageMode::durable_sync,
                                     .data_directory = path,
                                     .durable_open_mode = glifistore::DurableOpenMode::open_existing});
        GLIFI_REQUIRE(reopened.has_value());
        GLIFI_REQUIRE(value_string(*(*reopened)->get("stable")) == "first");
        GLIFI_REQUIRE((*reopened)->put("stable", bytes("second")).has_value());
        GLIFI_REQUIRE((*reopened)->erase("stable").has_value());
        GLIFI_REQUIRE(!(*reopened)->get("stable").has_value());
        const auto absent_erase = (*reopened)->erase("stable");
        GLIFI_REQUIRE(!absent_erase.has_value());
        GLIFI_REQUIRE(absent_erase.error().code == glifistore::ErrorCode::not_found);
    }

    const auto mismatch =
        glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                 .storage_mode = glifistore::StorageMode::durable_sync,
                                 .data_directory = path,
                                 .durable_open_mode = glifistore::DurableOpenMode::open_existing});
    GLIFI_REQUIRE(!mismatch.has_value());
    GLIFI_REQUIRE(mismatch.error().code == glifistore::ErrorCode::invalid_argument);

    const auto duplicate =
        glifistore::Store::open({.storage_mode = glifistore::StorageMode::durable_sync,
                                 .data_directory = path,
                                 .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(!duplicate.has_value());
    GLIFI_REQUIRE(duplicate.error().code == glifistore::ErrorCode::sequence_conflict);
}

GLIFI_TEST("public durable Store completes an interrupted bootstrap intent") {
    StoreTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    const auto store_id = bootstrap_store_id();
    const std::vector entries{
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::active},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{1},
                                         .role = glifistore::ManifestSegmentRole::active},
    };
    const glifistore::Manifest intent{
        .store_id = store_id,
        .manifest_generation = 1,
        .routing_algorithm = glifistore::RoutingAlgorithm::fnv1a64_v1,
        .worker_count = 2,
        .routing_epoch = 1,
        .next_segment_id = glifistore::SegmentId{3},
        .next_segment_generation = glifistore::GenerationId{1},
        .segments = entries,
    };
    {
        auto directory =
            glifistore::DataDirectory::open_and_lock(path, glifistore::DataDirectoryOpenMode::create_new);
        GLIFI_REQUIRE(directory.has_value());
        GLIFI_REQUIRE(directory->publish_bootstrap_intent(intent).has_value());
        GLIFI_REQUIRE(directory->publish_manifest(intent).durable());
        const glifistore::SegmentHeaderIdentity first_identity{
            .store_id = store_id,
            .segment_id = entries[0].segment_id,
            .generation = entries[0].generation,
            .owner_worker = entries[0].owner_worker,
        };
        GLIFI_REQUIRE(glifistore::DurableSegmentFile::create(*directory, first_identity).durable());
    }

    auto completed =
        glifistore::Store::open({.worker_config = {.explicit_count = 2},
                                 .storage_mode = glifistore::StorageMode::durable_sync,
                                 .data_directory = path,
                                 .durable_open_mode = glifistore::DurableOpenMode::open_existing});
    GLIFI_REQUIRE(completed.has_value());
    GLIFI_REQUIRE((*completed)->worker_count() == 2);
    GLIFI_REQUIRE((*completed)->put("after-bootstrap", bytes("value")).has_value());
    GLIFI_REQUIRE(value_string(*(*completed)->get("after-bootstrap")) == "value");
    GLIFI_REQUIRE(!std::filesystem::exists(path / glifistore::kBootstrapIntentFilename));
}

GLIFI_TEST("durable open-or-create initializes only a pristine directory") {
    {
        StoreTemporaryDirectory temporary;
        const auto path = temporary.store_path();
        GLIFI_REQUIRE(std::filesystem::create_directory(path));
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        const auto stale_temporary = path / glifistore::kBootstrapTemporaryFilename;
        const auto stale_descriptor =
            ::open(stale_temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        GLIFI_REQUIRE(stale_descriptor >= 0);
        GLIFI_REQUIRE(::close(stale_descriptor) == 0);
        auto initialized = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                                    .storage_mode = glifistore::StorageMode::durable_sync,
                                                    .data_directory = path});
        GLIFI_REQUIRE(initialized.has_value());
        GLIFI_REQUIRE(!std::filesystem::exists(stale_temporary));
        GLIFI_REQUIRE((*initialized)->put("created", bytes("yes")).has_value());
    }
    {
        StoreTemporaryDirectory temporary;
        const auto path = temporary.store_path();
        GLIFI_REQUIRE(std::filesystem::create_directory(path));
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        const auto foreign = path / "foreign-file";
        const auto descriptor = ::open(foreign.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        GLIFI_REQUIRE(descriptor >= 0);
        GLIFI_REQUIRE(::close(descriptor) == 0);
        const auto rejected = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                                       .storage_mode = glifistore::StorageMode::durable_sync,
                                                       .data_directory = path});
        GLIFI_REQUIRE(!rejected.has_value());
        GLIFI_REQUIRE(rejected.error().code == glifistore::ErrorCode::invalid_argument);
        GLIFI_REQUIRE(!std::filesystem::exists(path / glifistore::kBootstrapIntentFilename));
        GLIFI_REQUIRE(std::filesystem::exists(foreign));
    }
}

GLIFI_TEST("store rejects invalid public worker configuration") {
    const auto zero_workers = glifistore::Store::open({.worker_config = {.explicit_count = 0}});
    GLIFI_REQUIRE(!zero_workers.has_value());
    GLIFI_REQUIRE(zero_workers.error().code == glifistore::ErrorCode::invalid_argument);

    const auto too_many_workers =
        glifistore::Store::open({.worker_config = {.explicit_count = glifistore::kMaximumWorkerCount + 1U}});
    GLIFI_REQUIRE(!too_many_workers.has_value());
    GLIFI_REQUIRE(too_many_workers.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("durable_periodic rejects zero sync interval") {
    StoreTemporaryDirectory temporary;
    const auto opened = glifistore::Store::open({.storage_mode = glifistore::StorageMode::durable_periodic,
                                                 .data_directory = temporary.store_path(),
                                                 .durable_periodic = {.sync_interval_ms = 0}});
    GLIFI_REQUIRE(!opened.has_value());
    GLIFI_REQUIRE(opened.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("durable_periodic read after write is visible before flush") {
    StoreTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    auto opened = glifistore::Store::open({.storage_mode = glifistore::StorageMode::durable_periodic,
                                           .data_directory = path,
                                           .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                           .durable_periodic = {.sync_interval_ms = 60'000}});
    GLIFI_REQUIRE(opened.has_value());
    GLIFI_REQUIRE((*opened)->put("visible", bytes("now")).has_value());
    const auto value = (*opened)->get("visible");
    GLIFI_REQUIRE(value.has_value());
    GLIFI_REQUIRE(value_string(*value) == "now");
}

GLIFI_TEST("durable_periodic flush makes writes restart durable") {
    StoreTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    {
        auto opened = glifistore::Store::open({.storage_mode = glifistore::StorageMode::durable_periodic,
                                               .data_directory = path,
                                               .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                               .durable_periodic = {.sync_interval_ms = 60'000}});
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("flushed", bytes("value")).has_value());
        GLIFI_REQUIRE((*opened)->flush().has_value());
    }
    auto reopened =
        glifistore::Store::open({.storage_mode = glifistore::StorageMode::durable_periodic,
                                 .data_directory = path,
                                 .durable_open_mode = glifistore::DurableOpenMode::open_existing});
    GLIFI_REQUIRE(reopened.has_value());
    const auto value = (*reopened)->get("flushed");
    GLIFI_REQUIRE(value.has_value());
    GLIFI_REQUIRE(value_string(*value) == "value");
}

GLIFI_TEST("durable_periodic shutdown flush makes background writes restart durable") {
    StoreTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    {
        auto opened = glifistore::Store::open({.storage_mode = glifistore::StorageMode::durable_periodic,
                                               .data_directory = path,
                                               .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                               .durable_periodic = {.sync_interval_ms = 60'000}});
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("shutdown", bytes("value")).has_value());
    }
    auto reopened =
        glifistore::Store::open({.storage_mode = glifistore::StorageMode::durable_periodic,
                                 .data_directory = path,
                                 .durable_open_mode = glifistore::DurableOpenMode::open_existing});
    GLIFI_REQUIRE(reopened.has_value());
    const auto value = (*reopened)->get("shutdown");
    GLIFI_REQUIRE(value.has_value());
    GLIFI_REQUIRE(value_string(*value) == "value");
}

GLIFI_TEST("Store close is idempotent and rejects operations after releasing volatile resources") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("before-close", bytes("value")).has_value());
    GLIFI_REQUIRE(store.close().has_value());
    GLIFI_REQUIRE(store.close().has_value());
    GLIFI_REQUIRE(store.worker_count() == 1);

    const auto get = store.get("before-close");
    GLIFI_REQUIRE(!get.has_value());
    GLIFI_REQUIRE(get.error().code == glifistore::ErrorCode::unavailable);
    const auto put = store.put("after-close", bytes("value"));
    GLIFI_REQUIRE(!put.has_value());
    GLIFI_REQUIRE(put.error().code == glifistore::ErrorCode::unavailable);
    const auto erase = store.erase("before-close");
    GLIFI_REQUIRE(!erase.has_value());
    GLIFI_REQUIRE(erase.error().code == glifistore::ErrorCode::unavailable);
    const auto flush = store.flush();
    GLIFI_REQUIRE(!flush.has_value());
    GLIFI_REQUIRE(flush.error().code == glifistore::ErrorCode::unavailable);
    const auto compacted = store.compact();
    GLIFI_REQUIRE(!compacted.has_value());
    GLIFI_REQUIRE(compacted.error().code == glifistore::ErrorCode::unavailable);
    const auto verified = store.verify_index();
    GLIFI_REQUIRE(!verified.has_value());
    GLIFI_REQUIRE(verified.error().code == glifistore::ErrorCode::unavailable);
}

GLIFI_TEST("Store compaction is explicit maintenance and no-ops without sealed history") {
    auto volatile_store = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    GLIFI_REQUIRE(volatile_store.has_value());
    const auto volatile_no_work = (*volatile_store)->compact();
    GLIFI_REQUIRE(volatile_no_work.has_value());
    GLIFI_REQUIRE(!volatile_no_work->compacted);

    StoreTemporaryDirectory temporary;
    auto durable_store = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = temporary.store_path(),
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
    });
    GLIFI_REQUIRE(durable_store.has_value());
    const auto no_work = (*durable_store)->compact();
    GLIFI_REQUIRE(no_work.has_value());
    GLIFI_REQUIRE(!no_work->compacted);
    GLIFI_REQUIRE(!no_work->worker_index.has_value());
}

GLIFI_TEST("durable periodic close flushes and releases the directory lock before destruction") {
    StoreTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .storage_mode = glifistore::StorageMode::durable_periodic,
                                           .data_directory = path,
                                           .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                           .durable_periodic = {.sync_interval_ms = 60'000}});
    GLIFI_REQUIRE(opened.has_value());
    GLIFI_REQUIRE((*opened)->put("explicit-close", bytes("value")).has_value());
    GLIFI_REQUIRE((*opened)->close().has_value());

    auto reopened =
        glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                 .storage_mode = glifistore::StorageMode::durable_sync,
                                 .data_directory = path,
                                 .durable_open_mode = glifistore::DurableOpenMode::open_existing});
    GLIFI_REQUIRE(reopened.has_value());
    const auto value = (*reopened)->get("explicit-close");
    GLIFI_REQUIRE(value.has_value());
    GLIFI_REQUIRE(value_string(*value) == "value");
}

GLIFI_TEST("Store close forces a partial strict group and releases its producer") {
    StoreTemporaryDirectory temporary;
    auto opened = glifistore::Store::open(
        legacy_cfg({.worker_config = {.explicit_count = 1},
                    .storage_mode = glifistore::StorageMode::durable_group,
                    .data_directory = temporary.store_path(),
                    .durable_open_mode = glifistore::DurableOpenMode::create_new,
                    .durable_group = {.max_records = 32, .max_bytes = 65'536, .max_wait_ms = 60'000}}));
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    std::mutex mutex;
    std::condition_variable changed;
    bool producer_started{};
    bool producer_completed{};
    glifistore::Status producer_result;
    std::thread producer{[&] {
        {
            const std::lock_guard lock{mutex};
            producer_started = true;
        }
        changed.notify_all();
        auto result = store.put("partial-close", bytes("value"));
        {
            const std::lock_guard lock{mutex};
            producer_result = std::move(result);
            producer_completed = true;
        }
        changed.notify_all();
    }};
    {
        std::unique_lock lock{mutex};
        GLIFI_REQUIRE(changed.wait_for(lock, std::chrono::seconds{2}, [&] { return producer_started; }));
        GLIFI_REQUIRE(
            !changed.wait_for(lock, std::chrono::milliseconds{25}, [&] { return producer_completed; }));
    }

    const auto started = std::chrono::steady_clock::now();
    const auto closed = store.close();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    producer.join();
    GLIFI_REQUIRE(closed.has_value());
    GLIFI_REQUIRE(producer_result.has_value());
    GLIFI_REQUIRE(elapsed < std::chrono::seconds{2});
}

GLIFI_TEST("concurrent Store flush and close calls complete without deadlock") {
    StoreTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .storage_mode = glifistore::StorageMode::durable_periodic,
                                           .data_directory = path,
                                           .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                           .durable_periodic = {.sync_interval_ms = 60'000}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("flush-close-race", bytes("value")).has_value());

    constexpr std::size_t kCloserCount = 4;
    constexpr std::size_t kFlusherCount = 4;
    std::barrier start{static_cast<std::ptrdiff_t>(kCloserCount + kFlusherCount + 1)};
    std::array<glifistore::Status, kCloserCount> close_results;
    std::array<glifistore::Status, kFlusherCount> flush_results;
    std::vector<std::thread> threads;
    threads.reserve(kCloserCount + kFlusherCount);
    for (std::size_t index = 0; index < kCloserCount; ++index) {
        threads.emplace_back([&, index] {
            start.arrive_and_wait();
            close_results[index] = store.close();
        });
    }
    for (std::size_t index = 0; index < kFlusherCount; ++index) {
        threads.emplace_back([&, index] {
            start.arrive_and_wait();
            flush_results[index] = store.flush();
        });
    }
    start.arrive_and_wait();
    for (auto& thread : threads) {
        thread.join();
    }

    for (const auto& result : close_results) {
        GLIFI_REQUIRE(result.has_value());
    }
    for (const auto& result : flush_results) {
        GLIFI_REQUIRE(result.has_value() || result.error().code == glifistore::ErrorCode::unavailable);
    }

    auto reopened =
        glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                 .storage_mode = glifistore::StorageMode::durable_sync,
                                 .data_directory = path,
                                 .durable_open_mode = glifistore::DurableOpenMode::open_existing});
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->get("flush-close-race").has_value());
}

GLIFI_TEST("durable_group rejects invalid batch configuration") {
    StoreTemporaryDirectory temporary;
    const auto opened = glifistore::Store::open({.storage_mode = glifistore::StorageMode::durable_group,
                                                 .data_directory = temporary.store_path(),
                                                 .durable_group = {.max_records = 0}});
    GLIFI_REQUIRE(!opened.has_value());
    GLIFI_REQUIRE(opened.error().code == glifistore::ErrorCode::invalid_argument);

    const auto zero_minimum = glifistore::Store::open(
        {.storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = temporary.store_path(),
         .durable_group = {.max_records = 4, .max_bytes = 65'536, .max_wait_ms = 10, .min_records = 0}});
    GLIFI_REQUIRE(!zero_minimum.has_value());
    GLIFI_REQUIRE(zero_minimum.error().code == glifistore::ErrorCode::invalid_argument);

    const auto inverted = glifistore::Store::open(
        {.storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = temporary.store_path(),
         .durable_group = {.max_records = 4, .max_bytes = 65'536, .max_wait_ms = 10, .min_records = 5}});
    GLIFI_REQUIRE(!inverted.has_value());
    GLIFI_REQUIRE(inverted.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("durable_group concurrent puts batch and survive reopen") {
    StoreTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    constexpr std::uint32_t kBatchSize = 32;
    {
        auto opened = glifistore::Store::open(legacy_cfg(
            {.worker_config = {.explicit_count = 1},
             .storage_mode = glifistore::StorageMode::durable_group,
             .data_directory = path,
             .durable_open_mode = glifistore::DurableOpenMode::create_new,
             .durable_group = {.max_records = kBatchSize, .max_bytes = 65536, .max_wait_ms = 60'000}}));
        GLIFI_REQUIRE(opened.has_value());
        auto& store = **opened;
        std::atomic<bool> failed{false};
        std::vector<std::thread> workers;
        workers.reserve(kBatchSize);
        for (std::uint32_t index = 0; index < kBatchSize; ++index) {
            workers.emplace_back([&, index]() {
                const std::string key = std::string(96, 'K') + '-' + std::to_string(index);
                if (!store.put(key, bytes("value-" + std::to_string(index))).has_value()) {
                    failed.store(true);
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        GLIFI_REQUIRE(!failed.load());
        GLIFI_REQUIRE(store.flush().has_value());
    }
    {
        auto directory = glifistore::DataDirectory::open_and_lock(path);
        GLIFI_REQUIRE(directory.has_value());
        const auto manifest = directory->read_manifest();
        GLIFI_REQUIRE(manifest.has_value());
        GLIFI_REQUIRE(manifest->segments.size() == 1);
        const auto& active = manifest->segments.front();
        const glifistore::SegmentHeaderIdentity identity{
            .store_id = manifest->store_id,
            .segment_id = active.segment_id,
            .generation = active.generation,
            .owner_worker = active.owner_worker,
        };
        const auto segment = glifistore::DurableSegmentFile::open(*directory, identity,
                                                                  glifistore::SegmentFileOpenMode::read_only);
        GLIFI_REQUIRE(segment.has_value());
        GLIFI_REQUIRE(segment->selected_commit().commit.commit_generation == 2);
        GLIFI_REQUIRE(segment->selected_commit().commit.record_count == kBatchSize);
    }
    auto reopened = glifistore::Store::open(
        legacy_cfg({.worker_config = {.explicit_count = 1},
                    .storage_mode = glifistore::StorageMode::durable_group,
                    .data_directory = path,
                    .durable_open_mode = glifistore::DurableOpenMode::open_existing}));
    GLIFI_REQUIRE(reopened.has_value());
    for (std::uint32_t index = 0; index < kBatchSize; ++index) {
        const std::string key = std::string(96, 'K') + '-' + std::to_string(index);
        const auto value = (*reopened)->get(key);
        GLIFI_REQUIRE(value.has_value());
        GLIFI_REQUIRE(value_string(*value) == "value-" + std::to_string(index));
    }
}

GLIFI_TEST("durable_group preserves explicitly ordered same-key put and erase") {
    StoreTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    {
        auto opened = glifistore::Store::open(
            {.worker_config = {.explicit_count = 1},
             .storage_mode = glifistore::StorageMode::durable_group,
             .data_directory = path,
             .durable_open_mode = glifistore::DurableOpenMode::create_new,
             .durable_group = {.max_records = 2, .max_bytes = 65536, .max_wait_ms = 60'000}});
        GLIFI_REQUIRE(opened.has_value());
        auto& store = **opened;
        std::atomic_bool put_completed{};
        std::atomic_bool put_failed{};
        std::thread putter([&] {
            put_failed.store(!store.put("same-key", bytes("value")).has_value());
            put_completed.store(true);
        });

        auto* paired = glifistore::detail::StoreAccess::shard_pair_runtime(store);
        GLIFI_REQUIRE(paired != nullptr);
        const auto admitted_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (paired->stats()[0].sync_admitted == 0 &&
               std::chrono::steady_clock::now() < admitted_deadline) {
            std::this_thread::yield();
        }
        const bool put_admitted = paired->stats()[0].sync_admitted == 1;

        glifistore::Status erased = glifistore::fail(glifistore::ErrorCode::not_found, "not tried");
        while (!put_completed.load(std::memory_order_acquire)) {
            erased = store.erase("same-key");
            if (erased.has_value() || erased.error().code != glifistore::ErrorCode::not_found) {
                break;
            }
            std::this_thread::yield();
        }
        putter.join();
        if (!erased && erased.error().code == glifistore::ErrorCode::not_found) {
            // The PUT may complete after the loop condition is sampled but before
            // the first ERASE attempt. Completion publishes the key; retry once
            // after the acquire instead of asserting on the "not tried" sentinel.
            erased = store.erase("same-key");
        }
        GLIFI_REQUIRE(put_admitted);
        GLIFI_REQUIRE(!put_failed.load());
        GLIFI_REQUIRE(erased.has_value());
        const auto missing = store.get("same-key");
        GLIFI_REQUIRE(!missing.has_value());
        GLIFI_REQUIRE(missing.error().code == glifistore::ErrorCode::not_found);
    }
    auto reopened =
        glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                 .storage_mode = glifistore::StorageMode::durable_group,
                                 .data_directory = path,
                                 .durable_open_mode = glifistore::DurableOpenMode::open_existing});
    GLIFI_REQUIRE(reopened.has_value());
    const auto missing = (*reopened)->get("same-key");
    GLIFI_REQUIRE(!missing.has_value());
    GLIFI_REQUIRE(missing.error().code == glifistore::ErrorCode::not_found);
}

GLIFI_TEST("durable_group single put flushes within max_wait_ms") {
    StoreTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    {
        auto opened = glifistore::Store::open(
            {.worker_config = {.explicit_count = 1},
             .storage_mode = glifistore::StorageMode::durable_group,
             .data_directory = path,
             .durable_open_mode = glifistore::DurableOpenMode::create_new,
             .durable_group = {.max_records = 32, .max_bytes = 65536, .max_wait_ms = 50}});
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("solo", bytes("value")).has_value());
    }
    auto reopened =
        glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                 .storage_mode = glifistore::StorageMode::durable_group,
                                 .data_directory = path,
                                 .durable_open_mode = glifistore::DurableOpenMode::open_existing});
    GLIFI_REQUIRE(reopened.has_value());
    const auto value = (*reopened)->get("solo");
    GLIFI_REQUIRE(value.has_value());
    GLIFI_REQUIRE(value_string(*value) == "value");
}

GLIFI_TEST("durable catalog observation enters emergency and rejects put until close") {
    StoreTemporaryDirectory temporary;
    glifistore::DurableResourceLimits limits{};
    limits.max_segment_count = 1;
    limits.max_store_bytes = 4ULL * glifistore::kSegmentSizeBytes;
    limits.max_temporary_compaction_bytes = glifistore::kSegmentSizeBytes;

    {
        // Seed under cooperative maintenance so the background first-eval cannot
        // arm the emergency gate before the put completes (max_segment_count == 1).
        auto seeded = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = temporary.store_path(),
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
            .durable_limits = limits,
            .maintenance = {.mode = glifistore::MaintenanceMode::cooperative},
        });
        GLIFI_REQUIRE(seeded.has_value());
        GLIFI_REQUIRE((*seeded)->put("seed", bytes("value")).has_value());
        GLIFI_REQUIRE((*seeded)->close().has_value());
    }

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = temporary.store_path(),
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
        .durable_limits = limits,
        .maintenance =
            {
                .mode = glifistore::MaintenanceMode::background,
                .min_eval_interval_ms = 60'000,
                .max_eval_interval_ms = 60'000,
            },
    });
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(store);
    GLIFI_REQUIRE(controller != nullptr);
    controller->request_evaluate();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snap = store.maintenance_snapshot();
        if (snap.mutations_rejected) {
            GLIFI_REQUIRE(snap.pressure == glifistore::MaintenancePressureLevel::emergency);
            GLIFI_REQUIRE(snap.last_observation.durable);
            GLIFI_REQUIRE(snap.last_observation.segment_count >= snap.last_observation.max_segment_count);
            const auto put = store.put("blocked", bytes("x"));
            GLIFI_REQUIRE(!put.has_value());
            GLIFI_REQUIRE(put.error().code == glifistore::ErrorCode::storage_exhausted);
            GLIFI_REQUIRE(store.flush().has_value());
            GLIFI_REQUIRE(value_string(*store.get("seed")) == "value");
            GLIFI_REQUIRE(store.close().has_value());
            GLIFI_REQUIRE(!store.maintenance_snapshot().mutations_rejected);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(false);
}

GLIFI_TEST("close during blocked background compact drains then joins") {
    StoreTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    const auto store_id = bootstrap_store_id();
    const std::vector entries{
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{3},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::active},
    };
    const glifistore::Manifest manifest{
        .store_id = store_id,
        .manifest_generation = 1,
        .routing_algorithm = glifistore::RoutingAlgorithm::fnv1a64_v1,
        .worker_count = 1,
        .routing_epoch = 1,
        .next_segment_id = glifistore::SegmentId{4},
        .next_segment_generation = glifistore::GenerationId{1},
        .segments = entries,
    };
    {
        auto directory =
            glifistore::DataDirectory::open_and_lock(path, glifistore::DataDirectoryOpenMode::create_new);
        GLIFI_REQUIRE(directory.has_value());
        for (const auto& entry : entries) {
            const glifistore::SegmentHeaderIdentity identity{
                .store_id = store_id,
                .segment_id = entry.segment_id,
                .generation = entry.generation,
                .owner_worker = entry.owner_worker,
            };
            auto created = glifistore::DurableSegmentFile::create(*directory, identity);
            GLIFI_REQUIRE(created.durable());
            GLIFI_REQUIRE(created.file.has_value());
            if (entry.role == glifistore::ManifestSegmentRole::sealed) {
                const auto key = entry.segment_id.value == 1 ? "first" : "second";
                const auto encoded = glifistore::encode_record({
                    .sequence = glifistore::SequenceNumber{entry.segment_id.value},
                    .opcode = glifistore::Opcode::put,
                    .type = glifistore::ValueType::bytes,
                    .flags = 0,
                    .key_hash = glifistore::hash_key(key),
                    .expire_at_ns = 0,
                    .key = bytes(key),
                    .value = bytes("value"),
                });
                GLIFI_REQUIRE(encoded.has_value());
                GLIFI_REQUIRE(created.file->append(*encoded).committed());
                GLIFI_REQUIRE(created.file->seal().committed());
            }
        }
        GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
    }

    {
        auto thresholded = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = path,
            .durable_open_mode = glifistore::DurableOpenMode::open_existing,
            .maintenance =
                {
                    .mode = glifistore::MaintenanceMode::background,
                    .min_eval_interval_ms = 60'000,
                    .max_eval_interval_ms = 60'000,
                },
        });
        GLIFI_REQUIRE(thresholded.has_value());
        const auto threshold_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < threshold_deadline) {
            const auto snapshot = (*thresholded)->maintenance_snapshot();
            if (snapshot.last_skip_reason == glifistore::MaintenanceSkipReason::reclaim_threshold) {
                GLIFI_REQUIRE(snapshot.last_observation.candidate_dead_byte_ratio_bp == 0);
                GLIFI_REQUIRE(snapshot.compact_attempts == 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        GLIFI_REQUIRE((*thresholded)->maintenance_snapshot().last_skip_reason ==
                      glifistore::MaintenanceSkipReason::reclaim_threshold);
        GLIFI_REQUIRE((*thresholded)->close().has_value());
    }

    BlockingRecordRead blocked_build;
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = path,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
        .maintenance =
            {
                .mode = glifistore::MaintenanceMode::background,
                .min_eval_interval_ms = 60'000,
                .max_eval_interval_ms = 60'000,
                .dead_byte_ratio_bp_normal = 0,
            },
        .filesystem_hooks = {.file_io = {.context = &blocked_build,
                                         .read_some_at = &BlockingRecordRead::read_some_at}},
    });
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(store);
    GLIFI_REQUIRE(controller != nullptr);

    blocked_build.arm();
    controller->request_evaluate();
    GLIFI_REQUIRE(blocked_build.wait_until_blocked());

    glifistore::Status closed;
    std::thread closer{[&] { closed = store.close(); }};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    blocked_build.release();
    closer.join();
    GLIFI_REQUIRE(closed.has_value());
    GLIFI_REQUIRE(!store.maintenance_snapshot().thread_running);
    GLIFI_REQUIRE(store.maintenance_snapshot().state == glifistore::MaintenanceState::stopped);
}
