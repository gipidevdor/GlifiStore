// ADR 0045 §3: pipelined cold reads on one connection keep wire order, cancel on close and stay
// inside the output watermark.

#include "glifistore/core/types.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/store/store.hpp"
#include "server_reactor_test_support.hpp"
#include "test.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace glifistore::test::server_reactor_support;

namespace {

using glifistore::server::RequestOpcode;
using glifistore::server::ResponseStatus;

void seed_store(const std::filesystem::path& path, const std::size_t keys, const std::size_t value_bytes) {
    auto seed = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                         .storage_mode = glifistore::StorageMode::durable_sync,
                                         .data_directory = path,
                                         .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(seed.has_value());
    for (std::size_t index = 0; index < keys; ++index) {
        std::string value(value_bytes, static_cast<char>('a' + index % 26U));
        GLIFI_REQUIRE((*seed)->put("cold-" + std::to_string(index), bytes(value)).has_value());
    }
    GLIFI_REQUIRE((*seed)->close().has_value());
}

auto expected_value(const std::size_t index, const std::size_t value_bytes) -> std::string {
    return std::string(value_bytes, static_cast<char>('a' + index % 26U));
}

void append_request(std::vector<std::byte>& pipeline, const RequestOpcode opcode,
                    const std::uint64_t request_id, const std::string& key) {
    const auto encoded = glifistore::server::encode_request({
        .opcode = opcode,
        .request_id = request_id,
        .key = bytes(key),
        .value = opcode == RequestOpcode::ping ? bytes("pong") : std::span<const std::byte>{},
    });
    GLIFI_REQUIRE(encoded.has_value());
    pipeline.insert(pipeline.end(), encoded->begin(), encoded->end());
}

auto stat_value(glifistore::server::Server& server, const std::string_view name) -> std::uint64_t {
    const auto report = server.stats_report();
    GLIFI_REQUIRE(report.has_value());
    const auto key = std::string{name} + "=";
    const auto start = report->find(key);
    GLIFI_REQUIRE(start != std::string::npos);
    const auto end = report->find('\n', start);
    return std::stoull(report->substr(start + key.size(), end - start - key.size()));
}

auto wait_for_stat(glifistore::server::Server& server, const std::string_view name,
                   const std::uint64_t minimum) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        if (stat_value(server, name) >= minimum) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}

