// durable_group Writer sub-batches (ADR 0047).
//
// A Writer sub-batch appends its distinct-key siblings into one pending group and commits them together.
// A sibling rejected before it touches that group (an erase of an absent key, an exhausted live-key
// budget) reports its own known-not-committed error and leaves the earlier, still-unflushed siblings
// intact and committable. Only a failure after the append attempt abandons them fail-closed; that rule is
// proven by "paired durable sync Writer does not success-ACK abandoned unflushed batch siblings".
//
// The first three tests place the rejected sibling behind a successful one in the same sub-batch, on the
// embedded batch API and on the daemon's asynchronous lane. The last two call
// StoreAccess::mutate_durable_batch directly and assert the rejected_before_append mark on both sides of
// the boundary.

#include "glifistore/core/key_hash.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/store/store.hpp"
#include "server_reactor_test_support.hpp"
#include "store/store_internal.hpp"
#include "test.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

using glifistore::test::server_reactor_support::bytes;
using glifistore::test::server_reactor_support::connect_to;
using glifistore::test::server_reactor_support::initialize_and_bind;
using glifistore::test::server_reactor_support::receive_response;
using glifistore::test::server_reactor_support::send_all;
using glifistore::test::server_reactor_support::ServerTemporaryDirectory;

auto text_of(const glifistore::OwnedValue& value) -> std::string_view {
    return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

auto durable_group_config(const std::filesystem::path& store_path) -> glifistore::StoreConfig {
    glifistore::StoreConfig config;
    config.worker_config.explicit_count = 1;
    config.storage_mode = glifistore::StorageMode::durable_group;
    config.durable_group = {.max_records = 32, .max_bytes = 65'536, .max_wait_ms = 10, .min_records = 1};
    config.maintenance.mode = glifistore::MaintenanceMode::disabled;
    config.data_directory = store_path;
    config.durable_open_mode = glifistore::DurableOpenMode::create_new;
    return config;
}

void require_runtime_healthy(glifistore::Store& store) {
    auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
    GLIFI_REQUIRE(runtime != nullptr);
    GLIFI_REQUIRE(runtime->healthy());
}

void require_value(glifistore::Store& store, const std::string_view key, const std::string_view expected) {
    const auto got = store.get(key);
    GLIFI_REQUIRE(got.has_value());
    GLIFI_REQUIRE(text_of(*got) == expected);
}

void require_absent(glifistore::Store& store, const std::string_view key) {
    const auto got = store.get(key);
    GLIFI_REQUIRE(!got.has_value());
    GLIFI_REQUIRE(got.error().code == glifistore::ErrorCode::not_found);
}

} // namespace

GLIFI_TEST("durable_group erase_batch keeps unflushed siblings when an absent key is rejected") {
    ServerTemporaryDirectory temporary;
    const auto config = durable_group_config(temporary.store_path());
    {
        auto opened = glifistore::Store::open(config);
        GLIFI_REQUIRE(opened.has_value());
        auto& store = **opened;
        GLIFI_REQUIRE(store.put("sibling-a", bytes("a")).has_value());
        GLIFI_REQUIRE(store.put("sibling-b", bytes("b")).has_value());
        GLIFI_REQUIRE(store.put("sibling-c", bytes("c")).has_value());

        // One sub-batch of distinct keys: the erase of "sibling-a" is appended but unflushed when the
        // erase of the absent key is rejected, and "sibling-b" follows the rejection.
        const std::vector<glifistore::Store::EraseItem> erases{
            {.key = "sibling-a"}, {.key = "sibling-absent"}, {.key = "sibling-b"}};
        const auto statuses = store.erase_batch(erases);
        GLIFI_REQUIRE(statuses.size() == 3);
        GLIFI_REQUIRE(statuses[0].has_value());
        GLIFI_REQUIRE(!statuses[1].has_value());
        GLIFI_REQUIRE(statuses[1].error().code == glifistore::ErrorCode::not_found);
        GLIFI_REQUIRE(statuses[2].has_value());

        require_runtime_healthy(store);
        require_absent(store, "sibling-a");
        require_absent(store, "sibling-b");
        require_value(store, "sibling-c", "c");
        GLIFI_REQUIRE(store.put("sibling-d", bytes("d")).has_value());
        GLIFI_REQUIRE(store.close().has_value());
    }
    auto reopened = glifistore::Store::open([&] {
        auto reopen = config;
        reopen.durable_open_mode = glifistore::DurableOpenMode::open_existing;
        return reopen;
    }());
    GLIFI_REQUIRE(reopened.has_value());
    auto& store = **reopened;
    require_absent(store, "sibling-a");
    require_absent(store, "sibling-b");
    require_value(store, "sibling-c", "c");
    require_value(store, "sibling-d", "d");
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("durable_group put_batch keeps unflushed siblings when the live-key budget rejects a new key") {
    ServerTemporaryDirectory temporary;
    auto config = durable_group_config(temporary.store_path());
    config.durable_limits.max_live_keys = 2;
    {
        auto opened = glifistore::Store::open(config);
        GLIFI_REQUIRE(opened.has_value());
        auto& store = **opened;
        GLIFI_REQUIRE(store.put("budget-a", bytes("a1")).has_value());
        GLIFI_REQUIRE(store.put("budget-b", bytes("b1")).has_value());

        // The budget is full: overwriting the two live keys is admitted, a third key is rejected before it
        // is appended, and the sibling after the rejection is still processed.
        const std::vector<glifistore::Store::PutItem> puts{
            {.key = "budget-a", .value = bytes("a2")},
            {.key = "budget-new", .value = bytes("new")},
            {.key = "budget-b", .value = bytes("b2")},
        };
        const auto statuses = store.put_batch(puts);
        GLIFI_REQUIRE(statuses.size() == 3);
        GLIFI_REQUIRE(statuses[0].has_value());
        GLIFI_REQUIRE(!statuses[1].has_value());
        GLIFI_REQUIRE(statuses[1].error().code == glifistore::ErrorCode::resource_exhausted);
        GLIFI_REQUIRE(statuses[2].has_value());

        require_runtime_healthy(store);
        require_value(store, "budget-a", "a2");
        require_value(store, "budget-b", "b2");
        require_absent(store, "budget-new");
        GLIFI_REQUIRE(store.close().has_value());
    }
    config.durable_open_mode = glifistore::DurableOpenMode::open_existing;
    auto reopened = glifistore::Store::open(config);
    GLIFI_REQUIRE(reopened.has_value());
    auto& store = **reopened;
    require_value(store, "budget-a", "a2");
    require_value(store, "budget-b", "b2");
    require_absent(store, "budget-new");
    GLIFI_REQUIRE(store.close().has_value());
}

GLIFI_TEST("daemon durable_group keeps a pipelined PUT when the next ERASE of an absent key is rejected") {
    ServerTemporaryDirectory temporary;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 1, .durable_mutation_queue_capacity = 4},
        {.storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_group = {.max_records = 2, .max_bytes = 65'536, .max_wait_ms = 1'000, .min_records = 2}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    // One connection keeps the order: the PUT is appended, unflushed, when the ERASE of an absent key is
    // rejected, and both belong to one Writer group because min_records is two.
    const auto put = glifistore::server::encode_request({.opcode = glifistore::server::RequestOpcode::put,
                                                         .request_id = 110,
                                                         .key = bytes("daemon-sibling"),
                                                         .value = bytes("kept")});
    const auto erase = glifistore::server::encode_request({.opcode = glifistore::server::RequestOpcode::erase,
                                                           .request_id = 111,
                                                           .key = bytes("daemon-absent")});
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(erase.has_value());
    std::vector<std::byte> pipeline;
    pipeline.reserve(put->size() + erase->size());
    pipeline.insert(pipeline.end(), put->begin(), put->end());
    pipeline.insert(pipeline.end(), erase->begin(), erase->end());
    GLIFI_REQUIRE(send_all(socket, pipeline));

    const auto put_frame = receive_response(socket);
    const auto erase_frame = receive_response(socket);
    const auto put_response = glifistore::server::decode_response(put_frame);
    const auto erase_response = glifistore::server::decode_response(erase_frame);
    GLIFI_REQUIRE(put_response.has_value());
    GLIFI_REQUIRE(erase_response.has_value());
    GLIFI_REQUIRE(put_response->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(erase_response->frame.status == glifistore::server::ResponseStatus::not_found);

    // The daemon is still serviceable: a full group commits and the kept value is readable.
    const auto follow_a =
        glifistore::server::encode_request({.opcode = glifistore::server::RequestOpcode::put,
                                            .request_id = 112,
                                            .key = bytes("daemon-follow-a"),
                                            .value = bytes("a")});
    const auto follow_b =
        glifistore::server::encode_request({.opcode = glifistore::server::RequestOpcode::put,
                                            .request_id = 113,
                                            .key = bytes("daemon-follow-b"),
                                            .value = bytes("b")});
    GLIFI_REQUIRE(follow_a.has_value());
    GLIFI_REQUIRE(follow_b.has_value());
    std::vector<std::byte> follow_pipeline;
    follow_pipeline.insert(follow_pipeline.end(), follow_a->begin(), follow_a->end());
    follow_pipeline.insert(follow_pipeline.end(), follow_b->begin(), follow_b->end());
    GLIFI_REQUIRE(send_all(socket, follow_pipeline));
    const auto follow_a_frame = receive_response(socket);
    const auto follow_b_frame = receive_response(socket);
    const auto follow_a_response = glifistore::server::decode_response(follow_a_frame);
    const auto follow_b_response = glifistore::server::decode_response(follow_b_frame);
    GLIFI_REQUIRE(follow_a_response.has_value());
    GLIFI_REQUIRE(follow_b_response.has_value());
    GLIFI_REQUIRE(follow_a_response->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(follow_b_response->frame.status == glifistore::server::ResponseStatus::ok);

    const auto get = glifistore::server::encode_request({.opcode = glifistore::server::RequestOpcode::get,
                                                         .request_id = 114,
                                                         .key = bytes("daemon-sibling")});
    GLIFI_REQUIRE(get.has_value());
    GLIFI_REQUIRE(send_all(socket, *get));
    const auto get_frame = receive_response(socket);
    const auto get_response = glifistore::server::decode_response(get_frame);
    GLIFI_REQUIRE(get_response.has_value());
    GLIFI_REQUIRE(get_response->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(get_response->frame.value.data()),
                                   get_response->frame.value.size()) == "kept");

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());

    auto recovered =
        glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                 .storage_mode = glifistore::StorageMode::durable_sync,
                                 .data_directory = temporary.store_path(),
                                 .durable_open_mode = glifistore::DurableOpenMode::open_existing});
    GLIFI_REQUIRE(recovered.has_value());
    require_value(**recovered, "daemon-sibling", "kept");
    require_value(**recovered, "daemon-follow-a", "a");
    require_value(**recovered, "daemon-follow-b", "b");
}

