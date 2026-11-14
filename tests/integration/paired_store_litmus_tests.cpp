#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/store/paired/publication_coordinator.hpp"
#include "glifistore/store/store.hpp"
#include "paired_reader_quiescence.hpp"
#include "store/store_internal.hpp"
#include "test.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

auto bytes(const std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

} // namespace

GLIFI_TEST("paired Store concurrent GET observes live generation under overwrite storm") {
    // ADR 0036 V2/V3 baseline under production shared_ptr + ReadLease (not slot-pool).
    // Slot-pool landing must keep this class of race green (see ADR 0036 verification matrix).
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    const std::string key = "overwrite-storm";
    GLIFI_REQUIRE(store.put(key, bytes("seed")).has_value());

    std::atomic_bool stop{false};
    glifistore::test::PairedReaderQuiescence quiescence;
    std::atomic_int reader_failure{};
    int writer_failure{};
    std::thread reader{[&] {
        while (!stop.load(std::memory_order_acquire)) {
            quiescence.reader_checkpoint(stop);
            const auto got = store.get(key);
            if (!got.has_value()) {
                reader_failure.store(static_cast<int>(got.error().code) + 1, std::memory_order_relaxed);
                return;
            }
            if (got->bytes.empty()) {
                reader_failure.store(-1, std::memory_order_relaxed);
                return;
            }
        }
    }};
    for (std::size_t write = 0; write < 8'192; ++write) {
        const auto value = "v-" + std::to_string(write);
        auto put = store.put(key, bytes(value));
        if (!put && put.error().code == glifistore::ErrorCode::resource_exhausted) {
            // Counted Reader leases intentionally bound retired generations. If
            // the overwrite storm never exposes a quiescent instant, honor that
            // backpressure and retry the same mutation only after one explicit
            // quiescent hand-off.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            if (!quiescence.request_until(deadline)) {
                writer_failure = -1;
                break;
            }
            put = store.put(key, bytes(value));
            quiescence.release();
        }
        if (!put) {
            writer_failure = static_cast<int>(put.error().code) + 1;
            break;
        }
    }
    stop.store(true, std::memory_order_release);
    reader.join();
    if (const auto failure = reader_failure.load(std::memory_order_relaxed); failure != 0) {
        throw std::runtime_error{"overwrite-storm reader failure " + std::to_string(failure)};
    }
    if (writer_failure != 0) {
        throw std::runtime_error{"overwrite-storm writer failure " + std::to_string(writer_failure)};
    }
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("paired Store put_batch publishes once per shard and keeps RAW") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 2}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    std::vector<std::string> keys;
    std::vector<std::string> values;
    keys.reserve(64);
    values.reserve(64);
    std::vector<glifistore::Store::PutItem> items;
    items.reserve(64);
    for (int index = 0; index < 64; ++index) {
        keys.push_back("batch-key-" + std::to_string(index));
        values.push_back("batch-value-" + std::to_string(index));
        items.push_back(glifistore::Store::PutItem{
            .key = keys.back(),
            .value = bytes(values.back()),
        });
    }

    const auto statuses = store.put_batch(items);
    GLIFI_REQUIRE(statuses.size() == items.size());
    for (const auto& status : statuses) {
        GLIFI_REQUIRE(status.has_value());
    }
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto got = store.get(keys[index]);
        GLIFI_REQUIRE(got.has_value());
        GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) ==
                      values[index]);
    }
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("paired Store put_batch preserves same-key FIFO within one batch") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    const std::string key = "same-key";
    const std::string first = "first";
    const std::string second = "second";
    const std::vector<glifistore::Store::PutItem> items{
        {.key = key, .value = bytes(first)},
        {.key = key, .value = bytes(second)},
    };
    const auto statuses = store.put_batch(items);
    GLIFI_REQUIRE(statuses.size() == 2);
    GLIFI_REQUIRE(statuses[0].has_value());
    GLIFI_REQUIRE(statuses[1].has_value());
    const auto got = store.get(key);
    GLIFI_REQUIRE(got.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) ==
                  second);
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("paired embedded merge pays bounded debt before exhausting a tiny post delta") {
    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .paired = {.merge_delta_entries = 2, .merge_maximum_post_entries = 2, .merge_quantum_slots = 1}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("merge-cut-a", bytes("a")).has_value());
    GLIFI_REQUIRE(store.put("merge-cut-b", bytes("b")).has_value());

    const std::array<glifistore::Store::PutItem, 2> post_items{
        glifistore::Store::PutItem{.key = "merge-post-a", .value = bytes("c")},
        glifistore::Store::PutItem{.key = "merge-post-b", .value = bytes("d")},
    };
    const auto statuses = store.put_batch(post_items);
    GLIFI_REQUIRE(statuses.size() == post_items.size());
    GLIFI_REQUIRE(statuses[0].has_value());
    GLIFI_REQUIRE(statuses[1].has_value());

    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    const auto completed_stats = runtime->stats()[0];
    GLIFI_REQUIRE(completed_stats.read_merge_starts >= 1U);
    GLIFI_REQUIRE(completed_stats.read_merge_completions >= 1U);
    GLIFI_REQUIRE(completed_stats.read_merge_remaining_slots == 0U);
    GLIFI_REQUIRE(completed_stats.read_merge_post_capacity_remaining == 0U);
    GLIFI_REQUIRE(completed_stats.maximum_read_merge_quantum_slots > 1U);

    GLIFI_REQUIRE(store.put("merge-after-post", bytes("e")).has_value());
    const auto final_stats = runtime->stats()[0];
    GLIFI_REQUIRE(final_stats.read_merge_backpressure == 0U);
    GLIFI_REQUIRE(final_stats.generation_admission_backpressure_total == 0U);
    GLIFI_REQUIRE(store.get("merge-cut-a").has_value());
    GLIFI_REQUIRE(store.get("merge-post-b").has_value());
    GLIFI_REQUIRE(store.get("merge-after-post").has_value());
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("paired dedicated Writer merge pays bounded debt before exhausting a tiny post delta") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .paired = {.async_lane_capacity = 8,
                                                      .async_lane_payload_bytes = 64U * 1024U,
                                                      .merge_delta_entries = 2,
                                                      .merge_maximum_post_entries = 2,
                                                      .merge_quantum_slots = 1}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("writer-merge-cut-a", bytes("a")).has_value());
    GLIFI_REQUIRE(store.put("writer-merge-cut-b", bytes("b")).has_value());

    const std::array<glifistore::Store::PutItem, 2> post_items{
        glifistore::Store::PutItem{.key = "writer-merge-post-a", .value = bytes("c")},
        glifistore::Store::PutItem{.key = "writer-merge-post-b", .value = bytes("d")},
    };
    const auto statuses = store.put_batch(post_items);
    GLIFI_REQUIRE(statuses.size() == post_items.size());
    GLIFI_REQUIRE(statuses[0].has_value());
    GLIFI_REQUIRE(statuses[1].has_value());
    GLIFI_REQUIRE(store.put("writer-merge-after-post", bytes("e")).has_value());

    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    const auto stats = runtime->stats()[0];
    GLIFI_REQUIRE(stats.read_merge_starts >= 1U);
    GLIFI_REQUIRE(stats.read_merge_completions >= 1U);
    GLIFI_REQUIRE(stats.read_merge_failures == 0U);
    GLIFI_REQUIRE(stats.read_merge_backpressure == 0U);
    GLIFI_REQUIRE(stats.generation_admission_backpressure_total == 0U);
    GLIFI_REQUIRE(store.get("writer-merge-cut-a").has_value());
    GLIFI_REQUIRE(store.get("writer-merge-post-b").has_value());
    GLIFI_REQUIRE(store.get("writer-merge-after-post").has_value());
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("ADR 0036 production slot V7 embedded merge publishes post-cut under slot pressure") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .paired = {.merge_delta_entries = 2,
                                                      .merge_maximum_post_entries = 2,
                                                      .merge_quantum_slots = 1,
                                                      .generation_slot_pool = true}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);

    std::size_t pinned_retired{};
    {
        glifistore::store::paired::ShardPairRuntime::ReadLease pinned_reader{*runtime, 0};
        GLIFI_REQUIRE(static_cast<bool>(pinned_reader));
        GLIFI_REQUIRE(store.put("slot-merge-cut-a", bytes("a")).has_value());
        GLIFI_REQUIRE(store.put("slot-merge-cut-b", bytes("b")).has_value());

        const std::array<glifistore::Store::PutItem, 2> post_items{
            glifistore::Store::PutItem{.key = "slot-merge-post-a", .value = bytes("c")},
            glifistore::Store::PutItem{.key = "slot-merge-post-b", .value = bytes("d")},
        };
        const auto statuses = store.put_batch(post_items);
        GLIFI_REQUIRE(statuses.size() == post_items.size());
        GLIFI_REQUIRE(statuses[0].has_value());
        GLIFI_REQUIRE(statuses[1].has_value());

        const auto stats = runtime->stats()[0];
        GLIFI_REQUIRE(stats.read_merge_starts >= 1U);
        GLIFI_REQUIRE(stats.read_merge_completions >= 1U);
        GLIFI_REQUIRE(stats.read_merge_remaining_slots == 0U);
        GLIFI_REQUIRE(stats.read_merge_post_capacity_remaining == 0U);
        GLIFI_REQUIRE(stats.maximum_read_merge_quantum_slots > 1U);
        GLIFI_REQUIRE(stats.generation_admission_backpressure_total == 0U);
        GLIFI_REQUIRE(stats.retired_generation_count >= 3U);
        pinned_retired = stats.retired_generation_count;
        GLIFI_REQUIRE(store.get("slot-merge-cut-a").has_value());
        GLIFI_REQUIRE(store.get("slot-merge-post-b").has_value());
    }

    const auto after_quiescence = store.put("slot-merge-after-quiescence", bytes("e"));
    GLIFI_REQUIRE(after_quiescence.has_value());
    const auto resumed_retired = runtime->stats()[0].retired_generation_count;
    GLIFI_REQUIRE(resumed_retired < pinned_retired);
    GLIFI_REQUIRE(runtime->stats()[0].read_merge_active);
    GLIFI_REQUIRE(store.get("slot-merge-after-quiescence").has_value());
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("ADR 0036 production slot V7 dedicated Writer merge publishes post-cut under slot pressure") {
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .paired = {.async_lane_capacity = 8,
                                                      .async_lane_payload_bytes = 64U * 1024U,
                                                      .merge_delta_entries = 2,
                                                      .merge_maximum_post_entries = 2,
                                                      .merge_quantum_slots = 1,
                                                      .reader_epoch_lease = true,
                                                      .generation_slot_pool = true}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);

    GLIFI_REQUIRE(runtime->adopt_read_generation(0) != nullptr);
    GLIFI_REQUIRE(store.put("slot-writer-merge-cut-a", bytes("a")).has_value());
    GLIFI_REQUIRE(store.put("slot-writer-merge-cut-b", bytes("b")).has_value());

    const std::array<glifistore::Store::PutItem, 2> post_items{
        glifistore::Store::PutItem{.key = "slot-writer-merge-post-a", .value = bytes("c")},
        glifistore::Store::PutItem{.key = "slot-writer-merge-post-b", .value = bytes("d")},
    };
    const auto statuses = store.put_batch(post_items);
    GLIFI_REQUIRE(statuses.size() == post_items.size());
    GLIFI_REQUIRE(statuses[0].has_value());
    GLIFI_REQUIRE(statuses[1].has_value());
    GLIFI_REQUIRE(store.put("slot-writer-merge-post-c", bytes("e")).has_value());

    const auto stats = runtime->stats()[0];
    GLIFI_REQUIRE(stats.read_merge_starts >= 1U);
    GLIFI_REQUIRE(stats.read_merge_completions >= 1U);
    GLIFI_REQUIRE(stats.read_merge_failures == 0U);
    GLIFI_REQUIRE(stats.read_merge_backpressure == 0U);
    GLIFI_REQUIRE(stats.generation_admission_backpressure_total == 0U);
    GLIFI_REQUIRE(stats.writer_epoch > stats.reader_safe_epoch);
    GLIFI_REQUIRE(store.get("slot-writer-merge-cut-a").has_value());
    GLIFI_REQUIRE(store.get("slot-writer-merge-post-b").has_value());
    GLIFI_REQUIRE(store.get("slot-writer-merge-post-c").has_value());

    GLIFI_REQUIRE(runtime->adopt_read_generation(0) != nullptr);
    const auto adopted = runtime->stats()[0];
    GLIFI_REQUIRE(adopted.reader_safe_epoch == adopted.writer_epoch);
    const auto after_quiescence = store.put("slot-writer-merge-after-quiescence", bytes("f"));
    GLIFI_REQUIRE(after_quiescence.has_value());
    GLIFI_REQUIRE(store.get("slot-writer-merge-after-quiescence").has_value());
    GLIFI_REQUIRE(store.close().has_value());
}

