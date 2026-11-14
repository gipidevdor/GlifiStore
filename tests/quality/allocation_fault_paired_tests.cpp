#include "allocation_fault_test_support.hpp"
#include "allocation_fault_tests_decl.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/server/server.hpp"

namespace allocation_fault_test {
void run_paired_async_durable_coalesced_fail_closed() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    // Capture-fail litmus needs the debug fault seam (durable stays healthy).
    return;
#else
    // Two same-key async puts coalesce (min_records=2) then split into durable
    // sub-batches. First commit + capture fail → drain-snapshot + success ACK;
    // Writer must not Store-mutate the later sub-batch.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-async-fc-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(::mkdtemp(writable.data()) != nullptr, "mkdtemp failed");
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    struct WriteCounter final {
        std::atomic_uint64_t writes{0};

        static auto before(void* context, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto* self = static_cast<WriteCounter*>(context);
            if (operation == glifistore::FilesystemOperation::write_record) {
                self->writes.fetch_add(1, std::memory_order_relaxed);
            }
            return {};
        }
    } counter;

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
        .storage_mode = glifistore::StorageMode::durable_group,
        .data_directory = store_path,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .durable_group = {.max_records = 32, .max_bytes = 65'536, .max_wait_ms = 100, .min_records = 2},
        .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
        .filesystem_hooks = {.context = &counter, .before = &WriteCounter::before},
    });
    require(opened.has_value(), "failed to open paired durable_group Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    glifistore::server::BoundedSpscQueue<glifistore::server::MutationCompletion> completions{8};
    auto wakeup = glifistore::server::Wakeup::create();
    require(wakeup.has_value(), "Wakeup::create failed");
    auto executor = glifistore::server::PairWriterPool::create(store, 1, 8, 1U * 1024U * 1024U,
                                                               std::chrono::milliseconds{0});
    require(executor.has_value(), "PairWriterPool::create failed");
    require((*executor)->start().has_value(), "PairWriterPool::start failed");

    const std::string key = "async-fc";
    const auto key_hash = glifistore::hash_key(key);
    const auto baseline_writes = counter.writes.load(std::memory_order_relaxed);

    glifistore::fault::fail_once(glifistore::fault::Site::capture);
    require((*executor)
                ->try_submit({
                    .connection = {.slot = 1, .generation = 1},
                    .request_id = 1,
                    .worker_index = 0,
                    .kind = glifistore::server::MutationKind::put,
                    .key = bytes(key),
                    .key_hash = key_hash,
                    .value = bytes("first"),
                    .completions = &completions,
                    .wakeup = &*wakeup,
                })
                .has_value(),
            "first async put submit failed");
    require((*executor)
                ->try_submit({
                    .connection = {.slot = 2, .generation = 1},
                    .request_id = 2,
                    .worker_index = 0,
                    .kind = glifistore::server::MutationKind::put,
                    .key = bytes(key),
                    .key_hash = key_hash,
                    .value = bytes("second"),
                    .completions = &completions,
                    .wakeup = &*wakeup,
                })
                .has_value(),
            "second async put submit failed");

    std::array<std::optional<glifistore::server::MutationCompletion>, 2> done{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while ((!done[0] || !done[1]) && std::chrono::steady_clock::now() < deadline) {
        if (auto completion = completions.try_pop()) {
            const auto slot = completion->request_id == 1 ? 0U : 1U;
            require(!done[slot].has_value(), "duplicate completion");
            done[slot] = std::move(*completion);
            require((*executor)->release_payload(0, done[slot]->payload_slot), "release_payload failed");
        } else {
            static_cast<void>((*executor)->adopt_read_generation(0));
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    glifistore::fault::reset();
    require(done[0].has_value() && done[1].has_value(), "async completions timed out");
    require(!runtime->healthy(), "capture publication failure did not sticky-fail the pair");
    // First: ACK-after-drain. Second: never mutated after sticky (or reject).
    require(!done[0]->error.has_value(),
            "first clean commit kept error ACK after successful drain (inverted RAW)");
    require(done[1]->error.has_value(), "later same-key sub-batch must not success-ACK after fail-closed");
    require(done[1]->error->code == glifistore::ErrorCode::resource_exhausted,
            "pre-Store sibling fail-closed must be resource_exhausted (not unavailable/reconcile)");
    const auto armed_writes = counter.writes.load(std::memory_order_relaxed) - baseline_writes;
    require(armed_writes <= 1U,
            "async durable Writer mutated a later sub-batch after post-commit fail-closed");

    const auto got = store.get(key);
    require(got.has_value(), "Store::get missed drain-snapshotted first value");
    require(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) == "first",
            "drain-snapshotted value mismatch");

    const auto late = store.put("async-fc-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    require(late.error().message.find("fail-closed") != std::string::npos,
            "late put did not hit pair fail-closed reject");
    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
#endif
}

void run_paired_async_durable_sibling_publish_after_capture_fail() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Two distinct same-shard keys coalesce into one Writer batch. Key A stages
    // successfully; key B's capture fails. Both clean commits must success-ACK and
    // become GET-visible via durable snapshot publish; pair sticky-fails.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-async-sib-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(::mkdtemp(writable.data()) != nullptr, "mkdtemp failed");
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
        .storage_mode = glifistore::StorageMode::durable_group,
        .data_directory = store_path,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .durable_group = {.max_records = 32, .max_bytes = 65'536, .max_wait_ms = 100, .min_records = 2},
        .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
    });
    require(opened.has_value(), "failed to open paired durable_group Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    glifistore::server::BoundedSpscQueue<glifistore::server::MutationCompletion> completions{8};
    auto wakeup = glifistore::server::Wakeup::create();
    require(wakeup.has_value(), "Wakeup::create failed");
    auto executor = glifistore::server::PairWriterPool::create(store, 1, 8, 1U * 1024U * 1024U,
                                                               std::chrono::milliseconds{0});
    require(executor.has_value(), "PairWriterPool::create failed");
    require((*executor)->start().has_value(), "PairWriterPool::start failed");

    const std::string key_a = "async-sib-a";
    const std::string key_b = "async-sib-b";
    glifistore::fault::fail_nth(glifistore::fault::Site::capture, 2);
    require((*executor)
                ->try_submit({
                    .connection = {.slot = 1, .generation = 1},
                    .request_id = 1,
                    .worker_index = 0,
                    .kind = glifistore::server::MutationKind::put,
                    .key = bytes(key_a),
                    .key_hash = glifistore::hash_key(key_a),
                    .value = bytes("alpha"),
                    .completions = &completions,
                    .wakeup = &*wakeup,
                })
                .has_value(),
            "first async put submit failed");
    require((*executor)
                ->try_submit({
                    .connection = {.slot = 2, .generation = 1},
                    .request_id = 2,
                    .worker_index = 0,
                    .kind = glifistore::server::MutationKind::put,
                    .key = bytes(key_b),
                    .key_hash = glifistore::hash_key(key_b),
                    .value = bytes("beta"),
                    .completions = &completions,
                    .wakeup = &*wakeup,
                })
                .has_value(),
            "second async put submit failed");

    std::array<std::optional<glifistore::server::MutationCompletion>, 2> done{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while ((!done[0] || !done[1]) && std::chrono::steady_clock::now() < deadline) {
        if (auto completion = completions.try_pop()) {
            const auto slot = completion->request_id == 1 ? 0U : 1U;
            require(!done[slot].has_value(), "duplicate completion");
            done[slot] = std::move(*completion);
            require((*executor)->release_payload(0, done[slot]->payload_slot), "release_payload failed");
        } else {
            static_cast<void>((*executor)->adopt_read_generation(0));
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    glifistore::fault::reset();
    require(done[0].has_value() && done[1].has_value(), "async completions timed out");
    require(!runtime->healthy(), "capture failure did not sticky-fail the pair");
    require(!done[0]->error.has_value(),
            "earlier committed sibling was aborted instead of snapshot-published");
    // ACK-after-publish: capture-failed sibling is drain-snapshotted — success ACK + GET.
    require(!done[1]->error.has_value(),
            "capture-failed committed sibling kept error ACK after successful drain (inverted RAW)");

    const auto got_a = store.get(key_a);
    require(got_a.has_value(), "Store::get rejected published sibling A after fail-closed");
    require(std::string_view(reinterpret_cast<const char*>(got_a->bytes.data()), got_a->bytes.size()) ==
                "alpha",
            "published sibling A value mismatch");
    const auto got_b = store.get(key_b);
    require(got_b.has_value(), "Store::get rejected drain-snapshotted sibling B after fail-closed");
    require(std::string_view(reinterpret_cast<const char*>(got_b->bytes.data()), got_b->bytes.size()) ==
                "beta",
            "published sibling B value mismatch");

    const auto late = store.put("async-sib-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    require(late.error().message.find("fail-closed") != std::string::npos,
            "late put did not hit pair fail-closed reject");
    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
#endif
}

void run_paired_durable_batch_stops_after_indeterminate_ttl() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Deferred TTL drain failure on the first mutate must sticky-fail durable and
    // reject later siblings in the same Writer batch (no further appends).
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-ttl-stop-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(::mkdtemp(writable.data()) != nullptr, "mkdtemp failed");
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

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

    const auto clock = std::make_shared<ManualStoreClock>(50);

    struct WriteCounter final {
        std::atomic_uint64_t writes{0};

        static auto before(void* context, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto* self = static_cast<WriteCounter*>(context);
            if (operation == glifistore::FilesystemOperation::write_record) {
                self->writes.fetch_add(1, std::memory_order_relaxed);
            }
            return {};
        }
    } counter;

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
        .storage_mode = glifistore::StorageMode::durable_group,
        .data_directory = store_path,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .durable_group = {.max_records = 32, .max_bytes = 65'536, .max_wait_ms = 10, .min_records = 1},
        .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
        .clock = clock,
        .filesystem_hooks = {.context = &counter, .before = &WriteCounter::before},
    });
    require(opened.has_value(), "failed to open paired durable_group Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    // Seed a deferred TTL reclaim via expired GET (same path as production drain).
    require(store.put("ttl-seed", bytes("stale"), 100).has_value(), "seed put failed");
    clock->set(100);
    const auto expired = store.get("ttl-seed");
    require(!expired.has_value() && expired.error().code == glifistore::ErrorCode::not_found,
            "expired seed GET did not return not_found");

    const auto baseline_writes = counter.writes.load(std::memory_order_relaxed);
    glifistore::fault::fail_once(glifistore::fault::Site::deferred_ttl);
    const std::string key_a = "ttl-stop-a";
    const std::string key_b = "ttl-stop-b";
    const std::vector<glifistore::Store::PutItem> items{
        {.key = key_a, .value = bytes("alpha")},
        {.key = key_b, .value = bytes("beta")},
    };
    const auto statuses = store.put_batch(items);
    glifistore::fault::reset();
    require(statuses.size() == 2, "put_batch size mismatch");
    require(!statuses[0].has_value() && !statuses[1].has_value(),
            "indeterminate TTL drain left a successful ACK");
    require(!runtime->healthy(), "TTL drain failure did not sticky-fail the pair");
    require(counter.writes.load(std::memory_order_relaxed) == baseline_writes,
            "later sibling appended after sticky TTL drain failure");

    const auto late = store.put("ttl-stop-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
#endif
}

void run_paired_volatile_multichunk_fail_closed() {
    // put_batch of >32 same-shard keys drains as multiple ≤32 sync publish chunks.
    // After the first chunk sticky-fail-closes, later chunks must not mutate/publish/ACK.
    constexpr std::size_t kBatch = 40;
    constexpr std::size_t kMaximumFailAt = 256;
    bool closed{};
    for (std::size_t fail_at = 0; fail_at < kMaximumFailAt; ++fail_at) {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .concurrency = glifistore::StoreConcurrencyMode::paired,
            .paired = {.async_lane_capacity = 8,
                       .async_lane_payload_bytes = 1U * 1024U * 1024U,
                       .reader_epoch_lease = true},
        });
        require(opened.has_value(), "failed to open paired volatile Store");
        auto& store = **opened;
        auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
        require(runtime != nullptr, "missing paired runtime");
        require(store.put("seed", bytes("ok")).has_value(), "seed put failed");

        std::vector<std::string> keys;
        std::vector<std::string> values;
        std::vector<glifistore::Store::PutItem> items;
        keys.reserve(kBatch);
        values.reserve(kBatch);
        items.reserve(kBatch);
        for (std::size_t index = 0; index < kBatch; ++index) {
            keys.push_back("mc-" + std::to_string(index));
            values.push_back("v-" + std::to_string(index));
            items.push_back(glifistore::Store::PutItem{.key = keys.back(), .value = bytes(values.back())});
        }

        allocation_fault::arm_process(fail_at);
        const auto statuses = store.put_batch(items);
        allocation_fault::disarm_process();
        require(statuses.size() == kBatch, "put_batch status size mismatch");

        if (runtime->healthy()) {
            static_cast<void>(store.close());
            continue;
        }

        closed = true;
        bool saw_failure = false;
        for (std::size_t index = 0; index < statuses.size(); ++index) {
            if (!statuses[index].has_value()) {
                saw_failure = true;
                continue;
            }
            // No success is allowed after the first failure in FIFO batch order.
            require(!saw_failure, "put_batch succeeded after an earlier sticky failure");
        }
        require(saw_failure, "fail-closed without any failed batch status");

        for (std::size_t index = 0; index < statuses.size(); ++index) {
            if (statuses[index].has_value()) {
                continue;
            }
            const auto got = store.get(keys[index]);
            require(!got.has_value(), "failed batch key became GET-visible after sticky fail-closed");
        }

        const auto late = store.put("mc-late", bytes("no"));
        require(!late.has_value(), "late put accepted after sticky fail-closed");
        require(late.error().code == glifistore::ErrorCode::unavailable,
                "late put was not unavailable after sticky fail-closed");
        require(late.error().message.find("fail-closed") != std::string::npos,
                "late put did not hit pair fail-closed reject");
        static_cast<void>(store.close());
        break;
    }
    require(closed, "paired volatile multichunk fail-closed never tripped");
}

void run_paired_durable_sync_multichunk_fail_closed() {
    // put_batch of >32 same-shard keys on durable_sync spans Writer sync turns
    // (≤32). After the first turn sticky-fail-closes under process allocation
    // injection, later turns must not success-ACK.
    constexpr std::size_t kBatch = 40;
    constexpr std::size_t kMaximumFailAt = 256;
    bool closed{};
    for (std::size_t fail_at = 0; fail_at < kMaximumFailAt; ++fail_at) {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-mc-dur-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        require(::mkdtemp(writable.data()) != nullptr, "mkdtemp failed");
        const std::filesystem::path root{writable.data()};
        const auto store_path = root / "store";

        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .concurrency = glifistore::StoreConcurrencyMode::paired,
            .paired = {.async_lane_capacity = 8,
                       .async_lane_payload_bytes = 1U * 1024U * 1024U,
                       .reader_epoch_lease = true},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = store_path,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
            .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
        });
        require(opened.has_value(), "failed to open paired durable_sync Store");
        auto& store = **opened;
        auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
        require(runtime != nullptr, "missing paired runtime");
        require(store.put("seed", bytes("ok")).has_value(), "seed put failed");

        std::vector<std::string> keys;
        std::vector<std::string> values;
        std::vector<glifistore::Store::PutItem> items;
        keys.reserve(kBatch);
        values.reserve(kBatch);
        items.reserve(kBatch);
        for (std::size_t index = 0; index < kBatch; ++index) {
            keys.push_back("mc-d-" + std::to_string(index));
            values.push_back("v-" + std::to_string(index));
            items.push_back(glifistore::Store::PutItem{.key = keys.back(), .value = bytes(values.back())});
        }

        allocation_fault::arm_process(fail_at);
        const auto statuses = store.put_batch(items);
        allocation_fault::disarm_process();
        require(statuses.size() == kBatch, "put_batch status size mismatch");

        if (runtime->healthy()) {
            static_cast<void>(store.close());
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
            continue;
        }

        closed = true;
        bool saw_failure = false;
        for (std::size_t index = 0; index < statuses.size(); ++index) {
            if (!statuses[index].has_value()) {
                saw_failure = true;
                continue;
            }
            require(!saw_failure, "durable put_batch succeeded after an earlier sticky failure");
        }
        require(saw_failure, "durable fail-closed without any failed batch status");

        for (std::size_t index = 0; index < statuses.size(); ++index) {
            if (statuses[index].has_value()) {
                continue;
            }
            const auto got = store.get(keys[index]);
            require(!got.has_value(), "failed durable batch key became GET-visible after sticky");
        }

        const auto late = store.put("mc-d-late", bytes("no"));
        require(!late.has_value(), "late put accepted after durable sticky fail-closed");
        require(late.error().code == glifistore::ErrorCode::unavailable,
                "late put was not unavailable after durable sticky fail-closed");
        require(late.error().message.find("fail-closed") != std::string::npos,
                "late put did not hit pair fail-closed reject");
        static_cast<void>(store.close());
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        break;
    }
    require(closed, "paired durable_sync multichunk fail-closed never tripped");
}

