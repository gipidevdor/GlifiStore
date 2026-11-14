#include "allocation_fault_test_support.hpp"
#include "allocation_fault_tests_decl.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/server/server.hpp"

namespace allocation_fault_test {
void run_paired_async_durable_sync_ack_after_publish_catch() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Async durable_sync: post-publish Site::publish fault must keep success completion.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-async-pub-XXXXXX").string();
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

    const std::string key = "async-dur-pub-a";
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
            "async durable put submit failed");

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
    require(done.has_value(), "async durable completion timed out");
    require(!runtime->healthy(), "async durable publish-path fault did not sticky-fail the pair");
    require(!done->error.has_value(), "async durable catch after publish inverted RAW with error ACK");

    const auto got = store.get(key);
    require(got.has_value(), "Store::get missed async published durable key after catch");
    require(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) == "alpha",
            "async published durable value mismatch");

    const auto late = store.put("async-dur-pub-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
#endif
}

void run_paired_async_durable_sync_erase_ack_after_publish_catch() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Async durable_sync erase: post-publish fault must success-ACK + GET miss.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-async-pube-XXXXXX").string();
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

    const std::string key = "async-dur-pub-erase";
    require(store.put(key, bytes("seed")).has_value(), "seed put failed");

    glifistore::server::BoundedSpscQueue<glifistore::server::MutationCompletion> completions{4};
    auto wakeup = glifistore::server::Wakeup::create();
    require(wakeup.has_value(), "Wakeup::create failed");
    auto executor = glifistore::server::PairWriterPool::create(store, 1, 8, 1U * 1024U * 1024U,
                                                               std::chrono::milliseconds{0});
    require(executor.has_value(), "PairWriterPool::create failed");
    require((*executor)->start().has_value(), "PairWriterPool::start failed");

    glifistore::fault::fail_once(glifistore::fault::Site::publish);
    require((*executor)
                ->try_submit({
                    .connection = {.slot = 1, .generation = 1},
                    .request_id = 1,
                    .worker_index = 0,
                    .kind = glifistore::server::MutationKind::erase,
                    .key = bytes(key),
                    .key_hash = glifistore::hash_key(key),
                    .value = {},
                    .completions = &completions,
                    .wakeup = &*wakeup,
                })
                .has_value(),
            "async durable erase submit failed");

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
    require(done.has_value(), "async durable erase completion timed out");
    require(!runtime->healthy(), "async durable erase publish-path fault did not sticky-fail");
    require(!done->error.has_value(), "async durable erase catch after publish inverted RAW with error ACK");

    const auto got = store.get(key);
    require(!got.has_value(), "Store::get still saw key after async published erase catch");
    require(got.error().code == glifistore::ErrorCode::not_found, "post-erase get was not not_found");

    const auto late = store.put("async-dur-pube-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
#endif
}

void run_paired_async_volatile_erase_ack_after_publish_catch() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Async volatile erase: post-publish fault must success-ACK + GET miss.
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

    const std::string key = "async-vol-pub-erase";
    require(store.put(key, bytes("seed")).has_value(), "seed put failed");

    glifistore::server::BoundedSpscQueue<glifistore::server::MutationCompletion> completions{4};
    auto wakeup = glifistore::server::Wakeup::create();
    require(wakeup.has_value(), "Wakeup::create failed");
    auto executor = glifistore::server::PairWriterPool::create(store, 1, 8, 1U * 1024U * 1024U,
                                                               std::chrono::milliseconds{0});
    require(executor.has_value(), "PairWriterPool::create failed");
    require((*executor)->start().has_value(), "PairWriterPool::start failed");

    glifistore::fault::fail_once(glifistore::fault::Site::publish);
    require((*executor)
                ->try_submit({
                    .connection = {.slot = 1, .generation = 1},
                    .request_id = 1,
                    .worker_index = 0,
                    .kind = glifistore::server::MutationKind::erase,
                    .key = bytes(key),
                    .key_hash = glifistore::hash_key(key),
                    .value = {},
                    .completions = &completions,
                    .wakeup = &*wakeup,
                })
                .has_value(),
            "async volatile erase submit failed");

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
    require(done.has_value(), "async volatile erase completion timed out");
    require(!runtime->healthy(), "async volatile erase publish-path fault did not sticky-fail");
    require(!done->error.has_value(), "async volatile erase catch after publish inverted RAW with error ACK");

    const auto got = store.get(key);
    require(!got.has_value(), "Store::get still saw key after async volatile erase catch");
    require(got.error().code == glifistore::ErrorCode::not_found, "post-erase get was not not_found");

    const auto late = store.put("async-vol-pube-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
#endif
}