namespace {

// Direct calls to StoreAccess::mutate_durable_batch need the legacy runtime: its Index is visible to
// Store::get, while a paired Store reads a published generation that only the Writer produces.
// The record target and the deadline are out of reach, so a group closes only at commit_writer_batch and
// the first sibling is still unflushed when a later one is rejected or fails.
auto white_box_config(const std::filesystem::path& store_path) -> glifistore::StoreConfig {
    auto config = durable_group_config(store_path);
    config.concurrency = glifistore::StoreConcurrencyMode::legacy_mutex;
    config.durable_group = {.max_records = 32, .max_bytes = 65'536, .max_wait_ms = 60'000, .min_records = 32};
    return config;
}

using View = glifistore::detail::StoreAccess::DurableMutationView;
using Operation = glifistore::detail::StoreAccess::MutationOperation;

auto hashed_key(glifistore::Store& store, const std::string_view key) -> glifistore::HashedKey {
    const auto routing = glifistore::detail::StoreAccess::worker_routing(store);
    return {key, glifistore::hash_key_routing(key, routing)};
}

// Seeds through a one-item batch: the single-operation API would wait for the group deadline.
void commit_seed(glifistore::Store& store, const std::string_view key, const std::string_view value) {
    const View views[] = {{.operation = Operation::put,
                           .key = hashed_key(store, key),
                           .value = bytes(value),
                           .expire_at_ns = 0}};
    const auto results = glifistore::detail::StoreAccess::mutate_durable_batch(store, 0, views);
    GLIFI_REQUIRE(results.size() == 1);
    GLIFI_REQUIRE(results[0].mutation.committed());
    GLIFI_REQUIRE(!results[0].mutation.error.has_value());
}

struct ThrowOnSecondArmedWrite final {
    std::atomic_bool armed{false};
    std::atomic_uint64_t writes{0};