void run_paired_volatile_sync_midchunk_fail_closed_resource_exhausted() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Sync volatile put_batch: after the first same-shard item mutates, Site::mutate
    // sticky-closes the pair. The later sibling never enters Store and must be
    // resource_exhausted (rejected), not unavailable (reconcile / INTERNAL_ERROR).
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
    });
    require(opened.has_value(), "failed to open paired volatile Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    const std::string key_a = "mid-a";
    const std::string key_b = "mid-b";
    const std::vector<glifistore::Store::PutItem> items{
        {.key = key_a, .value = bytes("alpha")},
        {.key = key_b, .value = bytes("beta")},
    };

    glifistore::fault::fail_once(glifistore::fault::Site::mutate);
    const auto statuses = store.put_batch(items);
    glifistore::fault::reset();
    require(statuses.size() == 2, "put_batch size mismatch");
    require(statuses[0].has_value(), "first mid-chunk put lost success ACK after publication");
    require(!statuses[1].has_value(), "later mid-chunk sibling must not success-ACK after sticky");
    require(statuses[1].error().code == glifistore::ErrorCode::resource_exhausted,
            "mid-chunk never-Store-entered sibling must be resource_exhausted");
    require(statuses[1].error().message.find("fail-closed") != std::string::npos,
            "mid-chunk sibling did not hit fail-closed reject");
    require(!runtime->healthy(), "Site::mutate sticky did not fail-close the pair");

    const auto got_a = store.get(key_a);
    require(got_a.has_value(), "Store::get missed published first mid-chunk key");
    require(std::string_view(reinterpret_cast<const char*>(got_a->bytes.data()), got_a->bytes.size()) ==
                "alpha",
            "published mid-chunk value mismatch");
    const auto got_b = store.get(key_b);
    require(!got_b.has_value(), "never-Store-entered mid-chunk sibling became GET-visible");

    const auto late = store.put("mid-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
#endif
}

void run_paired_volatile_sync_midchunk_catch_preserves_resource_exhausted() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Mid-chunk sticky stamps the never-entered sibling resource_exhausted; a later
    // Site::publish catch must not upgrade that to unavailable (false indeterminate).
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
    });
    require(opened.has_value(), "failed to open paired volatile Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    const std::string key_a = "mid-catch-a";
    const std::string key_b = "mid-catch-b";
    const std::vector<glifistore::Store::PutItem> items{
        {.key = key_a, .value = bytes("alpha")},
        {.key = key_b, .value = bytes("beta")},
    };

    glifistore::fault::fail_once(glifistore::fault::Site::mutate);
    glifistore::fault::fail_once(glifistore::fault::Site::publish);
    const auto statuses = store.put_batch(items);
    glifistore::fault::reset();
    require(statuses.size() == 2, "put_batch size mismatch");
    require(statuses[0].has_value(), "first mid-chunk put lost success ACK after publish-then-catch");
    require(!statuses[1].has_value(), "later mid-chunk sibling must not success-ACK after sticky");
    require(statuses[1].error().code == glifistore::ErrorCode::resource_exhausted,
            "catch must not upgrade never-Store-entered sibling to unavailable");
    require(statuses[1].error().message.find("fail-closed") != std::string::npos,
            "mid-chunk sibling did not keep fail-closed reject through catch");
    require(!runtime->healthy(), "mutate+publish sticky did not fail-close the pair");

    const auto got_a = store.get(key_a);
    require(got_a.has_value(), "Store::get missed published first mid-chunk key after catch");
    require(std::string_view(reinterpret_cast<const char*>(got_a->bytes.data()), got_a->bytes.size()) ==
                "alpha",
            "published mid-chunk value mismatch after catch");
    const auto got_b = store.get(key_b);
    require(!got_b.has_value(), "never-Store-entered mid-chunk sibling became GET-visible");

    const auto late = store.put("mid-catch-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
#endif
}

void run_paired_sync_durable_group_catch_preserves_resource_exhausted() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Sync durable_group put_batch: same-key splits into sub-batches. First Index
    // publish + Site::index_account sticky-closes; second is never Store-entered
    // (resource_exhausted). Site::publish catch must not upgrade that sibling to
    // unavailable.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-sync-grp-catch-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(::mkdtemp(writable.data()) != nullptr, "mkdtemp failed");
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
        .storage_mode = glifistore::StorageMode::durable_group,
        .data_directory = store_path,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .durable_group = {.max_records = 1, .max_bytes = 65'536, .max_wait_ms = 60'000, .min_records = 1},
        .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
    });
    require(opened.has_value(), "failed to open paired durable_group Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    const std::string key = "grp-catch";
    const std::vector<glifistore::Store::PutItem> items{
        {.key = key, .value = bytes("first")},
        {.key = key, .value = bytes("second")},
    };

    glifistore::fault::fail_once(glifistore::fault::Site::index_account);
    glifistore::fault::fail_once(glifistore::fault::Site::publish);
    const auto statuses = store.put_batch(items);
    glifistore::fault::reset();
    require(statuses.size() == 2, "put_batch size mismatch");
    require(statuses[0].has_value(),
            "first durable-group put lost success ACK after index_account+publish catch");
    require(!statuses[1].has_value(), "later same-key sub-batch must not success-ACK after sticky");
    require(statuses[1].error().code == glifistore::ErrorCode::resource_exhausted,
            "durable-group catch must not upgrade never-Store-entered sibling to unavailable");
    require(statuses[1].error().message.find("fail-closed") != std::string::npos,
            "later sibling did not keep fail-closed reject through catch");
    require(!runtime->healthy(), "index_account+publish sticky did not fail-close the pair");

    const auto got = store.get(key);
    require(got.has_value(), "Store::get missed drain-snapshotted first value after catch");
    require(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) == "first",
            "drain-snapshotted value mismatch after catch");

    const auto late = store.put("grp-catch-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
#endif
}

