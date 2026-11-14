#include "glifistore/server/mutation_window.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/server.hpp"
#include "server_reactor_test_support.hpp"
#include "test.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace glifistore::test::server_reactor_support;
using glifistore::server::kMaximumMutationWindow;

namespace {

[[nodiscard]] auto socket_has_readable(const int socket, const int timeout_ms) -> bool {
    pollfd descriptor{.fd = socket, .events = POLLIN, .revents = 0};
    const int result = ::poll(&descriptor, 1, timeout_ms);
    return result > 0 && (descriptor.revents & POLLIN) != 0;
}

[[nodiscard]] auto expect_ok(const int socket, const std::uint64_t request_id,
                             const std::string_view expected_value = {}) -> bool {
    const auto frame = receive_response(socket);
    const auto response = glifistore::server::decode_response(frame);
    if (!response.has_value()) {
        return false;
    }
    if (response->frame.request_id != request_id ||
        response->frame.status != glifistore::server::ResponseStatus::ok) {
        return false;
    }
    if (!expected_value.empty() && text(response->frame.value) != expected_value) {
        return false;
    }
    return true;
}

} // namespace

GLIFI_TEST("volatile mutation window PUT then GET sees RAW values in FIFO order") {
    auto opened = glifistore::server::Server::create({.port = 0, .maximum_connections = 1});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    constexpr std::uint32_t kWindow = 8;
    std::vector<std::byte> pipeline;
    pipeline.reserve(kWindow * 96U);
    for (std::uint32_t index = 0; index < kWindow; ++index) {
        const auto key = "mw-put-" + std::to_string(index);
        const auto value = "mw-val-" + std::to_string(index);
        const auto put = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::put,
            .request_id = 1'000 + index,
            .key = bytes(key),
            .value = bytes(value),
        });
        GLIFI_REQUIRE(put.has_value());
        pipeline.insert(pipeline.end(), put->begin(), put->end());
    }
    const auto get_first = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 2'000,
        .key = bytes("mw-put-0"),
    });
    const auto get_last = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 2'001,
        .key = bytes("mw-put-7"),
    });
    GLIFI_REQUIRE(get_first.has_value());
    GLIFI_REQUIRE(get_last.has_value());
    pipeline.insert(pipeline.end(), get_first->begin(), get_first->end());
    pipeline.insert(pipeline.end(), get_last->begin(), get_last->end());
    GLIFI_REQUIRE(send_all(socket, pipeline));

    for (std::uint32_t index = 0; index < kWindow; ++index) {
        GLIFI_REQUIRE(expect_ok(socket, 1'000 + index));
    }
    GLIFI_REQUIRE(expect_ok(socket, 2'000, "mw-val-0"));
    GLIFI_REQUIRE(expect_ok(socket, 2'001, "mw-val-7"));

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("volatile mutation window ERASE then GET observes tombstone after barrier") {
    auto opened = glifistore::server::Server::create({.port = 0, .maximum_connections = 1});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 10,
        .key = bytes("mw-erase"),
        .value = bytes("present"),
    });
    const auto erase = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::erase,
        .request_id = 11,
        .key = bytes("mw-erase"),
    });
    const auto get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 12,
        .key = bytes("mw-erase"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(erase.has_value());
    GLIFI_REQUIRE(get.has_value());
    std::vector<std::byte> pipeline;
    pipeline.insert(pipeline.end(), put->begin(), put->end());
    pipeline.insert(pipeline.end(), erase->begin(), erase->end());
    pipeline.insert(pipeline.end(), get->begin(), get->end());
    GLIFI_REQUIRE(send_all(socket, pipeline));

    GLIFI_REQUIRE(expect_ok(socket, 10));
    GLIFI_REQUIRE(expect_ok(socket, 11));
    const auto get_frame = receive_response(socket);
    const auto get_response = glifistore::server::decode_response(get_frame);
    GLIFI_REQUIRE(get_response.has_value());
    GLIFI_REQUIRE(get_response->frame.request_id == 12);
    GLIFI_REQUIRE(get_response->frame.status == glifistore::server::ResponseStatus::not_found);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("volatile mutation window admits more than 32 contiguous PUTs across resumes") {
    GLIFI_REQUIRE(kMaximumMutationWindow == 32);
    auto opened = glifistore::server::Server::create({.port = 0, .maximum_connections = 1});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    constexpr std::uint32_t kCount = kMaximumMutationWindow + 1U;
    std::vector<std::byte> pipeline;
    pipeline.reserve(kCount * 96U);
    for (std::uint32_t index = 0; index < kCount; ++index) {
        const auto key = "mw-cap-" + std::to_string(index);
        const auto put = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::put,
            .request_id = 3'000 + index,
            .key = bytes(key),
            .value = bytes("x"),
        });
        GLIFI_REQUIRE(put.has_value());
        pipeline.insert(pipeline.end(), put->begin(), put->end());
    }
    GLIFI_REQUIRE(send_all(socket, pipeline));
    for (std::uint32_t index = 0; index < kCount; ++index) {
        GLIFI_REQUIRE(expect_ok(socket, 3'000 + index));
    }

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("durable_sync GET barrier waits for open mutation window before response") {
    ServerTemporaryDirectory temporary;
    BlockingFileSync blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.file_io = {.context = &blocker, .sync_file = &BlockingFileSync::sync_file}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    SyncReleaseGuard release_on_exit{blocker};
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 40,
        .key = bytes("mw-barrier"),
        .value = bytes("held"),
    });
    const auto get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 41,
        .key = bytes("mw-barrier"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(get.has_value());
    std::vector<std::byte> pipeline;
    pipeline.insert(pipeline.end(), put->begin(), put->end());
    pipeline.insert(pipeline.end(), get->begin(), get->end());

    blocker.arm();
    GLIFI_REQUIRE(send_all(socket, pipeline));
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    // While the PUT window is in flight, GET must not produce a response frame.
    GLIFI_REQUIRE(!socket_has_readable(socket, 100));
    blocker.release();

    GLIFI_REQUIRE(expect_ok(socket, 40));
    GLIFI_REQUIRE(expect_ok(socket, 41, "held"));

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("durable_sync PING drains open mutation window like GET barrier") {
    ServerTemporaryDirectory temporary;
    BlockingFileSync blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.file_io = {.context = &blocker, .sync_file = &BlockingFileSync::sync_file}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    SyncReleaseGuard release_on_exit{blocker};
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 50,
        .key = bytes("mw-ping"),
        .value = bytes("before-ping"),
    });
    const auto ping = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 51,
        .value = bytes("pong"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(ping.has_value());
    std::vector<std::byte> pipeline;
    pipeline.insert(pipeline.end(), put->begin(), put->end());
    pipeline.insert(pipeline.end(), ping->begin(), ping->end());

    blocker.arm();
    GLIFI_REQUIRE(send_all(socket, pipeline));
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    // Non-mutation opcodes are drain boundaries: no PING response while PUT is blocked.
    GLIFI_REQUIRE(!socket_has_readable(socket, 100));
    blocker.release();

    GLIFI_REQUIRE(expect_ok(socket, 50));
    GLIFI_REQUIRE(expect_ok(socket, 51, "pong"));

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}