    static auto before(void* context, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto* self = static_cast<ThrowOnSecondArmedWrite*>(context);
        if (!self->armed.load(std::memory_order_acquire)) {
            return {};
        }
        if (operation == glifistore::FilesystemOperation::write_record &&
            self->writes.fetch_add(1, std::memory_order_relaxed) >= 1U) {
            throw std::bad_alloc{};
        }
        return {};
    }
};

} // namespace

GLIFI_TEST("mutate_durable_batch marks each rejection before append and keeps the unflushed prefix") {
    ServerTemporaryDirectory temporary;
    auto config = white_box_config(temporary.store_path());
    config.durable_limits.max_live_keys = 2;
    auto opened = glifistore::Store::open(config);
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    commit_seed(store, "wb-present", "p");

    const auto first = hashed_key(store, "wb-first");
    const auto absent = hashed_key(store, "wb-absent");
    const auto second = hashed_key(store, "wb-second");
    const auto present = hashed_key(store, "wb-present");
    // The first new key takes the last free slot of the live-key budget and stays unflushed; the absent
    // erase and the second new key are rejected before they touch the pending group; the last erase
    // commits behind both rejections.
    const View views[] = {
        {.operation = Operation::put, .key = first, .value = bytes("one"), .expire_at_ns = 0},
        {.operation = Operation::erase, .key = absent, .value = {}, .expire_at_ns = 0},
        {.operation = Operation::put, .key = second, .value = bytes("two"), .expire_at_ns = 0},
        {.operation = Operation::erase, .key = present, .value = {}, .expire_at_ns = 0},
    };
    const auto results = glifistore::detail::StoreAccess::mutate_durable_batch(store, 0, views);
    GLIFI_REQUIRE(results.size() == 4);
    GLIFI_REQUIRE(results[0].mutation.committed());
    GLIFI_REQUIRE(!results[0].mutation.error.has_value());
    GLIFI_REQUIRE(results[1].mutation.outcome == glifistore::DurableMutationOutcome::not_committed);
    GLIFI_REQUIRE(results[1].mutation.rejected_before_append);
    GLIFI_REQUIRE(results[1].mutation.error.has_value());
    GLIFI_REQUIRE(results[1].mutation.error->code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE(results[2].mutation.outcome == glifistore::DurableMutationOutcome::not_committed);
    GLIFI_REQUIRE(results[2].mutation.rejected_before_append);
    GLIFI_REQUIRE(results[2].mutation.error.has_value());
    GLIFI_REQUIRE(results[2].mutation.error->code == glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(results[3].mutation.committed());
    GLIFI_REQUIRE(!results[3].mutation.error.has_value());
    GLIFI_REQUIRE(glifistore::detail::StoreAccess::operational(store));

    require_value(store, "wb-first", "one");
    require_absent(store, "wb-present");
    require_absent(store, "wb-second");
    GLIFI_REQUIRE(store.close().has_value());

    config.durable_open_mode = glifistore::DurableOpenMode::open_existing;
    auto reopened = glifistore::Store::open(config);
    GLIFI_REQUIRE(reopened.has_value());
    require_value(**reopened, "wb-first", "one");
    require_absent(**reopened, "wb-present");
    require_absent(**reopened, "wb-second");
    GLIFI_REQUIRE((**reopened).close().has_value());
}

GLIFI_TEST("mutate_durable_batch abandons the unflushed prefix when a sibling fails at append") {
    ServerTemporaryDirectory temporary;
    ThrowOnSecondArmedWrite thrower;
    auto config = white_box_config(temporary.store_path());
    config.filesystem_hooks = {.context = &thrower, .before = &ThrowOnSecondArmedWrite::before};
    auto opened = glifistore::Store::open(config);
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    commit_seed(store, "append-seed", "ok");
    thrower.armed.store(true, std::memory_order_release);

    const auto first = hashed_key(store, "append-a");
    const auto second = hashed_key(store, "append-b");
    // The record write of the second sibling fails after the first one was appended: the failure is not a
    // rejection before append, so the pending group may disagree with the Segment pending commit and the
    // unflushed first sibling must not stay successful.
    const View views[] = {
        {.operation = Operation::put, .key = first, .value = bytes("alpha"), .expire_at_ns = 0},
        {.operation = Operation::put, .key = second, .value = bytes("beta"), .expire_at_ns = 0},
    };
    const auto results = glifistore::detail::StoreAccess::mutate_durable_batch(store, 0, views);
    GLIFI_REQUIRE(results.size() == 2);
    GLIFI_REQUIRE(thrower.writes.load(std::memory_order_relaxed) >= 2U);
    GLIFI_REQUIRE(results[1].mutation.outcome == glifistore::DurableMutationOutcome::not_committed);
    GLIFI_REQUIRE(!results[1].mutation.rejected_before_append);
    GLIFI_REQUIRE(results[0].mutation.outcome == glifistore::DurableMutationOutcome::indeterminate);
    GLIFI_REQUIRE(!glifistore::detail::StoreAccess::operational(store));
    static_cast<void>(store.close());
}
