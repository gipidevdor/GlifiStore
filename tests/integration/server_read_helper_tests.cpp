#include "glifistore/core/fault_injection.hpp"
#include "glifistore/server/server.hpp"
#include "server_reactor_test_support.hpp"
#include "test.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace glifistore::test::server_reactor_support;
using namespace glifistore::server;

namespace {
auto request(const int socket, const RequestOpcode opcode, const std::uint64_t id,
             const std::string_view value = {}) -> bool {
    auto encoded = encode_request(
        {.opcode = opcode, .request_id = id, .key = bytes("helper-key"), .value = bytes(value)});
    return encoded && send_all(socket, *encoded);
}

auto response(const int socket, const std::uint64_t id, const std::string_view expected = {},
              const ResponseStatus status = ResponseStatus::ok) -> bool {
    const auto frame = receive_response(socket);
    const auto decoded = decode_response(frame);
    return decoded && decoded->frame.request_id == id && decoded->frame.status == status &&
           text(decoded->frame.value) == expected;
}
} // namespace

GLIFI_TEST("read helper volatile pipeline preserves mutation barriers large values and owner") {
    ReactorConfig config{.port = 0, .maximum_connections = 8};
    config.read_helpers = {.maximum_helpers = 2, .fixed_owner = 0};
    auto opened = Server::create(config);
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());
    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0 && initialize_and_bind(socket, 0, 1));
    for (const auto size : std::array<std::size_t, 5>{0, 64, 4096, 8192, 256U * 1024U}) {
        const std::string old_value(size, 'a');
        const std::string new_value(size, 'b');
        GLIFI_REQUIRE(request(socket, RequestOpcode::put, 1, old_value));
        GLIFI_REQUIRE(response(socket, 1));
        GLIFI_REQUIRE(request(socket, RequestOpcode::get, 2));
        GLIFI_REQUIRE(request(socket, RequestOpcode::put, 3, new_value));
        GLIFI_REQUIRE(request(socket, RequestOpcode::get, 4));
        GLIFI_REQUIRE(request(socket, RequestOpcode::erase, 5));
        GLIFI_REQUIRE(request(socket, RequestOpcode::get, 6));
        GLIFI_REQUIRE(response(socket, 2, old_value));
        GLIFI_REQUIRE(response(socket, 3));
        GLIFI_REQUIRE(response(socket, 4, new_value));
        GLIFI_REQUIRE(response(socket, 5));
        GLIFI_REQUIRE(response(socket, 6, {}, ResponseStatus::not_found));
    }
    GLIFI_REQUIRE(server.delegated_reads() >= 5);
    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("read helper unknown capacity keeps the primary path usable") {
    ReactorConfig config{.port = 0};
    config.read_helpers.maximum_helpers = 2;
    auto opened = Server::create(config);
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());
    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0 && initialize_and_bind(socket, 0, 1));
    GLIFI_REQUIRE(request(socket, RequestOpcode::put, 1, "primary"));
    GLIFI_REQUIRE(response(socket, 1));
    for (std::uint64_t id = 2; id < 40; ++id) {
        GLIFI_REQUIRE(request(socket, RequestOpcode::get, id));
        GLIFI_REQUIRE(response(socket, id, "primary"));
    }
    GLIFI_REQUIRE(server.delegated_reads() == 0);
    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

#if defined(GLIFISTORE_FAULT_INJECTION)
GLIFI_TEST("read helper exact epoch survives publication fallback and cancellation") {
    for (const bool slots : {false, true}) {
        ReactorConfig config{.port = 0, .maximum_connections = 4};
        config.read_helpers = {.maximum_helpers = 1, .fixed_owner = 0};
        glifistore::StoreConfig store_config;
        store_config.paired.generation_slot_pool = slots;
        auto opened = Server::create(config, store_config);
        GLIFI_REQUIRE(opened.has_value());
        auto& server = **opened;
        GLIFI_REQUIRE(server.start().has_value());
        const auto reader = connect_to(server.port());
        const auto writer = connect_to(server.port());
        GLIFI_REQUIRE(reader >= 0 && writer >= 0);
        GLIFI_REQUIRE(initialize_and_bind(reader, 0, 1));
        GLIFI_REQUIRE(initialize_and_bind(writer, 0, 1));
        const std::string old_value(256U * 1024U, 'o');
        const std::string new_value(256U * 1024U, 'n');
        GLIFI_REQUIRE(request(writer, RequestOpcode::put, 1, old_value));
        GLIFI_REQUIRE(response(writer, 1));

        struct Release final {
            ~Release() {
                glifistore::fault::release_block(glifistore::fault::Site::read_helper_selected);
            }
        };

        for (const bool cancel : {false, true}) {
            glifistore::fault::arm_block(glifistore::fault::Site::read_helper_selected);
            Release release;
            GLIFI_REQUIRE(request(reader, RequestOpcode::get, 2));
            GLIFI_REQUIRE(
                glifistore::fault::wait_until_blocked(glifistore::fault::Site::read_helper_selected));
            for (std::uint64_t id = 10; id < 26; ++id) {
                GLIFI_REQUIRE(request(writer, RequestOpcode::put, id, new_value));
                GLIFI_REQUIRE(response(writer, id));
            }
            // With its only helper occupied, this owner must still serve a primary GET.
            GLIFI_REQUIRE(request(writer, RequestOpcode::get, 30));
            GLIFI_REQUIRE(response(writer, 30, new_value));
            SocketHandle replacement;
            if (cancel) {
                const linger reset{.l_onoff = 1, .l_linger = 0};
                GLIFI_REQUIRE(::setsockopt(reader, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
                static_cast<void>(::close(reader));
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
                while (server.active_connections_per_executor()[0] != 1 &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                GLIFI_REQUIRE(server.active_connections_per_executor()[0] == 1);
                replacement.reset(connect_to(server.port()));
                GLIFI_REQUIRE(replacement.valid() && initialize_and_bind(replacement.descriptor(), 0, 1));
            }
            glifistore::fault::release_block(glifistore::fault::Site::read_helper_selected);
            if (!cancel) {
                GLIFI_REQUIRE(response(reader, 2, old_value));
            } else {
                GLIFI_REQUIRE(request(replacement.descriptor(), RequestOpcode::get, 40));
                GLIFI_REQUIRE(response(replacement.descriptor(), 40, new_value));
            }
        }
        static_cast<void>(::close(writer));
        server.request_stop();
        GLIFI_REQUIRE(server.join().has_value());
    }
}
#endif