void run_paired_sync_durable_sync_drain_after_capture_fail() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Sync durable_sync (single-op Writer path): commit then capture fail must
    // drain-snapshot before sticky close so Store::get keeps RAW (async already did).
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-sync-cap-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(::mkdtemp(writable.data()) != nullptr, "mkdtemp failed");
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = store_path,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
    });
    require(opened.has_value(), "failed to open paired durable_sync Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    glifistore::fault::fail_once(glifistore::fault::Site::capture);
    const std::string key = "sync-cap-a";
    const auto put = store.put(key, bytes("alpha"));
    glifistore::fault::reset();
    require(put.has_value(), "drain after capture fail did not success-ACK published commit");
    require(!runtime->healthy(), "capture failure did not sticky-fail the pair");

    const auto got = store.get(key);
    require(got.has_value(), "Store::get missed drain-snapshotted key after sync capture fail");
    require(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) == "alpha",
            "drain-snapshotted value mismatch");

    const auto late = store.put("sync-cap-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
#endif
}

void run_paired_sync_durable_sync_ack_after_publish_catch() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Sync durable_sync: after publish_read_generation, Site::publish fault throws
    // before reclaim. Catch must keep success ACK (authority already published).
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-sync-pub-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(::mkdtemp(writable.data()) != nullptr, "mkdtemp failed");
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = store_path,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
    });
    require(opened.has_value(), "failed to open paired durable_sync Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    glifistore::fault::fail_once(glifistore::fault::Site::publish);
    const std::string key = "sync-pub-a";
    const auto put = store.put(key, bytes("alpha"));
    glifistore::fault::reset();
    require(put.has_value(), "catch after publish inverted RAW with error ACK");
    require(!runtime->healthy(), "publish-path fault did not sticky-fail the pair");

    const auto got = store.get(key);
    require(got.has_value(), "Store::get missed published key after catch");
    require(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) == "alpha",
            "published value mismatch after catch");

    const auto late = store.put("sync-pub-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
#endif
}