#if defined(GLIFISTORE_FAULT_INJECTION)
GLIFI_TEST("paired durable Writer fail-closes when mutate throws after durable I/O begins") {
    // ADR 0036 V6 durable_sync seam: Site::publish throws after commit + read-generation
    // publish. Client keeps success ACK (RAW); pair sticky-fails. before(write_record)
    // throws stay known-not-committed — see sibling "before-hook throw" litmus.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-paired-fc-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .concurrency = glifistore::StoreConcurrencyMode::paired,
                                           .paired = {.async_lane_capacity = 8,
                                                      .async_lane_payload_bytes = 1U * 1024U * 1024U,
                                                      .reader_epoch_lease = true},
                                           .storage_mode = glifistore::StorageMode::durable_sync,
                                           .data_directory = store_path,
                                           .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                           .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    GLIFI_REQUIRE(store.put("seed", bytes("ok")).has_value());

    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::publish);
    const std::string key_a = "fc-a";
    const std::string key_b = "fc-b";
    const std::vector<glifistore::Store::PutItem> items{
        {.key = key_a, .value = bytes("alpha")},
        {.key = key_b, .value = bytes("beta")},
    };
    const auto statuses = store.put_batch(items);
    glifistore::fault::reset();
    GLIFI_REQUIRE(statuses.size() == items.size());
    // First item: committed+published before throw → success ACK (no inverted RAW).
    GLIFI_REQUIRE(statuses[0].has_value());
    // Second: never Store-entered after sticky → known not committed.
    GLIFI_REQUIRE(!statuses[1].has_value());
    GLIFI_REQUIRE(statuses[1].error().code == glifistore::ErrorCode::resource_exhausted);

    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(!runtime->healthy());

    const auto late = store.put("fc-late", bytes("no"));
    GLIFI_REQUIRE(!late.has_value());
    GLIFI_REQUIRE(late.error().code == glifistore::ErrorCode::unavailable);
    GLIFI_REQUIRE(late.error().message.find("fail-closed") != std::string::npos);
    const auto seed_after = store.get("seed");
    GLIFI_REQUIRE(seed_after.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(seed_after->bytes.data()),
                                   seed_after->bytes.size()) == "ok");
    GLIFI_REQUIRE(store.get(key_a).has_value());
    GLIFI_REQUIRE(!store.get(key_b).has_value());

    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
