#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/persistence/store_backup.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/store/store.hpp"
#include "server_reactor_test_support.hpp"
#include "store/store_internal.hpp"
#include "test.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace glifistore::test::server_reactor_support;

namespace {

[[nodiscard]] auto stats_counter(const std::string_view report, const std::string_view name)
    -> std::optional<std::uint64_t> {
    const auto marker = report.find(name);
    if (marker == std::string_view::npos || marker + name.size() >= report.size() ||
        report[marker + name.size()] != '=') {
        return std::nullopt;
    }
    const auto newline = report.find('\n', marker);
    if (newline == std::string_view::npos) {
        return std::nullopt;
    }
    const auto first = report.data() + marker + name.size() + 1U;
    const auto last = report.data() + newline;
    std::uint64_t value{};
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return std::nullopt;
    }
    return value;
}

} // namespace

GLIFI_TEST("durable cold GET pipeline remains contiguous to preserve read overlap") {
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    constexpr std::size_t kValueBytes = 64U * 1024U;
    std::vector<std::byte> first_value(kValueBytes, std::byte{0x31});
    std::vector<std::byte> second_value(kValueBytes, std::byte{0x72});
    {
        auto seed = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                             .storage_mode = glifistore::StorageMode::durable_sync,
                                             .data_directory = path,
                                             .durable_open_mode = glifistore::DurableOpenMode::create_new});
        GLIFI_REQUIRE(seed.has_value());
        GLIFI_REQUIRE((*seed)->put("scatter-pipeline-a", first_value).has_value());
        GLIFI_REQUIRE((*seed)->put("scatter-pipeline-b", second_value).has_value());
        GLIFI_REQUIRE((*seed)->close().has_value());
    }

    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 1, .maximum_output_bytes = 256U * 1024U},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = path,
         .durable_open_mode = glifistore::DurableOpenMode::open_existing,
         // Cold-path test: resident images (ADR 0046) complete at prepare and take the value path.
         .durable_limits = {.resident_reads_enabled = false}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());
    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    const auto first_get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 62,
        .key = bytes("scatter-pipeline-a"),
    });
    const auto second_get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 63,
        .key = bytes("scatter-pipeline-b"),
    });
    GLIFI_REQUIRE(first_get.has_value());
    GLIFI_REQUIRE(second_get.has_value());
    std::vector<std::byte> pipeline;
    pipeline.reserve(first_get->size() + second_get->size());
    pipeline.insert(pipeline.end(), first_get->begin(), first_get->end());
    pipeline.insert(pipeline.end(), second_get->begin(), second_get->end());
    GLIFI_REQUIRE(send_all(socket, pipeline));

    const auto first_frame = receive_response(socket);
    const auto second_frame = receive_response(socket);
    const auto first = glifistore::server::decode_response(first_frame, 256U * 1024U);
    const auto second = glifistore::server::decode_response(second_frame, 256U * 1024U);
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE(second.has_value());
    GLIFI_REQUIRE(first->frame.request_id == 62);
    GLIFI_REQUIRE(second->frame.request_id == 63);
    GLIFI_REQUIRE(std::ranges::equal(first->frame.value, first_value));
    GLIFI_REQUIRE(std::ranges::equal(second->frame.value, second_value));

    const auto report = server.stats_report();
    GLIFI_REQUIRE(report.has_value());
    GLIFI_REQUIRE(report->find("output_scatter_responses=0\n") != std::string::npos);
    GLIFI_REQUIRE(report->find("output_scatter_completions=0\n") != std::string::npos);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("deep mutation pipeline keeps input compaction linear") {
    constexpr std::size_t kRequests = 128;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 1, .maximum_input_bytes = 64U * 1024U});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());
    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    std::vector<std::byte> pipeline;
    pipeline.reserve(16U * 1024U);
    for (std::size_t index = 0; index < kRequests; ++index) {
        const auto key = std::string{"sliding-pipeline-"} + std::to_string(index);
        const auto value = std::string{"value-"} + std::to_string(index);
        const auto request = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::put,
            .request_id = 1'000U + index,
            .key = bytes(key),
            .value = bytes(value),
        });
        GLIFI_REQUIRE(request.has_value());
        pipeline.insert(pipeline.end(), request->begin(), request->end());
    }
    GLIFI_REQUIRE(send_all(socket, pipeline));

    for (std::size_t index = 0; index < kRequests; ++index) {
        const auto frame = receive_response(socket);
        const auto response = glifistore::server::decode_response(frame);
        GLIFI_REQUIRE(response.has_value());
        GLIFI_REQUIRE(response->frame.status == glifistore::server::ResponseStatus::ok);
        GLIFI_REQUIRE(response->frame.request_id == 1'000U + index);
    }

    const auto report = server.stats_report();
    GLIFI_REQUIRE(report.has_value());
    const auto compactions = stats_counter(*report, "input_buffer_compactions");
    const auto bytes_moved = stats_counter(*report, "input_buffer_bytes_moved");
    const auto output_compactions = stats_counter(*report, "output_buffer_compactions");
    const auto output_bytes_moved = stats_counter(*report, "output_buffer_bytes_moved");
    GLIFI_REQUIRE(compactions.has_value());
    GLIFI_REQUIRE(bytes_moved.has_value());
    GLIFI_REQUIRE(output_compactions.has_value());
    GLIFI_REQUIRE(output_bytes_moved.has_value());
    const auto typed_stats = server.reactor_buffer_stats();
    GLIFI_REQUIRE(typed_stats.input_compactions == *compactions);
    GLIFI_REQUIRE(typed_stats.input_bytes_moved == *bytes_moved);
    GLIFI_REQUIRE(typed_stats.output_compactions == *output_compactions);
    GLIFI_REQUIRE(typed_stats.output_bytes_moved == *output_bytes_moved);
    // Capacity growth can trigger a bounded append-time compaction when TCP splits
    // the pipeline. It must never return to one suffix memmove per completion.
    GLIFI_REQUIRE(*compactions < kRequests / 4U);
    GLIFI_REQUIRE(*bytes_moved <= pipeline.size() * 2U);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("partial output appends compact only when retained capacity is exhausted") {
#if defined(__OpenBSD__) || defined(__FreeBSD__)
    // SO_RCVBUF and BSD readiness scheduling under qemu do not reliably retain
    // the user-space suffix required by this timing test. Linux and macOS
    // exercise the copy-avoidance path; BSD still runs the protocol suites.
    return;
#endif
    auto opened = glifistore::server::Server::create({
        .port = 0,
        .maximum_connections = 1,
        .maximum_output_bytes = 1024U * 1024U,
        .accepted_socket_send_buffer_bytes = 4U * 1024U,
    });
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    const int receive_buffer = 4 * 1024;
    GLIFI_REQUIRE(::setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)) == 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    // A completed large response leaves vector capacity retained after clear().
    const std::vector<std::byte> warm_value(768U * 1024U, std::byte{0x41});
    const auto warm = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 2'000,
        .value = warm_value,
    });
    GLIFI_REQUIRE(warm.has_value());
    GLIFI_REQUIRE(send_all(socket, *warm));
    auto response = glifistore::server::decode_response(receive_response(socket));
    GLIFI_REQUIRE(response.has_value());
    GLIFI_REQUIRE(response->frame.request_id == 2'000);
    GLIFI_REQUIRE(response->frame.value.size() == warm_value.size());

    // This response cannot fit in the deliberately small TCP windows. Follow-up
    // responses still fit in the retained vector capacity, so appending them must
    // preserve the consumed prefix cursor instead of moving the unsent suffix.
    const std::vector<std::byte> slow_value(256U * 1024U, std::byte{0x52});
    const auto slow = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 2'001,
        .value = slow_value,
    });
    GLIFI_REQUIRE(slow.has_value());
    GLIFI_REQUIRE(send_all(socket, *slow));
    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    constexpr std::size_t kFollowUps = 8;
    const std::vector<std::byte> follow_value(1024, std::byte{0x63});
    for (std::size_t index = 0; index < kFollowUps; ++index) {
        const auto follow = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::ping,
            .request_id = 2'002U + index,
            .value = follow_value,
        });
        GLIFI_REQUIRE(follow.has_value());
        GLIFI_REQUIRE(send_all(socket, *follow));
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    response = glifistore::server::decode_response(receive_response(socket));
    GLIFI_REQUIRE(response.has_value());
    GLIFI_REQUIRE(response->frame.request_id == 2'001);
    GLIFI_REQUIRE(response->frame.value.size() == slow_value.size());
    for (std::size_t index = 0; index < kFollowUps; ++index) {
        response = glifistore::server::decode_response(receive_response(socket));
        GLIFI_REQUIRE(response.has_value());
        GLIFI_REQUIRE(response->frame.request_id == 2'002U + index);
        GLIFI_REQUIRE(response->frame.value.size() == follow_value.size());
    }

    auto stats = server.reactor_buffer_stats();
    GLIFI_REQUIRE(stats.output_compactions == 0);
    GLIFI_REQUIRE(stats.output_bytes_moved == 0);

    // The retained capacity is approximately the warm response size. A 512 KiB
    // partial response plus a 256 KiB follow-up and its header cannot append
    // physically without growth, although their unsent logical bytes remain under
    // the 1 MiB watermark. Exactly one necessary compaction reclaims the prefix.
    const std::vector<std::byte> exhaust_value(512U * 1024U, std::byte{0x74});
    const auto exhaust = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 2'100,
        .value = exhaust_value,
    });
    GLIFI_REQUIRE(exhaust.has_value());
    GLIFI_REQUIRE(send_all(socket, *exhaust));
    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    const std::vector<std::byte> growth_value(256U * 1024U, std::byte{0x75});
    const auto growth = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 2'101,
        .value = growth_value,
    });
    GLIFI_REQUIRE(growth.has_value());
    GLIFI_REQUIRE(send_all(socket, *growth));

    response = glifistore::server::decode_response(receive_response(socket));
    GLIFI_REQUIRE(response.has_value());
    GLIFI_REQUIRE(response->frame.request_id == 2'100);
    GLIFI_REQUIRE(response->frame.value.size() == exhaust_value.size());
    response = glifistore::server::decode_response(receive_response(socket));
    GLIFI_REQUIRE(response.has_value());
    GLIFI_REQUIRE(response->frame.request_id == 2'101);
    GLIFI_REQUIRE(response->frame.value.size() == growth_value.size());

    stats = server.reactor_buffer_stats();
    GLIFI_REQUIRE(stats.output_compactions == 1);
    GLIFI_REQUIRE(stats.output_bytes_moved > 0);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("late cold-read completion cannot target a reused connection slot") {
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    {
        auto seed = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                             .storage_mode = glifistore::StorageMode::durable_sync,
                                             .data_directory = path,
                                             .durable_open_mode = glifistore::DurableOpenMode::create_new});
        GLIFI_REQUIRE(seed.has_value());
        GLIFI_REQUIRE((*seed)->put("stale-read", bytes("old-value")).has_value());
        GLIFI_REQUIRE((*seed)->close().has_value());
    }

    BlockingColdRead blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 1, .disk_read_thread_count = 1, .disk_read_queue_capacity = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = path,
         .durable_open_mode = glifistore::DurableOpenMode::open_existing,
         // Cold-path test: resident images (ADR 0046) would bypass the blocked file read.
         .durable_limits = {.resident_reads_enabled = false},
         .filesystem_hooks = {
             .file_io = {.context = &blocker, .read_some_at = &BlockingColdRead::read_some_at}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto old_socket = connect_to(server.port());
    GLIFI_REQUIRE(old_socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(old_socket, 0, 1));
    blocker.arm();
    const auto get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 50,
        .key = bytes("stale-read"),
    });
    GLIFI_REQUIRE(get.has_value());
    GLIFI_REQUIRE(send_all(old_socket, *get));
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    linger reset_on_close{.l_onoff = 1, .l_linger = 0};
    GLIFI_REQUIRE(::setsockopt(old_socket, SOL_SOCKET, SO_LINGER, &reset_on_close, sizeof(reset_on_close)) ==
                  0);
    static_cast<void>(::close(old_socket));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (server.active_connections_per_executor()[0] != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const auto old_connection_closed = server.active_connections_per_executor()[0] == 0;
    if (!old_connection_closed) {
        blocker.release();
    }
    GLIFI_REQUIRE(old_connection_closed);

    // maximum_connections=1 forces the next connection to reuse the same slot
    // with a new generation while the old pinned read is still in flight.
    const auto reused_socket = connect_to(server.port());
    if (reused_socket < 0) {
        blocker.release();
    }
    GLIFI_REQUIRE(reused_socket >= 0);
    const auto reused_initialized = initialize_and_bind(reused_socket, 0, 1);
    if (!reused_initialized) {
        blocker.release();
    }
    GLIFI_REQUIRE(reused_initialized);
    blocker.release();
    GLIFI_REQUIRE(blocker.wait_until_finished());

    const auto ping = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 51,
        .value = bytes("new-generation"),
    });
    GLIFI_REQUIRE(ping.has_value());
    GLIFI_REQUIRE(send_all(reused_socket, *ping));
    const auto frame = receive_response(reused_socket);
    const auto response = glifistore::server::decode_response(frame);
    GLIFI_REQUIRE(response.has_value());
    GLIFI_REQUIRE(response->frame.request_id == 51);
    GLIFI_REQUIRE(text(response->frame.value) == "new-generation");

    static_cast<void>(::close(reused_socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("server shutdown drains an in-flight pinned cold read before Store close") {
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    {
        auto seed = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                             .storage_mode = glifistore::StorageMode::durable_sync,
                                             .data_directory = path,
                                             .durable_open_mode = glifistore::DurableOpenMode::create_new});
        GLIFI_REQUIRE(seed.has_value());
        GLIFI_REQUIRE((*seed)->put("shutdown-read", bytes("value")).has_value());
        GLIFI_REQUIRE((*seed)->close().has_value());
    }

    BlockingColdRead blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 1, .disk_read_thread_count = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = path,
         .durable_open_mode = glifistore::DurableOpenMode::open_existing,
         // Cold-path test: resident images (ADR 0046) would bypass the blocked file read.
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
    const auto get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 60,
        .key = bytes("shutdown-read"),
    });
    GLIFI_REQUIRE(get.has_value());
    GLIFI_REQUIRE(send_all(socket, *get));
    GLIFI_REQUIRE(blocker.wait_until_blocked());

    std::atomic_bool join_finished{};
    bool join_succeeded{};
    server.request_stop();
    std::thread joiner{[&] {
        join_succeeded = server.join().has_value();
        join_finished.store(true, std::memory_order_release);
    }};
    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{50};
    while (!join_finished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < drain_deadline) {
        std::this_thread::yield();
    }
    const auto waited_for_read = !join_finished.load(std::memory_order_acquire);
    blocker.release();
    joiner.join();
    GLIFI_REQUIRE(waited_for_read);
    GLIFI_REQUIRE(join_succeeded);
    static_cast<void>(::close(socket));
}