auto no_response_buffered(const int socket) -> bool {
    std::byte probe{};
    const auto received = ::recv(socket, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    return received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

struct ExpectedResponse final {
    std::uint64_t request_id{};
    ResponseStatus status{};
    std::string value;
};

void require_responses(const int socket, const std::vector<ExpectedResponse>& expected) {
    for (const auto& next : expected) {
        const auto frame = receive_response(socket);
        GLIFI_REQUIRE(!frame.empty());
        const auto decoded = glifistore::server::decode_response(frame, frame.size());
        GLIFI_REQUIRE(decoded.has_value());
        GLIFI_REQUIRE(decoded->frame.request_id == next.request_id);
        GLIFI_REQUIRE(decoded->frame.status == next.status);
        GLIFI_REQUIRE(text(decoded->frame.value) == next.value);
    }
}

} // namespace

GLIFI_TEST("pipelined cold GETs keep request order around decided responses and barriers") {
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    seed_store(path, 4, 16);

    BlockingColdRead blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .disk_read_thread_count = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = path,
         .durable_open_mode = glifistore::DurableOpenMode::open_existing,
         .durable_limits = {.resident_reads_enabled = false},
         .filesystem_hooks = {
             .file_io = {.context = &blocker, .read_some_at = &BlockingColdRead::read_some_at}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
    blocker.arm();

    // Cold reads interleaved with responses decided at once (missing keys), then a PING that
    // must wait for the window to drain, then a cold read that opens a new window.
    std::vector<std::byte> pipeline;
    append_request(pipeline, RequestOpcode::get, 10, "cold-0");
    append_request(pipeline, RequestOpcode::get, 11, "missing-a");
    append_request(pipeline, RequestOpcode::get, 12, "cold-1");
    append_request(pipeline, RequestOpcode::get, 13, "missing-b");
    append_request(pipeline, RequestOpcode::get, 14, "cold-2");
    append_request(pipeline, RequestOpcode::ping, 15, "");
    append_request(pipeline, RequestOpcode::get, 16, "cold-3");
    GLIFI_REQUIRE(send_all(socket, pipeline));
    GLIFI_REQUIRE(blocker.wait_until_blocked());

    // Both later cold reads are in flight behind the blocked one, and nothing has overtaken it.
    GLIFI_REQUIRE(wait_for_stat(server, "cold_reads_pipelined", 2));
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    GLIFI_REQUIRE(no_response_buffered(socket));

    blocker.release();
    require_responses(socket,
                      {
                          {.request_id = 10, .status = ResponseStatus::ok, .value = expected_value(0, 16)},
                          {.request_id = 11, .status = ResponseStatus::not_found, .value = {}},
                          {.request_id = 12, .status = ResponseStatus::ok, .value = expected_value(1, 16)},
                          {.request_id = 13, .status = ResponseStatus::not_found, .value = {}},
                          {.request_id = 14, .status = ResponseStatus::ok, .value = expected_value(2, 16)},
                          {.request_id = 15, .status = ResponseStatus::ok, .value = "pong"},
                          {.request_id = 16, .status = ResponseStatus::ok, .value = expected_value(3, 16)},
                      });
    GLIFI_REQUIRE(stat_value(server, "cold_reads_pipelined") == 2);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("closing a connection cancels its pipelined cold GETs and keeps the Reactor serving") {
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    seed_store(path, 4, 16);

    BlockingColdRead blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .disk_read_thread_count = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = path,
         .durable_open_mode = glifistore::DurableOpenMode::open_existing,
         .durable_limits = {.resident_reads_enabled = false},
         .filesystem_hooks = {
             .file_io = {.context = &blocker, .read_some_at = &BlockingColdRead::read_some_at}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto abandoned = connect_to(server.port());
    GLIFI_REQUIRE(abandoned >= 0);
    GLIFI_REQUIRE(initialize_and_bind(abandoned, 0, 1));
    blocker.arm();
    std::vector<std::byte> pipeline;
    for (std::uint64_t index = 0; index < 4; ++index) {
        append_request(pipeline, RequestOpcode::get, 20 + index, "cold-" + std::to_string(index));
    }
    GLIFI_REQUIRE(send_all(abandoned, pipeline));
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    GLIFI_REQUIRE(wait_for_stat(server, "cold_reads_pipelined", 3));

    // A reset makes the Reactor close the connection with four reads in flight (an orderly EOF
    // would let them finish). Their completions must then find a stale token.
    const linger reset{.l_onoff = 1, .l_linger = 0};
    GLIFI_REQUIRE(::setsockopt(abandoned, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    static_cast<void>(::close(abandoned));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (stat_value(server, "connections_active") != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    GLIFI_REQUIRE(stat_value(server, "connections_active") == 0);
    blocker.release();
    GLIFI_REQUIRE(blocker.wait_until_finished());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
    std::vector<std::byte> follow_up;
    append_request(follow_up, RequestOpcode::get, 30, "cold-1");
    append_request(follow_up, RequestOpcode::get, 31, "cold-2");
    GLIFI_REQUIRE(send_all(socket, follow_up));
    require_responses(socket,
                      {
                          {.request_id = 30, .status = ResponseStatus::ok, .value = expected_value(1, 16)},
                          {.request_id = 31, .status = ResponseStatus::ok, .value = expected_value(2, 16)},
                      });

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("pipelined cold GETs wait for output watermark room instead of failing") {
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    constexpr std::size_t kKeys = 6;
    constexpr std::size_t kValueBytes = 300;
    seed_store(path, kKeys, kValueBytes);

    BlockingColdRead blocker;
    // Each cold slot reserves its header plus its Record size (> 300 bytes), so a 1 KiB
    // watermark admits two reads at a time and defers the rest.
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .maximum_output_bytes = 1024, .disk_read_thread_count = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = path,
         .durable_open_mode = glifistore::DurableOpenMode::open_existing,
         .durable_limits = {.resident_reads_enabled = false},
         .filesystem_hooks = {
             .file_io = {.context = &blocker, .read_some_at = &BlockingColdRead::read_some_at}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
    blocker.arm();
    std::vector<std::byte> pipeline;
    std::vector<ExpectedResponse> expected;
    for (std::size_t index = 0; index < kKeys; ++index) {
        append_request(pipeline, RequestOpcode::get, 40 + index, "cold-" + std::to_string(index));
        expected.push_back({.request_id = 40 + index,
                            .status = ResponseStatus::ok,
                            .value = expected_value(index, kValueBytes)});
    }
    GLIFI_REQUIRE(send_all(socket, pipeline));
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    GLIFI_REQUIRE(wait_for_stat(server, "cold_read_window_deferrals", 1));
    GLIFI_REQUIRE(stat_value(server, "cold_reads_pipelined") == 1);

    blocker.release();
    require_responses(socket, expected);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("pipelined cold GETs wait for a full disk-read lane instead of OVERLOADED") {
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    seed_store(path, 4, 16);

    BlockingColdRead blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .disk_read_thread_count = 1, .disk_read_queue_capacity = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = path,
         .durable_open_mode = glifistore::DurableOpenMode::open_existing,
         .durable_limits = {.resident_reads_enabled = false},
         .filesystem_hooks = {
             .file_io = {.context = &blocker, .read_some_at = &BlockingColdRead::read_some_at}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
    blocker.arm();
    std::vector<std::byte> pipeline;
    std::vector<ExpectedResponse> expected;
    for (std::uint64_t index = 0; index < 4; ++index) {
        append_request(pipeline, RequestOpcode::get, 50 + index, "cold-" + std::to_string(index));
        expected.push_back(
            {.request_id = 50 + index, .status = ResponseStatus::ok, .value = expected_value(index, 16)});
    }
    GLIFI_REQUIRE(send_all(socket, pipeline));
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    // One connection never sees OVERLOADED for its own queue depth: the lane holds one read and
    // the rest of the pipeline waits.
    GLIFI_REQUIRE(wait_for_stat(server, "cold_read_window_deferrals", 1));
    GLIFI_REQUIRE(stat_value(server, "cold_reads_pipelined") == 0);

    blocker.release();
    require_responses(socket, expected);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("resident GET responses wait behind earlier cold GETs on the same connection") {
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    // Fill more than one Segment so the oldest keys live in a sealed Segment, then write one key
    // into the active Segment. A reserve for a single image keeps only the active Segment resident.
    constexpr std::size_t kLargeValueBytes = std::size_t{512} * 1024U;
    constexpr std::size_t kLargeKeys = glifistore::kSegmentSizeBytes / kLargeValueBytes + 8U;
    {
        auto seed = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                             .storage_mode = glifistore::StorageMode::durable_periodic,
                                             .data_directory = path,
                                             .durable_open_mode = glifistore::DurableOpenMode::create_new});
        GLIFI_REQUIRE(seed.has_value());
        for (std::size_t index = 0; index < kLargeKeys; ++index) {
            const auto value = expected_value(index, kLargeValueBytes);
            GLIFI_REQUIRE((*seed)->put("cold-" + std::to_string(index), bytes(value)).has_value());
        }
        GLIFI_REQUIRE((*seed)->put("hot", bytes("hot-value")).has_value());
        GLIFI_REQUIRE((*seed)->close().has_value());
    }

    BlockingColdRead blocker;
    glifistore::DurableResourceLimits limits{};
    limits.memory_reserve_bytes = glifistore::kSegmentSizeBytes + (std::uint64_t{8} << 20U);
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .disk_read_thread_count = 1},
        {.storage_mode = glifistore::StorageMode::durable_periodic,
         .data_directory = path,
         .durable_open_mode = glifistore::DurableOpenMode::open_existing,
         .durable_limits = limits,
         .filesystem_hooks = {
             .file_io = {.context = &blocker, .read_some_at = &BlockingColdRead::read_some_at}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
    // The resident key answers at once while nothing is in flight.
    std::vector<std::byte> warm;
    append_request(warm, RequestOpcode::get, 60, "hot");
    GLIFI_REQUIRE(send_all(socket, warm));
    require_responses(socket, {{.request_id = 60, .status = ResponseStatus::ok, .value = "hot-value"}});

    blocker.arm();
    std::vector<std::byte> pipeline;
    append_request(pipeline, RequestOpcode::get, 61, "cold-0");
    append_request(pipeline, RequestOpcode::get, 62, "hot");
    append_request(pipeline, RequestOpcode::get, 63, "cold-1");
    append_request(pipeline, RequestOpcode::get, 64, "hot");
    GLIFI_REQUIRE(send_all(socket, pipeline));
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    GLIFI_REQUIRE(wait_for_stat(server, "cold_reads_pipelined", 1));
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    GLIFI_REQUIRE(no_response_buffered(socket));

    blocker.release();
    require_responses(
        socket,
        {
            {.request_id = 61, .status = ResponseStatus::ok, .value = expected_value(0, kLargeValueBytes)},
            {.request_id = 62, .status = ResponseStatus::ok, .value = "hot-value"},
            {.request_id = 63, .status = ResponseStatus::ok, .value = expected_value(1, kLargeValueBytes)},
            {.request_id = 64, .status = ResponseStatus::ok, .value = "hot-value"},
        });

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}
