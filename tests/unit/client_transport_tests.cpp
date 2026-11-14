#include "client/client_detail.hpp"
#include "test.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <span>
#include <sys/socket.h>

namespace {

using namespace glifistore;
using namespace glifistore::client::detail;

struct ConnectedPair final {
    WorkerConnection connection{0};
    Socket peer;

    ConnectedPair() {
        std::array<int, 2> descriptors{};
        GLIFI_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) == 0);
        connection.socket = Socket{descriptors[0]};
        peer = Socket{descriptors[1]};
    }
};

auto response_frame() -> std::vector<std::byte> {
    auto encoded = server::encode_response({.status = server::ResponseStatus::ok,
                                            .request_id = 7,
                                            .owner_worker = 0,
                                            .worker_count = 1,
                                            .routing_epoch = 7});
    GLIFI_REQUIRE(encoded.has_value());
    return std::move(*encoded);
}

void require_timeout(const ExchangeResult& result, const std::size_t bytes_sent) {
    const auto* failure = std::get_if<ExchangeFailure>(&result);
    GLIFI_REQUIRE(failure != nullptr);
    GLIFI_REQUIRE(failure->error.category == "transport");
    GLIFI_REQUIRE(failure->request_bytes_sent == bytes_sent);
}

} // namespace

GLIFI_TEST("C++ client expired deadline prevents sending on a writable socket") {
    ConnectedPair pair;
    const auto frame = response_frame();
    auto result = send_frame(pair.connection, frame, Clock::now() - std::chrono::seconds{1});
    const auto* failure = std::get_if<ExchangeFailure>(&result);
    GLIFI_REQUIRE(failure != nullptr);
    GLIFI_REQUIRE(failure->request_bytes_sent == 0);
    GLIFI_REQUIRE(failure->error.category == "transport");
    std::byte received{};
    GLIFI_REQUIRE(::recv(pair.peer.get(), &received, 1, MSG_DONTWAIT) == -1);
    GLIFI_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);
}

GLIFI_TEST("C++ client expired deadline rejects a response already readable on the socket") {
    ConnectedPair pair;
    const auto frame = response_frame();
    GLIFI_REQUIRE(::send(pair.peer.get(), frame.data(), frame.size(), 0) ==
                  static_cast<ssize_t>(frame.size()));
    auto result = receive_response(pair.connection, {}, Clock::now() - std::chrono::seconds{1}, 91);
    require_timeout(result, 91);
}

GLIFI_TEST("C++ client expired pipeline deadline rejects an already buffered response") {
    ConnectedPair pair;
    pair.connection.input = response_frame();
    auto result = receive_buffered_response(pair.connection, {}, Clock::now() - std::chrono::seconds{1}, 91);
    require_timeout(result, 91);
    GLIFI_REQUIRE(pair.connection.input_offset == 0);
}