void run_paired_async_durable_group_ack_after_index_account() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Async durable_group: Index publish then accounting fail must success-ACK after
    // mutate_durable_batch finalize + drain (distinct from durable_sync single-op).
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-async-grp-idx-XXXXXX").string();
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

    glifistore::server::BoundedSpscQueue<glifistore::server::MutationCompletion> completions{4};
    auto wakeup = glifistore::server::Wakeup::create();
    require(wakeup.has_value(), "Wakeup::create failed");
    auto executor = glifistore::server::PairWriterPool::create(store, 1, 8, 1U * 1024U * 1024U,
                                                               std::chrono::milliseconds{0});
    require(executor.has_value(), "PairWriterPool::create failed");
    require((*executor)->start().has_value(), "PairWriterPool::start failed");

    const std::string key = "async-grp-idx-a";
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
            "async group put submit failed");

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
    require(done.has_value(), "async group completion timed out");
    require(!runtime->healthy(), "async group Index accounting failure did not sticky-fail the pair");
    require(!done->error.has_value(), "async group Index-visible commit kept error ACK after drain");

    const auto got = store.get(key);
    require(got.has_value(), "Store::get missed async group Index-visible key");
    require(std::string_view(reinterpret_cast<const char*>(got->bytes.data()), got->bytes.size()) == "alpha",
            "async group Index-visible value mismatch");

    const auto late = store.put("async-grp-idx-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
#endif
}

void run_paired_async_durable_group_erase_ack_after_index_account() {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Async durable_group erase: Index erase then accounting fail → success ACK + miss.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-async-grp-idxe-XXXXXX").string();
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

    const std::string key = "async-grp-idx-erase";
    require(store.put(key, bytes("seed")).has_value(), "seed put failed");

    glifistore::server::BoundedSpscQueue<glifistore::server::MutationCompletion> completions{4};
    auto wakeup = glifistore::server::Wakeup::create();
    require(wakeup.has_value(), "Wakeup::create failed");
    auto executor = glifistore::server::PairWriterPool::create(store, 1, 8, 1U * 1024U * 1024U,
                                                               std::chrono::milliseconds{0});
    require(executor.has_value(), "PairWriterPool::create failed");
    require((*executor)->start().has_value(), "PairWriterPool::start failed");

    glifistore::fault::fail_once(glifistore::fault::Site::index_account);
    require((*executor)
                ->try_submit({
                    .connection = {.slot = 1, .generation = 1},
                    .request_id = 1,
                    .worker_index = 0,
                    .kind = glifistore::server::MutationKind::erase,
                    .key = bytes(key),
                    .key_hash = glifistore::hash_key(key),
                    .value = {},
                    .completions = &completions,
                    .wakeup = &*wakeup,
                })
                .has_value(),
            "async group erase submit failed");

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
    require(done.has_value(), "async group erase completion timed out");
    require(!runtime->healthy(), "async group erase Index accounting did not sticky-fail the pair");
    require(!done->error.has_value(), "async group Index-visible erase kept error ACK after drain");

    const auto got = store.get(key);
    require(!got.has_value(), "Store::get still saw key after async group Index erase");
    require(got.error().code == glifistore::ErrorCode::not_found, "post-erase get was not not_found");

    const auto late = store.put("async-grp-idxe-late", bytes("no"));
    require(!late.has_value(), "late put accepted after sticky fail-closed");
    require(late.error().code == glifistore::ErrorCode::unavailable,
            "late put was not unavailable after sticky fail-closed");
    static_cast<void>(store.close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
#endif
}
} // namespace allocation_fault_test
