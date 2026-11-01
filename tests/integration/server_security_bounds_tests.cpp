#include "glifistore/server/authz.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/server.hpp"
#include "server_reactor_test_support.hpp"
#include "test.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace glifistore::test::server_reactor_support;

GLIFI_TEST("security bounds closing denied stream yields despite continued input") {
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    glifistore::server::AuthzPolicy policy;
    policy.bind("unix:uid=" + std::to_string(::geteuid()), glifistore::server::Capability::read);
    auto opened = glifistore::server::Server::create({
        .port = 0,
        .maximum_connections = 8,
        .accepted_socket_send_buffer_bytes = 4U * 1024U,
        .shutdown_drain_ms = 200,
        .authz = policy,
        .abuse = {.request_timeout_ms = 1'000, .connection_max_requests_per_sec = 3},
        .unix_socket_path = path,
        .unix_peercred = true,
        .unix_peercred_required = true,
    });
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());
    glifistore::server::SocketHandle socket{::socket(AF_UNIX, SOCK_STREAM, 0)};
    GLIFI_REQUIRE(socket.valid());
    sockaddr_un endpoint{};
    endpoint.sun_family = AF_UNIX;
    const auto path_text = path.string();
    GLIFI_REQUIRE(path_text.size() < sizeof(endpoint.sun_path));
    std::memcpy(endpoint.sun_path, path_text.c_str(), path_text.size() + 1U);
    GLIFI_REQUIRE(
        ::connect(socket.descriptor(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0);
    const timeval timeout{.tv_sec = 2, .tv_usec = 0};
    GLIFI_REQUIRE(::setsockopt(socket.descriptor(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    GLIFI_REQUIRE(initialize_and_bind(socket.descriptor(), 0, 1));
    const std::vector<std::byte> payload(512U * 1024U, std::byte{0x42});
    const auto ping = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 3,
        .value = payload,
    });
    const auto denied = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 4,
        .key = bytes("denied"),
    });
    GLIFI_REQUIRE(ping.has_value());
    GLIFI_REQUIRE(denied.has_value());
    std::vector<std::byte> pipeline(ping->begin(), ping->end());
    pipeline.insert(pipeline.end(), denied->begin(), denied->end());
    GLIFI_REQUIRE(send_all(socket.descriptor(), pipeline));
    char peek{};
    GLIFI_REQUIRE(::recv(socket.descriptor(), &peek, 1, MSG_PEEK) == 1);
    std::jthread producer{[&](std::stop_token stop) {
        std::array<std::byte, 16U * 1024U> junk{};
        while (!stop.stop_requested()) {
            const auto sent = ::send(socket.descriptor(), junk.data(), junk.size(), MSG_DONTWAIT);
            if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            }
            std::this_thread::yield();
        }
    }};
    const auto probe = connect_to(server.port());
    GLIFI_REQUIRE(probe >= 0);
    const auto health = probe_lifecycle(probe, glifistore::server::RequestOpcode::health, 50);
    static_cast<void>(::close(probe));
    GLIFI_REQUIRE(health.has_value());
    GLIFI_REQUIRE(health->decoded.frame.status == glifistore::server::ResponseStatus::ok);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (server.active_connections_per_executor()[0] != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    producer.request_stop();
    producer.join();
    GLIFI_REQUIRE(server.active_connections_per_executor()[0] == 0);
    const auto report = server.stats_report();
    GLIFI_REQUIRE(report.has_value());
    GLIFI_REQUIRE(report->find("abuse_request_timeout_closed=1\n") != std::string::npos);
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("security bounds capability prefix and stats denials share principal quota") {
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path(); // Short existing private parent, new UDS leaf.
    glifistore::server::AuthzPolicy policy;
    policy.bind("unix:uid=" + std::to_string(::geteuid()), glifistore::server::Capability::read, "tenant:");
    auto opened = glifistore::server::Server::create({
        .port = 0,
        .authz = policy,
        .abuse = {.principal_max_requests_per_sec = 6},
        .unix_socket_path = path,
        .unix_peercred = true,
        .unix_peercred_required = true,
    });
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());
    glifistore::server::SocketHandle socket{::socket(AF_UNIX, SOCK_STREAM, 0)};
    GLIFI_REQUIRE(socket.valid());
    sockaddr_un endpoint{};
    endpoint.sun_family = AF_UNIX;
    const auto path_text = path.string();
    GLIFI_REQUIRE(path_text.size() < sizeof(endpoint.sun_path));
    std::memcpy(endpoint.sun_path, path_text.c_str(), path_text.size() + 1U);
    GLIFI_REQUIRE(
        ::connect(socket.descriptor(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0);
    const timeval timeout{.tv_sec = 2, .tv_usec = 0};
    GLIFI_REQUIRE(::setsockopt(socket.descriptor(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    GLIFI_REQUIRE(initialize_and_bind(socket.descriptor(), 0, 1));
    const auto allowed = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 10,
        .key = bytes("tenant:missing"),
    });
    GLIFI_REQUIRE(allowed.has_value());
    GLIFI_REQUIRE(send_all(socket.descriptor(), *allowed));
    const auto allowed_frame = receive_response(socket.descriptor());
    const auto allowed_reply = glifistore::server::decode_response(allowed_frame);
    GLIFI_REQUIRE(allowed_reply.has_value());
    GLIFI_REQUIRE(allowed_reply->frame.status == glifistore::server::ResponseStatus::not_found);
    const std::array opcodes{glifistore::server::RequestOpcode::put, glifistore::server::RequestOpcode::get,
                             glifistore::server::RequestOpcode::stats,
                             glifistore::server::RequestOpcode::put};
    std::vector<std::byte> pipeline;
    for (std::size_t index = 0; index < opcodes.size(); ++index) {
        const auto request = glifistore::server::encode_request({
            .opcode = opcodes[index],
            .request_id = 20U + index,
            .key = opcodes[index] == glifistore::server::RequestOpcode::stats ? std::span<const std::byte>{}
                                                                              : bytes("foreign:key"),
        });
        GLIFI_REQUIRE(request.has_value());
        pipeline.insert(pipeline.end(), request->begin(), request->end());
    }
    GLIFI_REQUIRE(send_all(socket.descriptor(), pipeline));
    for (std::size_t index = 0; index < opcodes.size(); ++index) {
        const auto frame = receive_response(socket.descriptor());
        const auto response = glifistore::server::decode_response(frame);
        GLIFI_REQUIRE(response.has_value());
        GLIFI_REQUIRE(response->frame.request_id == 20U + index);
        GLIFI_REQUIRE(response->frame.status == (index < 3
                                                     ? glifistore::server::ResponseStatus::permission_denied
                                                     : glifistore::server::ResponseStatus::overloaded));
    }
    char byte{};
    GLIFI_REQUIRE(::recv(socket.descriptor(), &byte, 1, 0) == 0);
    const auto report = server.stats_report();
    GLIFI_REQUIRE(report.has_value());
    GLIFI_REQUIRE(report->find("abuse_principal_request_rejected=1\n") != std::string::npos);
    GLIFI_REQUIRE(report->find("authz_denies=3\n") != std::string::npos);
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("security bounds denied requests and denied probes consume attempt quota") {
    for (const auto opcode :
         {glifistore::server::RequestOpcode::put, glifistore::server::RequestOpcode::stats}) {
        glifistore::server::AuthzPolicy policy;
        policy.set_enabled(true); // Anonymous cleartext has no grants.
        auto opened = glifistore::server::Server::create({
            .port = 0,
            .maximum_connections = 4,
            .authz = policy,
            .abuse = {.connection_max_requests_per_sec = 2},
        });
        GLIFI_REQUIRE(opened.has_value());
        auto& server = **opened;
        GLIFI_REQUIRE(server.start().has_value());
        const auto socket = connect_to(server.port());
        GLIFI_REQUIRE(socket >= 0);
        std::vector<std::byte> pipeline;
        for (std::uint64_t id = 1; id <= 4; ++id) {
            const auto frame = glifistore::server::encode_request({
                .opcode = opcode,
                .request_id = id,
                .key = opcode == glifistore::server::RequestOpcode::put ? bytes("key")
                                                                        : std::span<const std::byte>{},
            });
            GLIFI_REQUIRE(frame.has_value());
            pipeline.insert(pipeline.end(), frame->begin(), frame->end());
        }
        GLIFI_REQUIRE(send_all(socket, pipeline));
        for (std::uint64_t id = 1; id <= 3; ++id) {
            const auto frame = receive_response(socket);
            const auto response = glifistore::server::decode_response(frame);
            GLIFI_REQUIRE(response.has_value());
            GLIFI_REQUIRE(response->frame.request_id == id);
            GLIFI_REQUIRE(response->frame.status ==
                          (id < 3 ? glifistore::server::ResponseStatus::permission_denied
                                  : glifistore::server::ResponseStatus::overloaded));
        }
        char byte{};
        GLIFI_REQUIRE(::recv(socket, &byte, 1, 0) == 0);
        const auto report = server.stats_report();
        GLIFI_REQUIRE(report.has_value());
        GLIFI_REQUIRE(report->find("authz_denies=2\n") != std::string::npos);
        // Authorized HEALTH remains observable after another connection exhausts attempts.
        const auto probe = connect_to(server.port());
        GLIFI_REQUIRE(probe >= 0);
        const auto health = probe_lifecycle(probe, glifistore::server::RequestOpcode::health, 90);
        GLIFI_REQUIRE(health.has_value());
        GLIFI_REQUIRE(health->decoded.frame.status == glifistore::server::ResponseStatus::ok);
        static_cast<void>(::close(probe));
        static_cast<void>(::close(socket));
        server.request_stop();
        GLIFI_REQUIRE(server.join().has_value());
    }
}

GLIFI_TEST("security bounds stalled output expires and releases connection slot") {
    // Both PING's contiguous output and a hot GET's scatter lease must expire.
    for (const bool scatter_get : {false, true}) {
        auto opened = glifistore::server::Server::create({
            .port = 0,
            .maximum_connections = 1,
            .accepted_socket_send_buffer_bytes = 4U * 1024U,
            .shutdown_drain_ms = 200,
            .abuse = {.request_timeout_ms = 1'000},
        });
        GLIFI_REQUIRE(opened.has_value());
        auto& server = **opened;
        GLIFI_REQUIRE(server.start().has_value());
        const auto socket = connect_to(server.port());
        GLIFI_REQUIRE(socket >= 0);
        const int receive_buffer = 4 * 1024;
        GLIFI_REQUIRE(::setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)) ==
                      0);
        GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
        const std::vector<std::byte> payload(768U * 1024U, std::byte{0x5a});
        if (scatter_get) {
            const auto put = glifistore::server::encode_request({
                .opcode = glifistore::server::RequestOpcode::put,
                .request_id = 30,
                .key = bytes("output-deadline"),
                .value = payload,
            });
            GLIFI_REQUIRE(put.has_value());
            GLIFI_REQUIRE(send_all(socket, *put));
            const auto frame = receive_response(socket);
            const auto response = glifistore::server::decode_response(frame);
            GLIFI_REQUIRE(response.has_value());
            GLIFI_REQUIRE(response->frame.status == glifistore::server::ResponseStatus::ok);
        }
        const auto request = glifistore::server::encode_request({
            .opcode = scatter_get ? glifistore::server::RequestOpcode::get
                                  : glifistore::server::RequestOpcode::ping,
            .request_id = 31,
            .key = scatter_get ? bytes("output-deadline") : std::span<const std::byte>{},
            .value = scatter_get ? std::span<const std::byte>{} : std::span<const std::byte>{payload},
        });
        GLIFI_REQUIRE(request.has_value());
        GLIFI_REQUIRE(send_all(socket, *request));
        // Observe a decided response without consuming its receive window.
        char peek{};
        GLIFI_REQUIRE(::recv(socket, &peek, 1, MSG_PEEK) == 1);
        GLIFI_REQUIRE(server.active_connections_per_executor()[0] == 1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (server.active_connections_per_executor()[0] != 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        GLIFI_REQUIRE(server.active_connections_per_executor()[0] == 0);
        const auto report = server.stats_report();
        GLIFI_REQUIRE(report.has_value());
        GLIFI_REQUIRE(report->find("abuse_request_timeout_closed=1\n") != std::string::npos);
        if (scatter_get) {
            GLIFI_REQUIRE(report->find("output_scatter_responses=1\n") != std::string::npos);
        }
        const auto replacement = connect_to(server.port());
        GLIFI_REQUIRE(replacement >= 0);
        GLIFI_REQUIRE(initialize_and_bind(replacement, 0, 1));
        static_cast<void>(::close(replacement));
        static_cast<void>(::close(socket));
        server.request_stop();
        GLIFI_REQUIRE(server.join().has_value());
    }
}