void run_paired_sync_durable_sync_erase_ack_after_publish_catch() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Sync durable_sync erase: post-publish Site::publish fault must keep success ACK
    // and GET miss (tombstone already in the published generation).
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-sync-pube-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(::mkdtemp(writable.data()) != nullptr, "mkdtemp failed");
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = store_path,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
    });
    require(opened.has_value(), "failed to open paired durable_sync Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    const std::string key = "sync-pub-erase";
    require(store.put(key, bytes("seed")).has_value(), "seed put failed");
    glifistore::fault::fail_once(glifistore::fault::Site::publish);
    const auto erased = store.erase(key);
    glifistore::fault::reset();
    require(erased.has_value(), "erase catch after publish inverted RAW with error ACK");
    require(!runtime->healthy(), "erase publish-path fault did not sticky-fail the pair");

    const auto got = store.get(key);
    require(!got.has_value(), "Store::get still saw key after published erase catch");
    require(got.error().code == glifistore::ErrorCode::not_found, "post-erase get was not not_found");

    const auto late = store.put("sync-pube-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
#endif
}

void run_paired_sync_durable_sync_ack_after_index_account() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Index insert succeeds; secondary accounting fails (committed+error). Drain must
    // still success-ACK when the published generation shows the put (no inverted RAW).
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-sync-idx-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(::mkdtemp(writable.data()) != nullptr, "mkdtemp failed");
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = store_path,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
    });
    require(opened.has_value(), "failed to open paired durable_sync Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    glifistore::fault::fail_once(glifistore::fault::Site::index_account);
    const std::string key = "sync-idx-a";
    const auto put = store.put(key, bytes("alpha"));
    glifistore::fault::reset();
    require(put.has_value(), "Index-visible committed+error kept error ACK after drain");
    require(!runtime->healthy(), "Index accounting failure did not sticky-fail the pair");

    const auto got = store.get(key);
    require(got.has_value(), "Store::get missed Index-visible key after index_account fail");
    require(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) == "alpha",
            "Index-visible value mismatch after index_account fail");

    const auto late = store.put("sync-idx-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
#endif
}