#endif

GLIFI_TEST("paired durable sync Writer does not success-ACK abandoned unflushed batch siblings") {
    // Distinct keys in one mutate_durable_batch: A returns committed before flush/index,
    // B fails and clears pending_group. A must not keep a clean success ACK (RAW lie).
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-paired-unflush-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    struct ThrowOnSecondArmedWrite final {
        std::atomic_bool armed{false};
        std::atomic_uint64_t writes{0};

        static auto before(void* context, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto* self = static_cast<ThrowOnSecondArmedWrite*>(context);
            if (!self->armed.load(std::memory_order_acquire)) {
                return {};
            }
            if (operation == glifistore::FilesystemOperation::write_record) {
                const auto count = self->writes.fetch_add(1, std::memory_order_relaxed);
                if (count >= 1U) {
                    throw std::bad_alloc{};
                }
            }
            return {};
        }
    } thrower;

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = 1U * 1024U * 1024U,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = store_path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_group = {.max_records = 32, .max_bytes = 65'536, .max_wait_ms = 10, .min_records = 1},
         .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
         .filesystem_hooks = {.context = &thrower, .before = &ThrowOnSecondArmedWrite::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    GLIFI_REQUIRE(store.put("seed", bytes("ok")).has_value());
    thrower.armed.store(true, std::memory_order_release);

    const std::string key_a = "unflush-a";
    const std::string key_b = "unflush-b";
    const std::vector<glifistore::Store::PutItem> items{
        {.key = key_a, .value = bytes("alpha")},
        {.key = key_b, .value = bytes("beta")},
    };
    const auto statuses = store.put_batch(items);
    GLIFI_REQUIRE(statuses.size() == 2);
    GLIFI_REQUIRE(!statuses[0].has_value());
    GLIFI_REQUIRE(!statuses[1].has_value());
    GLIFI_REQUIRE(statuses[0].error().code == glifistore::ErrorCode::unavailable ||
                  statuses[0].error().code == glifistore::ErrorCode::resource_exhausted ||
                  statuses[0].error().code == glifistore::ErrorCode::internal_error);
    GLIFI_REQUIRE(thrower.writes.load(std::memory_order_relaxed) >= 2U);

    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(!runtime->healthy());

    const auto* generation = runtime->adopt_read_generation(0);
    GLIFI_REQUIRE(generation != nullptr);
    const auto view_a =
        generation->prepare_durable({.key = key_a, .hash = glifistore::hash_key_routing(key_a)});
    GLIFI_REQUIRE(!view_a.has_value());

    const auto late = store.put("unflush-late", bytes("no"));
    GLIFI_REQUIRE(!late.has_value());
    GLIFI_REQUIRE(late.error().code == glifistore::ErrorCode::unavailable);

    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

GLIFI_TEST("paired durable sync Writer keeps flushed siblings through orphan commit fail") {
    // Same Writer batch: A alone hits max_bytes → flush+index (durable_through), B appends
    // unflushed, C throws on write_record. commit_writer_batch fails (orphaned pending).
    // A must keep success ACK and appear in the published generation; B/C must not.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-paired-orphan-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    struct ThrowOnThirdArmedWrite final {
        std::atomic_bool armed{false};
        std::atomic_uint64_t writes{0};

        static auto before(void* context, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto* self = static_cast<ThrowOnThirdArmedWrite*>(context);
            if (!self->armed.load(std::memory_order_acquire)) {
                return {};
            }
            if (operation == glifistore::FilesystemOperation::write_record) {
                const auto count = self->writes.fetch_add(1, std::memory_order_relaxed);
                if (count >= 2U) {
                    throw std::bad_alloc{};
                }
            }
            return {};
        }
    } thrower;

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = 1U * 1024U * 1024U,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = store_path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_group = {.max_records = 32, .max_bytes = 256, .max_wait_ms = 60'000, .min_records = 1},
         .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
         .filesystem_hooks = {.context = &thrower, .before = &ThrowOnThirdArmedWrite::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    GLIFI_REQUIRE(store.put("seed", bytes("ok")).has_value());
    thrower.armed.store(true, std::memory_order_release);

    const std::string key_a = "orphan-a";
    const std::string key_b = "orphan-b";
    const std::string key_c = "orphan-c";
    const std::string large_a(300, 'A');
    const std::vector<glifistore::Store::PutItem> items{
        {.key = key_a, .value = bytes(large_a)},
        {.key = key_b, .value = bytes("beta")},
        {.key = key_c, .value = bytes("gamma")},
    };
    const auto statuses = store.put_batch(items);
    GLIFI_REQUIRE(statuses.size() == 3);
    GLIFI_REQUIRE(statuses[0].has_value());
    GLIFI_REQUIRE(!statuses[1].has_value());
    GLIFI_REQUIRE(!statuses[2].has_value());
    GLIFI_REQUIRE(thrower.writes.load(std::memory_order_relaxed) >= 3U);

    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(!runtime->healthy());

    const auto got_a = store.get(key_a);
    GLIFI_REQUIRE(got_a.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(got_a->bytes.data()), got_a->bytes.size()) ==
                  large_a);
    GLIFI_REQUIRE(!store.get(key_b).has_value());

    const auto late = store.put("orphan-late", bytes("no"));
    GLIFI_REQUIRE(!late.has_value());
    GLIFI_REQUIRE(late.error().code == glifistore::ErrorCode::unavailable);

    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

GLIFI_TEST(
    "paired durable sync Writer snapshot-publishes committed sibling after later known-not-committed") {
    // Sync durable_group same-key split: first sub-batch commits+indexes A; second hits
    // before(write_record) throw (known not committed). A keeps success ACK and stays
    // GET-visible. Pair stays healthy — sticky is for post-commit / Store-entered throws.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-paired-sib-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    struct ThrowOnSecondArmedWrite final {
        std::atomic_bool armed{false};
        std::atomic_uint64_t writes{0};

        static auto before(void* context, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto* self = static_cast<ThrowOnSecondArmedWrite*>(context);
            if (!self->armed.load(std::memory_order_acquire)) {
                return {};
            }
            if (operation == glifistore::FilesystemOperation::write_record) {
                const auto count = self->writes.fetch_add(1, std::memory_order_relaxed);
                if (count >= 1U) {
                    throw std::bad_alloc{};
                }
            }
            return {};
        }
    } thrower;

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = 1U * 1024U * 1024U,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = store_path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_group = {.max_records = 32, .max_bytes = 65'536, .max_wait_ms = 10, .min_records = 1},
         .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
         .filesystem_hooks = {.context = &thrower, .before = &ThrowOnSecondArmedWrite::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    GLIFI_REQUIRE(store.put("seed", bytes("ok")).has_value());
    thrower.armed.store(true, std::memory_order_release);

    // Same key forces duplicate-key sub-batch split so the first mutate_durable_batch
    // commit_writer_batch indexes A before the second sub-batch fails.
    const std::string key = "sib-key";
    const std::vector<glifistore::Store::PutItem> items{
        {.key = key, .value = bytes("alpha")},
        {.key = key, .value = bytes("beta")},
    };
    const auto statuses = store.put_batch(items);
    GLIFI_REQUIRE(statuses.size() == 2);
    GLIFI_REQUIRE(statuses[0].has_value());
    GLIFI_REQUIRE(!statuses[1].has_value());
    GLIFI_REQUIRE(statuses[1].error().code == glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(thrower.writes.load(std::memory_order_relaxed) >= 2U);

    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(runtime->healthy());

    const auto got = store.get(key);
    GLIFI_REQUIRE(got.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) ==
                  "alpha");

    thrower.armed.store(false, std::memory_order_release);
    const auto late = store.put("sib-late", bytes("yes"));
    GLIFI_REQUIRE(late.has_value());

    static_cast<void>(store.close());
    std::error_code ignored2;
    std::filesystem::remove_all(root, ignored2);
}

GLIFI_TEST("paired durable sync Writer does not success-ACK unprocessed batch items after drain") {
    // Same-key split: first sub-batch commits; second hits a pre-write before-hook throw;
    // third never starts. Pre-write failures are known not committed (resource_exhausted),
    // not unavailable / false indeterminate.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-paired-unproc-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    struct ThrowOnSecondArmedWrite final {
        std::atomic_bool armed{false};
        std::atomic_uint64_t writes{0};

        static auto before(void* context, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto* self = static_cast<ThrowOnSecondArmedWrite*>(context);
            if (!self->armed.load(std::memory_order_acquire)) {
                return {};
            }
            if (operation == glifistore::FilesystemOperation::write_record) {
                const auto count = self->writes.fetch_add(1, std::memory_order_relaxed);
                if (count >= 1U) {
                    throw std::bad_alloc{};
                }
            }
            return {};
        }
    } thrower;

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = 1U * 1024U * 1024U,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = store_path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_group = {.max_records = 32, .max_bytes = 65'536, .max_wait_ms = 10, .min_records = 1},
         .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
         .filesystem_hooks = {.context = &thrower, .before = &ThrowOnSecondArmedWrite::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    GLIFI_REQUIRE(store.put("seed", bytes("ok")).has_value());
    thrower.armed.store(true, std::memory_order_release);

    const std::string key = "unproc-key";
    const std::vector<glifistore::Store::PutItem> items{
        {.key = key, .value = bytes("alpha")},
        {.key = key, .value = bytes("beta")},
        {.key = key, .value = bytes("gamma")},
    };
    const auto statuses = store.put_batch(items);
    GLIFI_REQUIRE(statuses.size() == 3);
    GLIFI_REQUIRE(statuses[0].has_value());
    GLIFI_REQUIRE(!statuses[1].has_value());
    // before(write_record) throw never crossed the durable write boundary.
    GLIFI_REQUIRE(statuses[1].error().code == glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(!statuses[2].has_value());
    GLIFI_REQUIRE(statuses[2].error().code == glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(thrower.writes.load(std::memory_order_relaxed) >= 2U);

    const auto got = store.get(key);
    GLIFI_REQUIRE(got.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) ==
                  "alpha");

    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

#if defined(GLIFISTORE_FAULT_INJECTION)
GLIFI_TEST("paired durable sync single-op keeps known-not-committed after record write poison") {
    // append_record: pwrite failure poisons the data directory and returns
    // SegmentCommitOutcome::not_committed. Catalog goes fail-closed via
    // !directory_.healthy(); Writer rewrites to resource_exhausted. When the
    // fail-closed epilogue drain also fails (Site::drain_snapshot), the old
    // epilogue overwrote that resolved error to unavailable (wire INTERNAL_ERROR).
    // Keep known-not-committed polarity — matching catch / durable_group sync.
    auto pattern =
        (std::filesystem::temp_directory_path() / "glifistore-paired-write-poison-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    struct FailArmedRecordWrite final {
        std::atomic_bool armed{false};

        static auto write_some_at(void* context, const int descriptor, const std::span<const std::byte> bytes,
                                  const std::uint64_t offset) -> std::ptrdiff_t {
            auto* self = static_cast<FailArmedRecordWrite*>(context);
            if (self->armed.load(std::memory_order_acquire)) {
                errno = EIO;
                return -1;
            }
            return static_cast<std::ptrdiff_t>(
                ::pwrite(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset)));
        }
    } io;

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = 1U * 1024U * 1024U,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = store_path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
         .filesystem_hooks = {
             .file_io = {.context = &io, .write_some_at = &FailArmedRecordWrite::write_some_at}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    GLIFI_REQUIRE(store.put("seed", bytes("ok")).has_value());
    io.armed.store(true, std::memory_order_release);
    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::drain_snapshot);

    const auto put = store.put("poisoned", bytes("no"));
    glifistore::fault::reset();
    GLIFI_REQUIRE(!put.has_value());
    GLIFI_REQUIRE(put.error().code == glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(put.error().code != glifistore::ErrorCode::unavailable);
    GLIFI_REQUIRE(!store.get("poisoned").has_value());

    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(!runtime->healthy());

    const auto late = store.put("late", bytes("no"));
    GLIFI_REQUIRE(!late.has_value());
    GLIFI_REQUIRE(late.error().code == glifistore::ErrorCode::unavailable);

    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
#endif

GLIFI_TEST("durable before-hook throw before write_record is known not committed") {
    // A throwing filesystem before(write_record) must not become indeterminate /
    // INTERNAL_ERROR — the hook runs ahead of write_all_at.
    auto pattern =
        (std::filesystem::temp_directory_path() / "glifistore-paired-before-throw-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    struct ThrowOnWriteRecord final {
        static auto before(void* /*context*/, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            if (operation == glifistore::FilesystemOperation::write_record) {
                throw std::runtime_error{"injected before-hook failure"};
            }
            return {};
        }
    };

    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .concurrency = glifistore::StoreConcurrencyMode::paired,
                                           .paired = {.async_lane_capacity = 8,
                                                      .async_lane_payload_bytes = 1U * 1024U * 1024U,
                                                      .reader_epoch_lease = true},
                                           .storage_mode = glifistore::StorageMode::durable_sync,
                                           .data_directory = store_path,
                                           .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                           .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
                                           .filesystem_hooks = {.before = &ThrowOnWriteRecord::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    const auto put = store.put("before-throw", bytes("no"));
    GLIFI_REQUIRE(!put.has_value());
    GLIFI_REQUIRE(put.error().code == glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(!store.get("before-throw").has_value());

    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

#if defined(GLIFISTORE_FAULT_INJECTION)
GLIFI_TEST("paired durable_group pre-mutate batch alloc stays known not committed") {
    // Writer sets durable_mutate_entered before mutate_durable_batch. A throw from
    // that call before any durable mutate must not escape as Writer catch sticky /
    // unavailable (wire INTERNAL_ERROR). StoreAccess converts it to not_committed
    // resource_exhausted and keeps the pair healthy.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-paired-batch-pre-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = 1U * 1024U * 1024U,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = store_path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_group = {.max_records = 32, .max_bytes = 65'536, .max_wait_ms = 10, .min_records = 1},
         .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);

    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::durable_batch_pre);
    const std::vector<glifistore::Store::PutItem> items{
        {.key = "pre-a", .value = bytes("alpha")},
        {.key = "pre-b", .value = bytes("beta")},
    };
    const auto statuses = store.put_batch(items);
    glifistore::fault::reset();
    GLIFI_REQUIRE(statuses.size() == 2);
    for (const auto& status : statuses) {
        GLIFI_REQUIRE(!status.has_value());
        GLIFI_REQUIRE(status.error().code == glifistore::ErrorCode::resource_exhausted);
        GLIFI_REQUIRE(status.error().code != glifistore::ErrorCode::unavailable);
    }
    GLIFI_REQUIRE(!store.get("pre-a").has_value());
    GLIFI_REQUIRE(!store.get("pre-b").has_value());
    GLIFI_REQUIRE(runtime->healthy());

    const auto retry = store.put("pre-a", bytes("ok"));
    GLIFI_REQUIRE(retry.has_value());
    const auto got = store.get("pre-a");
    GLIFI_REQUIRE(got.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) ==
                  "ok");

    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

GLIFI_TEST("paired durable_group post-mutate catch keeps Store-entered siblings unavailable") {
    // mutate_durable_batch commits the sub-batch then Site::post_mutate throws before
    // classification finishes. Those Store-entered items must not become
    // resource_exhausted (wire OVERLOADED) while drain still makes them GET-visible.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-paired-post-mutate-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = 1U * 1024U * 1024U,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = store_path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_group = {.max_records = 32, .max_bytes = 65'536, .max_wait_ms = 10, .min_records = 1},
         .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::post_mutate);
    const std::vector<glifistore::Store::PutItem> items{
        {.key = "pm-a", .value = bytes("alpha")},
        {.key = "pm-b", .value = bytes("beta")},
        {.key = "pm-c", .value = bytes("gamma")},
    };
    const auto statuses = store.put_batch(items);
    glifistore::fault::reset();
    GLIFI_REQUIRE(statuses.size() == 3);
    for (const auto& status : statuses) {
        GLIFI_REQUIRE(!status.has_value());
        // Store-entered (mutate returned) → unavailable, never resource_exhausted/OVERLOADED.
        GLIFI_REQUIRE(status.error().code == glifistore::ErrorCode::unavailable);
        GLIFI_REQUIRE(status.error().code != glifistore::ErrorCode::resource_exhausted);
    }

    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(!runtime->healthy());

    // Drain publishes Index authority; siblings must be GET-visible with non-OVERLOADED polarity.
    for (const auto* key : {"pm-a", "pm-b", "pm-c"}) {
        GLIFI_REQUIRE(store.get(key).has_value());
    }

    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
#endif

GLIFI_TEST("paired durable sync catch drain does not success-ACK put-hit on pre-existing key") {
    // Attempted put that fails before write must not success-ACK solely because the key
    // was already Index-visible from a prior put (false ACK with unchanged value).
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-paired-puthit-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    struct ThrowOnFirstArmedWrite final {
        std::atomic_bool armed{false};

        static auto before(void* context, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto* self = static_cast<ThrowOnFirstArmedWrite*>(context);
            if (!self->armed.load(std::memory_order_acquire)) {
                return {};
            }
            if (operation == glifistore::FilesystemOperation::write_record) {
                throw std::bad_alloc{};
            }
            return {};
        }
    } thrower;

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = 1U * 1024U * 1024U,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = store_path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_group = {.max_records = 32, .max_bytes = 65'536, .max_wait_ms = 10, .min_records = 1},
         .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
         .filesystem_hooks = {.context = &thrower, .before = &ThrowOnFirstArmedWrite::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    const std::string key = "put-hit-key";
    GLIFI_REQUIRE(store.put(key, bytes("old")).has_value());
    thrower.armed.store(true, std::memory_order_release);

    // put_batch takes the sync durable batch Writer path (ack_attempted lived there).
    const std::vector<glifistore::Store::PutItem> items{{.key = key, .value = bytes("new")}};
    const auto statuses = store.put_batch(items);
    GLIFI_REQUIRE(statuses.size() == 1);
    GLIFI_REQUIRE(!statuses[0].has_value());
    // before(write_record) throw is known not committed — not sticky unavailable.
    GLIFI_REQUIRE(statuses[0].error().code == glifistore::ErrorCode::resource_exhausted);

    const auto got = store.get(key);
    GLIFI_REQUIRE(got.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) ==
                  "old");

    static_cast<void>(store.close());
    std::error_code ignored2;
    std::filesystem::remove_all(root, ignored2);
}

#if defined(GLIFISTORE_FAULT_INJECTION)
GLIFI_TEST("paired volatile pre-append rotation failure is known not committed") {
    // Pre-append invalid_reference (rotation/catalog) must rewrite to
    // resource_exhausted → wire OVERLOADED — not INTERNAL_ERROR / reconcile.
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .concurrency = glifistore::StoreConcurrencyMode::paired,
                                           .paired = {.async_lane_capacity = 8,
                                                      .async_lane_payload_bytes = 1U * 1024U * 1024U,
                                                      .reader_epoch_lease = true},
                                           .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);

    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::rotate);
    const auto put = store.put("pre-append", bytes("no"));
    glifistore::fault::reset();
    GLIFI_REQUIRE(!put.has_value());
    GLIFI_REQUIRE(put.error().code == glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(put.error().code != glifistore::ErrorCode::invalid_reference);
    GLIFI_REQUIRE(put.error().code != glifistore::ErrorCode::unavailable);
    GLIFI_REQUIRE(!store.get("pre-append").has_value());
    GLIFI_REQUIRE(runtime->healthy());

    const auto retry = store.put("pre-append", bytes("yes"));
    GLIFI_REQUIRE(retry.has_value());
    const auto got = store.get("pre-append");
    GLIFI_REQUIRE(got.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) ==
                  "yes");
    static_cast<void>(store.close());
}

GLIFI_TEST("paired durable pre-write rotation failure is known not committed") {
    // rotate_active runs only after this PUT's append returned segment_full
    // (not_committed). A throw before seal/create/publish must stay not_committed →
    // resource_exhausted — not indeterminate sticky INTERNAL_ERROR.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-paired-rot-pre-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    struct SegmentFullOnce final {
        std::size_t write_records{};

        static auto before(void* opaque, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto& state = *static_cast<SegmentFullOnce*>(opaque);
            if (operation != glifistore::FilesystemOperation::write_record) {
                return {};
            }
            ++state.write_records;
            if (state.write_records == 1) {
                return glifistore::fail(glifistore::ErrorCode::segment_full, "injected segment full");
            }
            return {};
        }
    } failure;

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = 1U * 1024U * 1024U,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = store_path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
         .filesystem_hooks = {.context = &failure, .before = &SegmentFullOnce::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);

    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::rotate);
    const auto put = store.put("rot-pre", bytes("no"));
    glifistore::fault::reset();
    GLIFI_REQUIRE(!put.has_value());
    GLIFI_REQUIRE(put.error().code == glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(put.error().code != glifistore::ErrorCode::unavailable);
    GLIFI_REQUIRE(failure.write_records >= 1);
    GLIFI_REQUIRE(!store.get("rot-pre").has_value());
    GLIFI_REQUIRE(runtime->healthy());

    const auto retry = store.put("rot-pre", bytes("yes"));
    GLIFI_REQUIRE(retry.has_value());
    const auto got = store.get("rot-pre");
    GLIFI_REQUIRE(got.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) ==
                  "yes");

    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

GLIFI_TEST("paired durable post-seal rotation reader-open is sticky indeterminate") {
    // After rotate_active commits a seal, sealed-reader open failure must stay
    // indeterminate / unavailable (fail-closed) — not known-not-committed OVERLOADED.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-paired-rot-seal-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    struct SegmentFullOnSecond final {
        std::size_t write_records{};

        static auto before(void* opaque, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto& state = *static_cast<SegmentFullOnSecond*>(opaque);
            if (operation != glifistore::FilesystemOperation::write_record) {
                return {};
            }
            ++state.write_records;
            if (state.write_records == 2) {
                return glifistore::fail(glifistore::ErrorCode::segment_full, "injected segment full");
            }
            return {};
        }
    } failure;

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = 1U * 1024U * 1024U,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = store_path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
         .filesystem_hooks = {.context = &failure, .before = &SegmentFullOnSecond::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);

    const auto warm = store.put("rot-seal-warm", bytes("warm"));
    GLIFI_REQUIRE(warm.has_value());

    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::segment_open);
    const auto put = store.put("rot-seal", bytes("no"));
    glifistore::fault::reset();
    GLIFI_REQUIRE(!put.has_value());
    GLIFI_REQUIRE(put.error().code == glifistore::ErrorCode::unavailable);
    GLIFI_REQUIRE(put.error().code != glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(failure.write_records >= 2);
    GLIFI_REQUIRE(!store.get("rot-seal").has_value());
    GLIFI_REQUIRE(!runtime->healthy());

    const auto late = store.put("rot-seal-late", bytes("no"));
    GLIFI_REQUIRE(!late.has_value());
    GLIFI_REQUIRE(late.error().code == glifistore::ErrorCode::unavailable);

    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

GLIFI_TEST("paired durable post-seal rotation create reject is sticky indeterminate") {
    // After rotate_active commits a seal, a pre-rename create reject (not_published)
    // must stay indeterminate / unavailable — not known-not-committed OVERLOADED.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-paired-rot-create-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    struct SegmentFullThenCreateReject final {
        std::size_t write_records{};
        std::size_t preallocates{};

        static auto before(void* opaque, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto& state = *static_cast<SegmentFullThenCreateReject*>(opaque);
            if (operation == glifistore::FilesystemOperation::write_record) {
                ++state.write_records;
                if (state.write_records == 2) {
                    return glifistore::fail(glifistore::ErrorCode::segment_full, "injected segment full");
                }
                return {};
            }
            if (operation == glifistore::FilesystemOperation::preallocate_segment) {
                ++state.preallocates;
                // Only reject replacement create during rotation (after segment_full),
                // not bootstrap Segment creation.
                if (state.write_records >= 2) {
                    return glifistore::fail(glifistore::ErrorCode::io_error, "injected create reject");
                }
                return {};
            }
            return {};
        }
    } failure;

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .paired = {.async_lane_capacity = 8,
                    .async_lane_payload_bytes = 1U * 1024U * 1024U,
                    .reader_epoch_lease = true},
         .storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = store_path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
         .filesystem_hooks = {.context = &failure, .before = &SegmentFullThenCreateReject::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);

    const auto warm = store.put("rot-create-warm", bytes("warm"));
    GLIFI_REQUIRE(warm.has_value());

    const auto put = store.put("rot-create", bytes("no"));
    GLIFI_REQUIRE(!put.has_value());
    GLIFI_REQUIRE(put.error().code == glifistore::ErrorCode::unavailable);
    GLIFI_REQUIRE(put.error().code != glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(failure.write_records >= 2);
    GLIFI_REQUIRE(failure.preallocates >= 1);
    GLIFI_REQUIRE(!store.get("rot-create").has_value());
    GLIFI_REQUIRE(!runtime->healthy());

    const auto late = store.put("rot-create-late", bytes("no"));
    GLIFI_REQUIRE(!late.has_value());
    GLIFI_REQUIRE(late.error().code == glifistore::ErrorCode::unavailable);

    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

GLIFI_TEST("paired durable pre-append segment open failure is known not committed") {
    // DurableSegmentFile::open before any Record write must stay not_committed →
    // resource_exhausted / wire OVERLOADED — not indeterminate sticky INTERNAL_ERROR.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-paired-seg-open-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .concurrency = glifistore::StoreConcurrencyMode::paired,
                                           .paired = {.async_lane_capacity = 8,
                                                      .async_lane_payload_bytes = 1U * 1024U * 1024U,
                                                      .reader_epoch_lease = true},
                                           .storage_mode = glifistore::StorageMode::durable_sync,
                                           .data_directory = store_path,
                                           .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                           .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);

    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::segment_open);
    const auto put = store.put("seg-open", bytes("no"));
    glifistore::fault::reset();
    GLIFI_REQUIRE(!put.has_value());
    GLIFI_REQUIRE(put.error().code == glifistore::ErrorCode::descriptor_exhausted ||
                  put.error().code == glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(put.error().code != glifistore::ErrorCode::unavailable);
    GLIFI_REQUIRE(!store.get("seg-open").has_value());
    GLIFI_REQUIRE(runtime->healthy());

    const auto retry = store.put("seg-open", bytes("yes"));
    GLIFI_REQUIRE(retry.has_value());
    const auto got = store.get("seg-open");
    GLIFI_REQUIRE(got.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) ==
                  "yes");

    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

GLIFI_TEST("paired volatile post-append index failure stays indeterminate") {
    // After append, Index publication failure must stay unavailable (sticky) —
    // rewrite_known_not_committed must not demote it to OVERLOADED.
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .concurrency = glifistore::StoreConcurrencyMode::paired,
                                           .paired = {.async_lane_capacity = 8,
                                                      .async_lane_payload_bytes = 1U * 1024U * 1024U,
                                                      .reader_epoch_lease = true},
                                           .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);

    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::index_account);
    const auto put = store.put("post-append", bytes("orphan"));
    glifistore::fault::reset();
    GLIFI_REQUIRE(!put.has_value());
    GLIFI_REQUIRE(put.error().code == glifistore::ErrorCode::unavailable);
    GLIFI_REQUIRE(!store.get("post-append").has_value());
    GLIFI_REQUIRE(!runtime->healthy());

    const auto late = store.put("late", bytes("no"));
    GLIFI_REQUIRE(!late.has_value());
    GLIFI_REQUIRE(late.error().code == glifistore::ErrorCode::unavailable);
    static_cast<void>(store.close());
}

GLIFI_TEST("paired volatile exclusive compact gates Index publish") {
    // Exclusive Writer put/erase_locked_published elides mutex_; compact must arm
    // Index quiesce + drain hot_path_depth before Index touch. Sibling put under
    // the gate sees sequence_conflict (rewritten to resource_exhausted on the
    // paired wire) — never a torn Index vs unlocked publish.
    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .concurrency = glifistore::StoreConcurrencyMode::paired,
                                           .paired = {.async_lane_capacity = 8,
                                                      .async_lane_payload_bytes = 1U * 1024U * 1024U,
                                                      .reader_epoch_lease = true},
                                           .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.put("seed", bytes("ok")).has_value());

    glifistore::fault::reset();
    glifistore::fault::arm_block(glifistore::fault::Site::compact);
    glifistore::Result<glifistore::CompactionResult> compacted{
        glifistore::fail(glifistore::ErrorCode::internal_error, "unset")};
    std::thread compactor{[&] { compacted = store.compact(); }};
    GLIFI_REQUIRE(glifistore::fault::wait_until_blocked(glifistore::fault::Site::compact));

    bool saw_gate{};
    const auto gate_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    for (std::uint32_t attempt = 0; std::chrono::steady_clock::now() < gate_deadline; ++attempt) {
        const auto key = std::string{"gated-"} + std::to_string(attempt);
        const auto put = store.put(key, bytes("x"));
        if (!put.has_value() && (put.error().code == glifistore::ErrorCode::sequence_conflict ||
                                 put.error().code == glifistore::ErrorCode::resource_exhausted)) {
            saw_gate = true;
            break;
        }
        GLIFI_REQUIRE(put.has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    GLIFI_REQUIRE(saw_gate);

    glifistore::fault::release_block(glifistore::fault::Site::compact);
    compactor.join();
    glifistore::fault::reset();

    GLIFI_REQUIRE(compacted.has_value());
    GLIFI_REQUIRE(store.put("after", bytes("y")).has_value());
    GLIFI_REQUIRE(store.verify_index().has_value());
    static_cast<void>(store.close());
}
#endif

GLIFI_TEST("ADR 0036 production slot V5 shutdown finalization rejects a live Reader lease") {
    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1}, .paired = {.generation_slot_pool = true}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(!runtime->finalize_reader_shutdown().has_value());

    {
        glifistore::store::paired::ShardPairRuntime::ReadLease lease{*runtime, 0};
        GLIFI_REQUIRE(static_cast<bool>(lease));
        GLIFI_REQUIRE(store.put("adr0036-slot-shutdown", bytes("value")).has_value());
        GLIFI_REQUIRE(runtime->stop_and_drain().has_value());

        const auto blocked = runtime->finalize_reader_shutdown();
        GLIFI_REQUIRE(!blocked.has_value());
        GLIFI_REQUIRE(blocked.error().code == glifistore::ErrorCode::unavailable);
        GLIFI_REQUIRE(runtime->adopt_read_generation(0) != nullptr);
    }

    GLIFI_REQUIRE(runtime->finalize_reader_shutdown().has_value());
    GLIFI_REQUIRE(runtime->adopt_read_generation(0) == nullptr);
    GLIFI_REQUIRE(runtime->stats()[0].reader_shutdown_finalized);
    static_cast<void>(store.close());
}

GLIFI_TEST("paired shutdown reclamation torture leaves every bounded resource terminal") {
    constexpr std::uint64_t kFirstSeed = 0x5A17'0000ULL;
    constexpr std::size_t kSeedCount = 24U;
    const auto require_seed = [](const bool condition, const std::uint64_t seed,
                                 const std::string_view invariant) {
        if (!condition) {
            throw std::runtime_error{"shutdown torture seed=" + std::to_string(seed) +
                                     " invariant=" + std::string{invariant}};
        }
    };

    for (std::size_t iteration = 0; iteration < kSeedCount; ++iteration) {
        const auto seed = kFirstSeed + iteration;
        const bool slot_pool = (seed & 1U) != 0U;
        const bool dedicated_writer = (seed & 2U) != 0U;
        auto opened =
            glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                     .paired = {.async_lane_capacity = dedicated_writer ? 8U : 0U,
                                                .async_lane_payload_bytes = dedicated_writer ? 65'536U : 0U,
                                                .merge_delta_entries = 2U,
                                                .merge_maximum_post_entries = 4U,
                                                .merge_quantum_slots = 1U,
                                                .reader_epoch_lease = false,
                                                .generation_slot_pool = slot_pool}});
        require_seed(opened.has_value(), seed, "open");
        auto& store = **opened;
        auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
        require_seed(runtime != nullptr, seed, "paired runtime exists");

        // Unique-key growth, same-key replacement, erasure, merge churn, and a
        // close immediately following the final accepted mutation.
        for (std::size_t operation = 0; operation < 12U; ++operation) {
            const auto key = "shutdown-" + std::to_string(operation);
            const auto value = "seed-" + std::to_string(seed) + "-" + std::to_string(operation);
            require_seed(store.put(key, bytes(value)).has_value(), seed, "put");
            require_seed(store.get(key).has_value(), seed, "get after put");
            if ((operation % 3U) == 0U) {
                require_seed(store.erase(key).has_value(), seed, "erase");
                require_seed(!store.get(key).has_value(), seed, "get after erase");
            }
        }
        require_seed(store.put("shutdown-tail", bytes("tail")).has_value(), seed, "tail put");

        if ((seed & 4U) != 0U) {
            glifistore::store::paired::ShardPairRuntime::ReadLease lease{*runtime, 0};
            require_seed(static_cast<bool>(lease), seed, "live Reader lease");
            require_seed(runtime->stop_and_drain().has_value(), seed, "stop and drain with lease");
            const auto blocked = runtime->finalize_reader_shutdown();
            require_seed(!blocked.has_value() && blocked.error().code == glifistore::ErrorCode::unavailable,
                         seed, "live lease blocks final reclaim");
        } else {
            require_seed(runtime->stop_and_drain().has_value(), seed, "stop and drain");
        }

        require_seed(runtime->finalize_reader_shutdown().has_value(), seed, "finalize Reader shutdown");
        const auto stats = runtime->stats();
        require_seed(stats.size() == 1U, seed, "one shard stats row");
        const auto& lane = stats.front();
        require_seed(lane.reader_shutdown_finalized, seed, "Reader terminal state");
        require_seed(lane.retired_generation_count == 0U, seed, "retired generations reclaimed");
        require_seed(!lane.read_merge_active, seed, "merge abandoned or complete");
        require_seed(lane.queue_depth == 0U && lane.queued_bytes == 0U, seed, "mutation queue drained");
        require_seed(lane.payload_slots_in_use == 0U && lane.payload_arena_bytes_in_use == 0U &&
                         lane.payload_admission_bytes_in_use == 0U,
                     seed, "payload ownership returned");
        require_seed(store.close().has_value(), seed, "Store close");
    }
}

GLIFI_TEST("ADR 0036 production slot V10 put_batch preserves same-key FIFO within one batch") {
    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1}, .paired = {.generation_slot_pool = true}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    const std::vector<glifistore::Store::PutItem> items{
        {.key = "adr0036-fifo", .value = bytes("one")},
        {.key = "adr0036-fifo", .value = bytes("two")},
        {.key = "adr0036-fifo", .value = bytes("three")},
    };
    const auto statuses = store.put_batch(items);
    GLIFI_REQUIRE(statuses.size() == items.size());
    for (const auto& status : statuses) {
        GLIFI_REQUIRE(status.has_value());
    }
    const auto got = store.get("adr0036-fifo");
    GLIFI_REQUIRE(got.has_value());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) ==
                  "three");
    static_cast<void>(store.close());
}