GLIFI_TEST("TCP reactor handles partial and pipelined protocol frames") {
    auto opened = glifistore::server::Server::create({.port = 0, .maximum_connections = 16});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    const auto ping = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 10,
        .value = bytes("first"),
    });
    GLIFI_REQUIRE(ping.has_value());
    GLIFI_REQUIRE(send_all(socket, std::span<const std::byte>{ping->data(), 3}));
    GLIFI_REQUIRE(send_all(socket, std::span<const std::byte>{ping->data() + 3, ping->size() - 3}));

    const auto first_frame = receive_response(socket);
    GLIFI_REQUIRE(!first_frame.empty());
    const auto first = glifistore::server::decode_response(first_frame);
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE(first->frame.request_id == 10);
    GLIFI_REQUIRE(text(first->frame.value) == "first");

    const auto init = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::init,
        .request_id = 11,
    });
    const auto second_ping = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 12,
        .value = bytes("second"),
    });
    GLIFI_REQUIRE(init.has_value());
    GLIFI_REQUIRE(second_ping.has_value());
    std::vector<std::byte> pipelined;
    pipelined.insert(pipelined.end(), init->begin(), init->end());
    pipelined.insert(pipelined.end(), second_ping->begin(), second_ping->end());
    GLIFI_REQUIRE(send_all(socket, pipelined));

    const auto init_frame = receive_response(socket);
    const auto second_ping_frame = receive_response(socket);
    const auto init_response = glifistore::server::decode_response(init_frame);
    const auto ping_response = glifistore::server::decode_response(second_ping_frame);
    GLIFI_REQUIRE(init_response.has_value());
    GLIFI_REQUIRE(ping_response.has_value());
    GLIFI_REQUIRE(init_response->frame.request_id == 11);
    GLIFI_REQUIRE(text(init_response->frame.value) == "GlifiStore/2");
    GLIFI_REQUIRE(init_response->frame.worker_count == 1);
    GLIFI_REQUIRE(ping_response->frame.request_id == 12);
    GLIFI_REQUIRE(text(ping_response->frame.value) == "second");
    const auto bind = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::bind_worker,
        .request_id = 14,
        .target_worker = 0,
    });
    GLIFI_REQUIRE(bind.has_value());
    GLIFI_REQUIRE(send_all(socket, *bind));
    const auto bind_frame = receive_response(socket);
    const auto bind_response = glifistore::server::decode_response(bind_frame);
    GLIFI_REQUIRE(bind_response.has_value());
    GLIFI_REQUIRE(bind_response->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(bind_response->frame.owner_worker == 0);

    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 20,
        .key = bytes("network-key"),
        .value = bytes("network-value"),
    });
    const auto get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 21,
        .key = bytes("network-key"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(get.has_value());
    std::vector<std::byte> store_pipeline;
    store_pipeline.insert(store_pipeline.end(), put->begin(), put->end());
    store_pipeline.insert(store_pipeline.end(), get->begin(), get->end());
    GLIFI_REQUIRE(send_all(socket, store_pipeline));
    const auto put_frame = receive_response(socket);
    const auto get_frame = receive_response(socket);
    const auto put_response = glifistore::server::decode_response(put_frame);
    const auto get_response = glifistore::server::decode_response(get_frame);
    GLIFI_REQUIRE(put_response.has_value());
    GLIFI_REQUIRE(get_response.has_value());
    GLIFI_REQUIRE(put_response->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(get_response->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(text(get_response->frame.value) == "network-value");

    const auto erase = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::erase,
        .request_id = 22,
        .key = bytes("network-key"),
    });
    GLIFI_REQUIRE(erase.has_value());
    GLIFI_REQUIRE(send_all(socket, *erase));
    const auto erase_frame = receive_response(socket);
    const auto erase_response = glifistore::server::decode_response(erase_frame);
    GLIFI_REQUIRE(erase_response.has_value());
    GLIFI_REQUIRE(erase_response->frame.status == glifistore::server::ResponseStatus::ok);

    const auto missing_get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 23,
        .key = bytes("network-key"),
    });
    GLIFI_REQUIRE(missing_get.has_value());
    GLIFI_REQUIRE(send_all(socket, *missing_get));
    const auto missing_frame = receive_response(socket);
    const auto missing_response = glifistore::server::decode_response(missing_frame);
    GLIFI_REQUIRE(missing_response.has_value());
    GLIFI_REQUIRE(missing_response->frame.status == glifistore::server::ResponseStatus::not_found);

    std::vector<std::byte> large_payload(1536U * 1024U, std::byte{0x5A});
    const auto large_ping = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 13,
        .value = large_payload,
    });
    GLIFI_REQUIRE(large_ping.has_value());
    GLIFI_REQUIRE(send_all(socket, *large_ping));
    const auto large_response_frame = receive_response(socket);
    const auto large_response = glifistore::server::decode_response(large_response_frame);
    GLIFI_REQUIRE(large_response.has_value());
    GLIFI_REQUIRE(large_response->frame.request_id == 13);
    GLIFI_REQUIRE(large_response->frame.value.size() == large_payload.size());
    GLIFI_REQUIRE(std::ranges::equal(large_response->frame.value, large_payload));

    static_cast<void>(::close(socket));

    const auto half_closed_socket = connect_to(server.port());
    GLIFI_REQUIRE(half_closed_socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(half_closed_socket, 0, 1));
    const auto final_put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 30,
        .key = bytes("half-close-key"),
        .value = bytes("half-close-value"),
    });
    GLIFI_REQUIRE(final_put.has_value());
    GLIFI_REQUIRE(send_all(half_closed_socket, *final_put));
    GLIFI_REQUIRE(::shutdown(half_closed_socket, SHUT_WR) == 0);
    const auto final_frame = receive_response(half_closed_socket);
    const auto final_response = glifistore::server::decode_response(final_frame);
    GLIFI_REQUIRE(final_response.has_value());
    GLIFI_REQUIRE(final_response->frame.request_id == 30);
    GLIFI_REQUIRE(final_response->frame.status == glifistore::server::ResponseStatus::ok);
    static_cast<void>(::close(half_closed_socket));

    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("half-close drains pipelined mutations before teardown") {
    // SHUT_WR means done-sending: frames already received must still run and ACK.
    // write_ready used to close on peer_read_closed before draining residual input.
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .worker_count = 1, .disk_read_thread_count = 1});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    const auto put1 = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 40,
        .key = bytes("half-close-a"),
        .value = bytes("one"),
    });
    const auto put2 = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 41,
        .key = bytes("half-close-b"),
        .value = bytes("two"),
    });
    GLIFI_REQUIRE(put1.has_value());
    GLIFI_REQUIRE(put2.has_value());
    std::vector<std::byte> pipeline(put1->begin(), put1->end());
    pipeline.insert(pipeline.end(), put2->begin(), put2->end());
    GLIFI_REQUIRE(send_all(socket, pipeline));
    GLIFI_REQUIRE(::shutdown(socket, SHUT_WR) == 0);

    const auto frame1 = receive_response(socket);
    const auto frame2 = receive_response(socket);
    const auto ack1 = glifistore::server::decode_response(frame1);
    const auto ack2 = glifistore::server::decode_response(frame2);
    GLIFI_REQUIRE(ack1.has_value());
    GLIFI_REQUIRE(ack2.has_value());
    GLIFI_REQUIRE(ack1->frame.request_id == 40);
    GLIFI_REQUIRE(ack2->frame.request_id == 41);
    GLIFI_REQUIRE(ack1->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(ack2->frame.status == glifistore::server::ResponseStatus::ok);

    static_cast<void>(::close(socket));

    const auto probe = connect_to(server.port());
    GLIFI_REQUIRE(probe >= 0);
    GLIFI_REQUIRE(initialize_and_bind(probe, 0, 1));
    for (const auto& [id, key, want] :
         {std::tuple{50ULL, "half-close-a", "one"}, std::tuple{51ULL, "half-close-b", "two"}}) {
        const auto get = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::get,
            .request_id = id,
            .key = bytes(key),
        });
        GLIFI_REQUIRE(get.has_value());
        GLIFI_REQUIRE(send_all(probe, *get));
        const auto get_frame = receive_response(probe);
        const auto got = glifistore::server::decode_response(get_frame);
        GLIFI_REQUIRE(got.has_value());
        GLIFI_REQUIRE(got->frame.status == glifistore::server::ResponseStatus::ok);
        GLIFI_REQUIRE(text(got->frame.value) == want);
    }
    static_cast<void>(::close(probe));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("bound Reactor redirects wrong owners without forwarding") {
    GLIFI_REQUIRE(glifistore::route_worker("bounded-key-0", 2) == 1);
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .worker_count = 2, .reuse_port = false});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto wrong_socket = connect_to(server.port());
    GLIFI_REQUIRE(wrong_socket >= 0);
    const auto premature_bind = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::bind_worker,
        .request_id = 98,
        .target_worker = 0,
    });
    GLIFI_REQUIRE(premature_bind.has_value());
    GLIFI_REQUIRE(send_all(wrong_socket, *premature_bind));
    const auto premature_bind_frame = receive_response(wrong_socket);
    const auto premature = glifistore::server::decode_response(premature_bind_frame);
    GLIFI_REQUIRE(premature.has_value());
    GLIFI_REQUIRE(premature->frame.status == glifistore::server::ResponseStatus::invalid_request);
    GLIFI_REQUIRE(premature->frame.owner_worker == glifistore::server::kNoWorker);

    const auto unbound_get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 99,
        .key = bytes("bounded-key-0"),
    });
    GLIFI_REQUIRE(unbound_get.has_value());
    GLIFI_REQUIRE(send_all(wrong_socket, *unbound_get));
    const auto unbound_frame = receive_response(wrong_socket);
    const auto unbound = glifistore::server::decode_response(unbound_frame);
    GLIFI_REQUIRE(unbound.has_value());
    GLIFI_REQUIRE(unbound->frame.status == glifistore::server::ResponseStatus::not_bound);
    GLIFI_REQUIRE(unbound->frame.owner_worker == 1);
    GLIFI_REQUIRE(initialize_and_bind(wrong_socket, 0, 2));
    const auto repeated_bind = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::bind_worker,
        .request_id = 100,
        .target_worker = 1,
    });
    GLIFI_REQUIRE(repeated_bind.has_value());
    GLIFI_REQUIRE(send_all(wrong_socket, *repeated_bind));
    const auto repeated_bind_frame = receive_response(wrong_socket);
    const auto repeated = glifistore::server::decode_response(repeated_bind_frame);
    GLIFI_REQUIRE(repeated.has_value());
    GLIFI_REQUIRE(repeated->frame.status == glifistore::server::ResponseStatus::invalid_request);
    GLIFI_REQUIRE(repeated->frame.owner_worker == 0);

    const auto misplaced_put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 101,
        .key = bytes("bounded-key-0"),
        .value = bytes("stored"),
    });
    GLIFI_REQUIRE(misplaced_put.has_value());
    GLIFI_REQUIRE(send_all(wrong_socket, *misplaced_put));
    const auto redirect_frame = receive_response(wrong_socket);
    const auto redirect = glifistore::server::decode_response(redirect_frame);
    GLIFI_REQUIRE(redirect.has_value());
    GLIFI_REQUIRE(redirect->frame.status == glifistore::server::ResponseStatus::wrong_owner);
    GLIFI_REQUIRE(redirect->frame.owner_worker == 1);
    static_cast<void>(::close(wrong_socket));

    const auto owner_socket = connect_to(server.port());
    GLIFI_REQUIRE(owner_socket >= 0);
    const auto owner_init = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::init,
        .request_id = 101,
    });
    const auto owner_bind = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::bind_worker,
        .request_id = 102,
        .target_worker = 1,
    });
    const auto missing_get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 103,
        .key = bytes("bounded-key-0"),
    });
    GLIFI_REQUIRE(owner_init.has_value());
    GLIFI_REQUIRE(owner_bind.has_value());
    GLIFI_REQUIRE(missing_get.has_value());
    std::vector<std::byte> bind_pipeline;
    bind_pipeline.insert(bind_pipeline.end(), owner_init->begin(), owner_init->end());
    bind_pipeline.insert(bind_pipeline.end(), owner_bind->begin(), owner_bind->end());
    bind_pipeline.insert(bind_pipeline.end(), missing_get->begin(), missing_get->end());
    GLIFI_REQUIRE(send_all(owner_socket, bind_pipeline));
    const auto owner_init_frame = receive_response(owner_socket);
    const auto owner_bind_frame = receive_response(owner_socket);
    const auto missing_frame = receive_response(owner_socket);
    const auto owner_initialized = glifistore::server::decode_response(owner_init_frame);
    const auto owner_bound = glifistore::server::decode_response(owner_bind_frame);
    const auto missing = glifistore::server::decode_response(missing_frame);
    GLIFI_REQUIRE(owner_initialized.has_value());
    GLIFI_REQUIRE(owner_initialized->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(owner_bound.has_value());
    GLIFI_REQUIRE(owner_bound->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(owner_bound->frame.owner_worker == 1);
    GLIFI_REQUIRE(missing.has_value());
    GLIFI_REQUIRE(missing->frame.status == glifistore::server::ResponseStatus::not_found);
    static_cast<void>(::close(owner_socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("multi-Reactor executors distribute connections and share one Store") {
    // The test creates 32 short-lived connections. Keep admission independent
    // from how quickly a platform's reactor observes peer close and reclaims a
    // slot; connection-limit recycling has its own focused test below.
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 32, .worker_count = 2, .executor_affinity = true});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.executor_count() == 2);
    GLIFI_REQUIRE(server.start().has_value());

    for (std::uint64_t request = 0; request < 16; ++request) {
        const auto key = std::string{"reuse-key-"} + std::to_string(request);
        const auto value = std::string{"reuse-value-"} + std::to_string(request);
        const auto owner = static_cast<std::uint32_t>(glifistore::route_worker(key, 2));
        const auto writer = connect_to(server.port());
        GLIFI_REQUIRE(writer >= 0);
        GLIFI_REQUIRE(initialize_and_bind(writer, owner, 2));
        const auto put = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::put,
            .request_id = request * 2U,
            .key = bytes(key),
            .value = bytes(value),
        });
        GLIFI_REQUIRE(put.has_value());
        GLIFI_REQUIRE(send_all(writer, *put));
        const auto put_frame = receive_response(writer);
        const auto put_response = glifistore::server::decode_response(put_frame);
        GLIFI_REQUIRE(put_response.has_value());
        GLIFI_REQUIRE(put_response->frame.status == glifistore::server::ResponseStatus::ok);
        static_cast<void>(::close(writer));

        const auto reader = connect_to(server.port());
        GLIFI_REQUIRE(reader >= 0);
        GLIFI_REQUIRE(initialize_and_bind(reader, owner, 2));
        const auto get = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::get,
            .request_id = request * 2U + 1U,
            .key = bytes(key),
        });
        GLIFI_REQUIRE(get.has_value());
        GLIFI_REQUIRE(send_all(reader, *get));
        const auto get_frame = receive_response(reader);
        const auto get_response = glifistore::server::decode_response(get_frame);
        GLIFI_REQUIRE(get_response.has_value());
        GLIFI_REQUIRE(get_response->frame.status == glifistore::server::ResponseStatus::ok);
        GLIFI_REQUIRE(text(get_response->frame.value) == value);
        static_cast<void>(::close(reader));
    }

    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
    const auto adopted = server.adopted_connections_per_executor();
    GLIFI_REQUIRE(adopted.size() == 2);
    GLIFI_REQUIRE(adopted[0] > 0);
    GLIFI_REQUIRE(adopted[1] > 0);
    const auto affinity = server.executor_affinity_results();
    GLIFI_REQUIRE(affinity.size() == 2);
    GLIFI_REQUIRE(affinity[0].mode != glifistore::server::ExecutorAffinityMode::disabled);
    GLIFI_REQUIRE(affinity[1].mode != glifistore::server::ExecutorAffinityMode::disabled);
}