void run_paired_sync_durable_sync_erase_ack_after_index_account() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Seed then erase: Index erase succeeds; accounting fails. Drain must success-ACK
    // when published generation shows absence (erase miss).
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-sync-idxe-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(::mkdtemp(writable.data()) != nullptr, "mkdtemp failed");
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = store_path,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
    });
    require(opened.has_value(), "failed to open paired durable_sync Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    const std::string key = "sync-idx-erase";
    require(store.put(key, bytes("seed")).has_value(), "seed put failed");
    glifistore::fault::fail_once(glifistore::fault::Site::index_account);
    const auto erased = store.erase(key);
    glifistore::fault::reset();
    require(erased.has_value(), "Index-visible erase committed+error kept error ACK after drain");
    require(!runtime->healthy(), "erase Index accounting failure did not sticky-fail the pair");

    const auto got = store.get(key);
    require(!got.has_value(), "Store::get still saw key after Index erase + accounting fail");
    require(got.error().code == glifistore::ErrorCode::not_found, "post-erase get was not not_found");

    const auto late = store.put("sync-idxe-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
#endif
}

void run_paired_async_durable_sync_ack_after_index_account() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Async durable_sync single-op: Index-visible committed+error must success-ACK
    // after drain-snapshot (mirrors sync ACK-after-visibility).
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-async-idx-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(::mkdtemp(writable.data()) != nullptr, "mkdtemp failed");
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = store_path,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
    });
    require(opened.has_value(), "failed to open paired durable_sync Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    glifistore::server::BoundedSpscQueue<glifistore::server::MutationCompletion> completions{4};
    auto wakeup = glifistore::server::Wakeup::create();
    require(wakeup.has_value(), "Wakeup::create failed");
    auto executor = glifistore::server::PairWriterPool::create(store, 1, 8, 1U * 1024U * 1024U,
                                                               std::chrono::milliseconds{0});
    require(executor.has_value(), "PairWriterPool::create failed");
    require((*executor)->start().has_value(), "PairWriterPool::start failed");

    const std::string key = "async-idx-a";
    glifistore::fault::fail_once(glifistore::fault::Site::index_account);
    require((*executor)
                ->try_submit({
                    .connection = {.slot = 1, .generation = 1},
                    .request_id = 1,
                    .worker_index = 0,
                    .kind = glifistore::server::MutationKind::put,
                    .key = bytes(key),
                    .key_hash = glifistore::hash_key(key),
                    .value = bytes("alpha"),
                    .completions = &completions,
                    .wakeup = &*wakeup,
                })
                .has_value(),
            "async put submit failed");

    std::optional<glifistore::server::MutationCompletion> done;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!done && std::chrono::steady_clock::now() < deadline) {
        if (auto completion = completions.try_pop()) {
            done = std::move(*completion);
            require((*executor)->release_payload(0, done->payload_slot), "release_payload failed");
        } else {
            static_cast<void>((*executor)->adopt_read_generation(0));
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    glifistore::fault::reset();
    require(done.has_value(), "async completion timed out");
    require(!runtime->healthy(), "async Index accounting failure did not sticky-fail the pair");
    require(!done->error.has_value(), "async Index-visible committed+error kept error ACK after drain");

    const auto got = store.get(key);
    require(got.has_value(), "Store::get missed async Index-visible key after index_account fail");
    require(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) == "alpha",
            "async Index-visible value mismatch");

    const auto late = store.put("async-idx-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
#endif
}

void run_paired_sync_durable_group_ack_after_index_account() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // durable_group flush: Index publish then accounting fail advances durable_through
    // before sticky close so finalize keeps success ACK + drain-snapshot (no RAW lie).
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-grp-idx-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(::mkdtemp(writable.data()) != nullptr, "mkdtemp failed");
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
        .storage_mode = glifistore::StorageMode::durable_group,
        .data_directory = store_path,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .durable_group = {.max_records = 1, .max_bytes = 65'536, .max_wait_ms = 60'000, .min_records = 1},
        .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
    });
    require(opened.has_value(), "failed to open paired durable_group Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    glifistore::fault::fail_once(glifistore::fault::Site::index_account);
    const std::string key = "grp-idx-a";
    const auto put = store.put(key, bytes("alpha"));
    glifistore::fault::reset();
    require(put.has_value(), "durable_group Index-visible flush kept error ACK after durable_through");
    require(!runtime->healthy(), "group Index accounting failure did not sticky-fail the pair");

    const auto got = store.get(key);
    require(got.has_value(), "Store::get missed group Index-visible key after index_account fail");
    require(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) == "alpha",
            "group Index-visible value mismatch");

    const auto late = store.put("grp-idx-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
#endif
}

void run_paired_volatile_sync_ack_after_publish_catch() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Volatile sync: after publish_read_generation, Site::publish fault throws before
    // reclaim. Catch must keep success ACK (authority already published) — mirrors
    // durable sync single-op; without this, GET-visible + error ACK inverts RAW.
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
    });
    require(opened.has_value(), "failed to open paired volatile Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    glifistore::fault::fail_once(glifistore::fault::Site::publish);
    const std::string key = "vol-pub-a";
    const auto put = store.put(key, bytes("alpha"));
    glifistore::fault::reset();
    require(put.has_value(), "volatile catch after publish inverted RAW with error ACK");
    require(!runtime->healthy(), "volatile publish-path fault did not sticky-fail the pair");

    const auto got = store.get(key);
    require(got.has_value(), "Store::get missed published volatile key after catch");
    require(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) == "alpha",
            "published volatile value mismatch after catch");

    const auto late = store.put("vol-pub-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
#endif
}

void run_paired_volatile_sync_erase_ack_after_publish_catch() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Volatile sync erase: post-publish Site::publish fault must keep success ACK + miss.
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
    });
    require(opened.has_value(), "failed to open paired volatile Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    const std::string key = "vol-pub-erase";
    require(store.put(key, bytes("seed")).has_value(), "seed put failed");
    glifistore::fault::fail_once(glifistore::fault::Site::publish);
    const auto erased = store.erase(key);
    glifistore::fault::reset();
    require(erased.has_value(), "volatile erase catch after publish inverted RAW with error ACK");
    require(!runtime->healthy(), "volatile erase publish-path fault did not sticky-fail the pair");

    const auto got = store.get(key);
    require(!got.has_value(), "Store::get still saw key after published volatile erase catch");
    require(got.error().code == glifistore::ErrorCode::not_found, "post-erase get was not not_found");

    const auto late = store.put("vol-pube-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
#endif
}

void run_paired_async_volatile_ack_after_publish_catch() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Async volatile: post-publish Site::publish fault must keep success completion
    // (staged indices ACK-after-publish) while sticky-failing the pair.
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
    });
    require(opened.has_value(), "failed to open paired volatile Store");
    auto& store = **opened;
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    require(runtime != nullptr, "missing paired runtime");

    glifistore::server::BoundedSpscQueue<glifistore::server::MutationCompletion> completions{4};
    auto wakeup = glifistore::server::Wakeup::create();
    require(wakeup.has_value(), "Wakeup::create failed");
    auto executor = glifistore::server::PairWriterPool::create(store, 1, 8, 1U * 1024U * 1024U,
                                                               std::chrono::milliseconds{0});
    require(executor.has_value(), "PairWriterPool::create failed");
    require((*executor)->start().has_value(), "PairWriterPool::start failed");

    const std::string key = "async-vol-pub-a";
    glifistore::fault::fail_once(glifistore::fault::Site::publish);
    require((*executor)
                ->try_submit({
                    .connection = {.slot = 1, .generation = 1},
                    .request_id = 1,
                    .worker_index = 0,
                    .kind = glifistore::server::MutationKind::put,
                    .key = bytes(key),
                    .key_hash = glifistore::hash_key(key),
                    .value = bytes("alpha"),
                    .completions = &completions,
                    .wakeup = &*wakeup,
                })
                .has_value(),
            "async volatile put submit failed");

    std::optional<glifistore::server::MutationCompletion> done;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!done && std::chrono::steady_clock::now() < deadline) {
        if (auto completion = completions.try_pop()) {
            done = std::move(*completion);
            require((*executor)->release_payload(0, done->payload_slot), "release_payload failed");
        } else {
            static_cast<void>((*executor)->adopt_read_generation(0));
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    glifistore::fault::reset();
    require(done.has_value(), "async volatile completion timed out");
    require(!runtime->healthy(), "async volatile publish-path fault did not sticky-fail the pair");
    require(!done->error.has_value(), "async volatile catch after publish inverted RAW with error ACK");

    const auto got = store.get(key);
    require(got.has_value(), "Store::get missed async published volatile key after catch");
    require(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) == "alpha",
            "async published volatile value mismatch");

    const auto late = store.put("async-vol-pub-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
#endif
}
} // namespace allocation_fault_test
