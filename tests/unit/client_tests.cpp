#include "glifistore/client/client.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/little_endian.hpp"
#include "glifistore/server/authz.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/server.hpp"
#include "test.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

auto bytes(const std::string_view value) noexcept -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

auto text(const std::span<const std::byte> value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto receive_exact(const int descriptor, const std::span<std::byte> output) -> bool {
    std::size_t received{};
    while (received < output.size()) {
        const auto count = ::recv(descriptor, output.data() + received, output.size() - received, 0);
        if (count <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(count);
    }
    return true;
}

auto load_u32(const std::span<const std::byte> input) noexcept -> std::uint32_t {
    return glifistore::le::get_u32(input, 0);
}

auto receive_request(const int descriptor) -> std::vector<std::byte> {
    std::array<std::byte, sizeof(std::uint32_t)> size{};
    if (!receive_exact(descriptor, size)) {
        return {};
    }
    const auto frame_size = static_cast<std::size_t>(load_u32(size));
    if (frame_size < glifistore::server::kRequestHeaderBytes ||
        frame_size > glifistore::server::kMaxFrameBytes) {
        return {};
    }
    std::vector<std::byte> frame(frame_size);
    std::ranges::copy(size, frame.begin());
    if (!receive_exact(descriptor, std::span<std::byte>{frame}.subspan(size.size()))) {
        return {};
    }
    return frame;
}

auto send_all(const int descriptor, const std::span<const std::byte> input) -> bool {
    std::size_t sent{};
    while (sent < input.size()) {
        const auto count = ::send(descriptor, input.data() + sent, input.size() - sent, 0);
        if (count <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

class DisconnectingPipelineServer final {
  public:
    DisconnectingPipelineServer() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        GLIFI_REQUIRE(listener_ >= 0);
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        endpoint.sin_port = 0;
        GLIFI_REQUIRE(::bind(listener_, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0);
        GLIFI_REQUIRE(::listen(listener_, 1) == 0);
        socklen_t endpoint_size = sizeof(endpoint);
        GLIFI_REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&endpoint), &endpoint_size) == 0);
        port_ = ntohs(endpoint.sin_port);
        thread_ = std::thread{[this] { run(); }};
    }

    ~DisconnectingPipelineServer() {
        if (thread_.joinable()) {
            thread_.join();
        }
        static_cast<void>(::close(listener_));
    }

    [[nodiscard]] auto port() const noexcept -> std::uint16_t {
        return port_;
    }

  private:
    void run() noexcept {
        const auto descriptor = ::accept(listener_, nullptr, nullptr);
        if (descriptor < 0) {
            return;
        }
        const auto reply = [&](const glifistore::server::ResponseView& response) {
            auto encoded = glifistore::server::encode_response(response);
            return encoded && send_all(descriptor, *encoded);
        };
        auto init_frame = receive_request(descriptor);
        auto init = glifistore::server::decode_request(init_frame);
        if (!init || !reply({.status = glifistore::server::ResponseStatus::ok,
                             .request_id = init->frame.request_id,
                             .owner_worker = 0,
                             .worker_count = 1,
                             .routing_epoch = 7,
                             .value = bytes("GlifiStore/2")})) {
            static_cast<void>(::close(descriptor));
            return;
        }
        auto bind_frame = receive_request(descriptor);
        auto bind = glifistore::server::decode_request(bind_frame);
        if (!bind || !reply({.status = glifistore::server::ResponseStatus::ok,
                             .request_id = bind->frame.request_id,
                             .owner_worker = 0,
                             .worker_count = 1,
                             .routing_epoch = 7})) {
            static_cast<void>(::close(descriptor));
            return;
        }
        for (std::size_t request = 0; request < 3; ++request) {
            if (receive_request(descriptor).empty()) {
                static_cast<void>(::close(descriptor));
                return;
            }
        }
        static_cast<void>(::close(descriptor));
    }

    int listener_{-1};
    std::uint16_t port_{};
    std::thread thread_;
};

class OverloadedPutServer final {
  public:
    OverloadedPutServer() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        GLIFI_REQUIRE(listener_ >= 0);
        int yes = 1;
        GLIFI_REQUIRE(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        GLIFI_REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        socklen_t length = sizeof(address);
        GLIFI_REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        GLIFI_REQUIRE(::listen(listener_, 1) == 0);
        thread_ = std::thread([this] { run(); });
    }

    ~OverloadedPutServer() {
        if (thread_.joinable()) {
            thread_.join();
        }
        static_cast<void>(::close(listener_));
    }

    [[nodiscard]] auto port() const noexcept -> std::uint16_t {
        return port_;
    }

  private:
    void run() noexcept {
        const auto descriptor = ::accept(listener_, nullptr, nullptr);
        if (descriptor < 0) {
            return;
        }
        const auto reply = [&](const glifistore::server::ResponseView& response) {
            auto encoded = glifistore::server::encode_response(response);
            return encoded && send_all(descriptor, *encoded);
        };
        auto init_frame = receive_request(descriptor);
        auto init = glifistore::server::decode_request(init_frame);
        if (!init || !reply({.status = glifistore::server::ResponseStatus::ok,
                             .request_id = init->frame.request_id,
                             .owner_worker = 0,
                             .worker_count = 1,
                             .routing_epoch = 7,
                             .value = bytes("GlifiStore/2")})) {
            static_cast<void>(::close(descriptor));
            return;
        }
        auto bind_frame = receive_request(descriptor);
        auto bind = glifistore::server::decode_request(bind_frame);
        if (!bind || !reply({.status = glifistore::server::ResponseStatus::ok,
                             .request_id = bind->frame.request_id,
                             .owner_worker = 0,
                             .worker_count = 1,
                             .routing_epoch = 7})) {
            static_cast<void>(::close(descriptor));
            return;
        }
        auto put_frame = receive_request(descriptor);
        auto put = glifistore::server::decode_request(put_frame);
        if (put) {
            static_cast<void>(reply({.status = glifistore::server::ResponseStatus::overloaded,
                                     .request_id = put->frame.request_id,
                                     .owner_worker = 0,
                                     .worker_count = 1,
                                     .routing_epoch = 7}));
        }
        static_cast<void>(::close(descriptor));
    }

    int listener_{-1};
    std::uint16_t port_{};
    std::thread thread_;
};

class BackupDropResponseServer final {
  public:
    BackupDropResponseServer() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        GLIFI_REQUIRE(listener_ >= 0);
        int yes = 1;
        GLIFI_REQUIRE(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        GLIFI_REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        socklen_t length = sizeof(address);
        GLIFI_REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        GLIFI_REQUIRE(::listen(listener_, 8) == 0);
        thread_ = std::thread([this] { run(); });
    }

    ~BackupDropResponseServer() {
        stop_.store(true, std::memory_order_release);
        // Unblock accept.
        const auto probe = ::socket(AF_INET, SOCK_STREAM, 0);
        if (probe >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port_);
            static_cast<void>(::connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
            static_cast<void>(::close(probe));
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        static_cast<void>(::close(listener_));
    }

    [[nodiscard]] auto port() const noexcept -> std::uint16_t {
        return port_;
    }

    [[nodiscard]] auto backup_requests() const noexcept -> std::uint32_t {
        return backup_requests_.load(std::memory_order_acquire);
    }

  private:
    void run() noexcept {
        while (!stop_.load(std::memory_order_acquire)) {
            const auto descriptor = ::accept(listener_, nullptr, nullptr);
            if (descriptor < 0) {
                return;
            }
            if (stop_.load(std::memory_order_acquire)) {
                static_cast<void>(::close(descriptor));
                return;
            }
            const auto reply = [&](const glifistore::server::ResponseView& response) {
                auto encoded = glifistore::server::encode_response(response);
                return encoded && send_all(descriptor, *encoded);
            };
            auto init_frame = receive_request(descriptor);
            auto init = glifistore::server::decode_request(init_frame);
            if (!init || !reply({.status = glifistore::server::ResponseStatus::ok,
                                 .request_id = init->frame.request_id,
                                 .owner_worker = 0,
                                 .worker_count = 1,
                                 .routing_epoch = 7,
                                 .value = bytes("GlifiStore/2")})) {
                static_cast<void>(::close(descriptor));
                continue;
            }
            auto bind_frame = receive_request(descriptor);
            auto bind = glifistore::server::decode_request(bind_frame);
            if (!bind || !reply({.status = glifistore::server::ResponseStatus::ok,
                                 .request_id = bind->frame.request_id,
                                 .owner_worker = 0,
                                 .worker_count = 1,
                                 .routing_epoch = 7})) {
                static_cast<void>(::close(descriptor));
                continue;
            }
            auto backup_frame = receive_request(descriptor);
            auto backup = glifistore::server::decode_request(backup_frame);
            if (backup && backup->frame.opcode == glifistore::server::RequestOpcode::backup) {
                backup_requests_.fetch_add(1U, std::memory_order_acq_rel);
            }
            // Drop the response so the client observes transport loss after send.
            static_cast<void>(::close(descriptor));
        }
    }

    int listener_{-1};
    std::uint16_t port_{};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint32_t> backup_requests_{0};
};

class BackupInternalErrorServer final {
  public:
    BackupInternalErrorServer() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        GLIFI_REQUIRE(listener_ >= 0);
        int yes = 1;
        GLIFI_REQUIRE(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        GLIFI_REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        socklen_t length = sizeof(address);
        GLIFI_REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        GLIFI_REQUIRE(::listen(listener_, 8) == 0);
        thread_ = std::thread([this] { run(); });
    }

    ~BackupInternalErrorServer() {
        stop_.store(true, std::memory_order_release);
        const auto probe = ::socket(AF_INET, SOCK_STREAM, 0);
        if (probe >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port_);
            static_cast<void>(::connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
            static_cast<void>(::close(probe));
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        static_cast<void>(::close(listener_));
    }

    [[nodiscard]] auto port() const noexcept -> std::uint16_t {
        return port_;
    }

    [[nodiscard]] auto backup_requests() const noexcept -> std::uint32_t {
        return backup_requests_.load(std::memory_order_acquire);
    }

  private:
    void run() noexcept {
        while (!stop_.load(std::memory_order_acquire)) {
            const auto descriptor = ::accept(listener_, nullptr, nullptr);
            if (descriptor < 0) {
                return;
            }
            if (stop_.load(std::memory_order_acquire)) {
                static_cast<void>(::close(descriptor));
                return;
            }
            const auto reply = [&](const glifistore::server::ResponseView& response) {
                auto encoded = glifistore::server::encode_response(response);
                return encoded && send_all(descriptor, *encoded);
            };
            auto init_frame = receive_request(descriptor);
            auto init = glifistore::server::decode_request(init_frame);
            if (!init || !reply({.status = glifistore::server::ResponseStatus::ok,
                                 .request_id = init->frame.request_id,
                                 .owner_worker = 0,
                                 .worker_count = 1,
                                 .routing_epoch = 7,
                                 .value = bytes("GlifiStore/2")})) {
                static_cast<void>(::close(descriptor));
                continue;
            }
            auto bind_frame = receive_request(descriptor);
            auto bind = glifistore::server::decode_request(bind_frame);
            if (!bind || !reply({.status = glifistore::server::ResponseStatus::ok,
                                 .request_id = bind->frame.request_id,
                                 .owner_worker = 0,
                                 .worker_count = 1,
                                 .routing_epoch = 7})) {
                static_cast<void>(::close(descriptor));
                continue;
            }
            auto backup_frame = receive_request(descriptor);
            auto backup = glifistore::server::decode_request(backup_frame);
            if (backup && backup->frame.opcode == glifistore::server::RequestOpcode::backup) {
                backup_requests_.fetch_add(1U, std::memory_order_acq_rel);
                static_cast<void>(reply({.status = glifistore::server::ResponseStatus::internal_error,
                                         .request_id = backup->frame.request_id,
                                         .owner_worker = 0,
                                         .worker_count = 1,
                                         .routing_epoch = 7,
                                         .value = bytes("report failed")}));
            }
            static_cast<void>(::close(descriptor));
        }
    }

    int listener_{-1};
    std::uint16_t port_{};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint32_t> backup_requests_{0};
};

class BackupWrongRequestIdServer final {
  public:
    BackupWrongRequestIdServer() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        GLIFI_REQUIRE(listener_ >= 0);
        int yes = 1;
        GLIFI_REQUIRE(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        GLIFI_REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        socklen_t length = sizeof(address);
        GLIFI_REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        GLIFI_REQUIRE(::listen(listener_, 8) == 0);
        thread_ = std::thread([this] { run(); });
    }

    ~BackupWrongRequestIdServer() {
        stop_.store(true, std::memory_order_release);
        const auto probe = ::socket(AF_INET, SOCK_STREAM, 0);
        if (probe >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port_);
            static_cast<void>(::connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
            static_cast<void>(::close(probe));
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        static_cast<void>(::close(listener_));
    }

    [[nodiscard]] auto port() const noexcept -> std::uint16_t {
        return port_;
    }

    [[nodiscard]] auto backup_requests() const noexcept -> std::uint32_t {
        return backup_requests_.load(std::memory_order_acquire);
    }

  private:
    void run() noexcept {
        while (!stop_.load(std::memory_order_acquire)) {
            const auto descriptor = ::accept(listener_, nullptr, nullptr);
            if (descriptor < 0) {
                return;
            }
            if (stop_.load(std::memory_order_acquire)) {
                static_cast<void>(::close(descriptor));
                return;
            }
            const auto reply = [&](const glifistore::server::ResponseView& response) {
                auto encoded = glifistore::server::encode_response(response);
                return encoded && send_all(descriptor, *encoded);
            };
            auto init_frame = receive_request(descriptor);
            auto init = glifistore::server::decode_request(init_frame);
            if (!init || !reply({.status = glifistore::server::ResponseStatus::ok,
                                 .request_id = init->frame.request_id,
                                 .owner_worker = 0,
                                 .worker_count = 1,
                                 .routing_epoch = 7,
                                 .value = bytes("GlifiStore/2")})) {
                static_cast<void>(::close(descriptor));
                continue;
            }
            auto bind_frame = receive_request(descriptor);
            auto bind = glifistore::server::decode_request(bind_frame);
            if (!bind || !reply({.status = glifistore::server::ResponseStatus::ok,
                                 .request_id = bind->frame.request_id,
                                 .owner_worker = 0,
                                 .worker_count = 1,
                                 .routing_epoch = 7})) {
                static_cast<void>(::close(descriptor));
                continue;
            }
            auto backup_frame = receive_request(descriptor);
            auto backup = glifistore::server::decode_request(backup_frame);
            if (backup && backup->frame.opcode == glifistore::server::RequestOpcode::backup) {
                backup_requests_.fetch_add(1U, std::memory_order_acq_rel);
                static_cast<void>(reply({.status = glifistore::server::ResponseStatus::ok,
                                         .request_id = backup->frame.request_id ^ 1U,
                                         .owner_worker = 0,
                                         .worker_count = 1,
                                         .routing_epoch = 7,
                                         .value = bytes("status=ok files=0 bytes=0")}));
            }
            static_cast<void>(::close(descriptor));
        }
    }

    int listener_{-1};
    std::uint16_t port_{};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint32_t> backup_requests_{0};
};

class RunningServer final {
  public:
    explicit RunningServer(const std::size_t workers = 2) {
        auto created = glifistore::server::Server::create(
            {.port = 0, .maximum_connections = 64, .worker_count = workers});
        if (!created) {
            throw std::runtime_error{"client test server creation failed (code " +
                                     std::to_string(static_cast<int>(created.error().code)) +
                                     "): " + created.error().message};
        }
        server_ = std::move(*created);
        GLIFI_REQUIRE(server_->start().has_value());
    }

    ~RunningServer() {
        server_->request_stop();
        static_cast<void>(server_->join());
    }

    [[nodiscard]] auto port() const noexcept -> std::uint16_t {
        return server_->port();
    }

  private:
    std::unique_ptr<glifistore::server::Server> server_;
};

auto key_for_worker(const std::size_t worker, const std::size_t worker_count) -> std::string {
    for (std::size_t candidate = 0;; ++candidate) {
        auto key = "client-worker-" + std::to_string(worker) + '-' + std::to_string(candidate);
        if (glifistore::route_worker(glifistore::hash_key(key), worker_count) == worker) {
            return key;
        }
    }
}

} // namespace

GLIFI_TEST("C++ client bootstraps every worker and handles binary cache operations") {
    RunningServer server;
    auto connected = glifistore::client::Client::connect({.port = server.port(), .maximum_frame_bytes = 64});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);
    GLIFI_REQUIRE(client.healthy());
    GLIFI_REQUIRE(client.worker_count() == 2);
    GLIFI_REQUIRE(client.routing_epoch() != 0);

    const std::array<std::byte, 4> ping_payload{std::byte{0}, std::byte{1}, std::byte{0xFE}, std::byte{0xFF}};
    auto pong = client.ping(ping_payload);
    GLIFI_REQUIRE(pong.has_value());
    GLIFI_REQUIRE(*pong == std::vector<std::byte>(ping_payload.begin(), ping_payload.end()));

    const std::array<std::byte, 5> key{std::byte{'k'}, std::byte{0}, std::byte{'e'}, std::byte{'y'},
                                       std::byte{0xFF}};
    const std::array<std::byte, 5> value{std::byte{0}, std::byte{'v'}, std::byte{'a'}, std::byte{'l'},
                                         std::byte{0xFE}};
    const auto stored = client.put(key, value);
    GLIFI_REQUIRE(stored.committed());
    GLIFI_REQUIRE(!stored.error.has_value());

    auto loaded = client.get(key);
    GLIFI_REQUIRE(loaded.has_value());
    GLIFI_REQUIRE(*loaded == std::vector<std::byte>(value.begin(), value.end()));

    const auto erased = client.erase(key);
    GLIFI_REQUIRE(erased.committed());
    auto missing = client.get(key);
    GLIFI_REQUIRE(!missing.has_value());
    GLIFI_REQUIRE(missing.error().code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE(missing.error().category == "not_found");
    GLIFI_REQUIRE(missing.error().wire_status.has_value());
    GLIFI_REQUIRE(*missing.error().wire_status ==
                  static_cast<std::uint16_t>(glifistore::server::ResponseStatus::not_found));
    GLIFI_REQUIRE(missing.error().retryability == "new_attempt");
    GLIFI_REQUIRE(missing.error().operation == "get");

    const std::array<std::byte, 32> oversized_value{};
    const auto oversized = client.put(key, oversized_value);
    GLIFI_REQUIRE(oversized.outcome == glifistore::client::MutationOutcome::rejected);
    GLIFI_REQUIRE(oversized.error.has_value());
    GLIFI_REQUIRE(oversized.error->code == glifistore::ErrorCode::record_too_large);
    GLIFI_REQUIRE(oversized.error->category == "invalid_argument");
    GLIFI_REQUIRE(oversized.error->retryability == "never");
    GLIFI_REQUIRE(oversized.error->operation == "put");
    GLIFI_REQUIRE(oversized.error->bytes_sent == 0);

    client.close();
    GLIFI_REQUIRE(!client.healthy());
    GLIFI_REQUIRE(!client.get("after-close").has_value());
}

GLIFI_TEST("C++ client erase of absent key is rejected") {
    // client-semantics §3: wire NOT_FOUND on standalone ERASE → rejected.
    RunningServer server;
    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);
    const auto result = client.erase("missing-key");
    GLIFI_REQUIRE(result.outcome == glifistore::client::MutationOutcome::rejected);
    GLIFI_REQUIRE(result.error.has_value());
    GLIFI_REQUIRE(result.error->code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE(result.error->category == "not_found");
    GLIFI_REQUIRE(result.error->mutation_outcome == "rejected");
    GLIFI_REQUIRE(result.error->retryability == "new_attempt");
    GLIFI_REQUIRE(result.error->operation == "erase");
    client.close();
}

GLIFI_TEST("C++ client pipeline erase of absent key is failed rejected") {
    // client-semantics §3: wire NOT_FOUND on pipeline ERASE → failed + rejected.
    RunningServer server;
    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);
    const std::array requests{
        glifistore::client::PipelineRequest{.opcode = glifistore::client::PipelineOpcode::erase,
                                            .key = bytes("missing-key")},
    };
    const auto executed = client.execute_pipeline(requests);
    GLIFI_REQUIRE(executed.has_value());
    GLIFI_REQUIRE(executed->size() == 1);
    GLIFI_REQUIRE((*executed)[0].outcome == glifistore::client::PipelineOutcome::failed);
    GLIFI_REQUIRE((*executed)[0].error.has_value());
    GLIFI_REQUIRE((*executed)[0].error->code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE((*executed)[0].error->category == "not_found");
    GLIFI_REQUIRE((*executed)[0].error->mutation_outcome == "rejected");
    GLIFI_REQUIRE((*executed)[0].error->retryability == "new_attempt");
    GLIFI_REQUIRE((*executed)[0].error->operation == "erase");
    client.close();
}

GLIFI_TEST("C++ client batch erase of absent key is failed rejected") {
    // client-semantics §3/§5: execute_batch absent ERASE → failed + rejected.
    RunningServer server;
    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);
    const std::array requests{
        glifistore::client::PipelineRequest{.opcode = glifistore::client::PipelineOpcode::erase,
                                            .key = bytes("missing-key")},
    };
    const auto executed = client.execute_batch(requests);
    GLIFI_REQUIRE(executed.has_value());
    GLIFI_REQUIRE(executed->size() == 1);
    GLIFI_REQUIRE((*executed)[0].outcome == glifistore::client::PipelineOutcome::failed);
    GLIFI_REQUIRE((*executed)[0].error.has_value());
    GLIFI_REQUIRE((*executed)[0].error->code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE((*executed)[0].error->category == "not_found");
    GLIFI_REQUIRE((*executed)[0].error->mutation_outcome == "rejected");
    GLIFI_REQUIRE((*executed)[0].error->retryability == "new_attempt");
    GLIFI_REQUIRE((*executed)[0].error->operation == "erase");
    client.close();
}

GLIFI_TEST("C++ client worker pipelines erase of absent key is failed rejected") {
    // client-semantics §3/§5: pre-sharded worker pipelines inherit pipeline ERASE mapping.
    RunningServer server{1};
    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);
    using Batch = std::vector<glifistore::client::PipelineRequest>;
    const std::vector<Batch> batches{
        Batch{
            {.opcode = glifistore::client::PipelineOpcode::erase, .key = bytes("missing-key")},
        },
    };
    const auto executed = client.execute_worker_pipelines(batches);
    GLIFI_REQUIRE(executed.has_value());
    GLIFI_REQUIRE(executed->size() == 1);
    GLIFI_REQUIRE((*executed)[0].size() == 1);
    GLIFI_REQUIRE((*executed)[0][0].outcome == glifistore::client::PipelineOutcome::failed);
    GLIFI_REQUIRE((*executed)[0][0].error.has_value());
    GLIFI_REQUIRE((*executed)[0][0].error->code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE((*executed)[0][0].error->category == "not_found");
    GLIFI_REQUIRE((*executed)[0][0].error->mutation_outcome == "rejected");
    GLIFI_REQUIRE((*executed)[0][0].error->retryability == "new_attempt");
    GLIFI_REQUIRE((*executed)[0][0].error->operation == "erase");
    client.close();
}

GLIFI_TEST("C++ client maps OVERLOADED mutations to rejected with retryability never") {
    OverloadedPutServer server;
    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);
    const auto put = client.put("k", "v");
    GLIFI_REQUIRE(put.outcome == glifistore::client::MutationOutcome::rejected);
    GLIFI_REQUIRE(put.error.has_value());
    GLIFI_REQUIRE(put.error->code == glifistore::ErrorCode::resource_exhausted);
    GLIFI_REQUIRE(put.error->category == "overloaded");
    GLIFI_REQUIRE(put.error->mutation_outcome == "rejected");
    GLIFI_REQUIRE(put.error->retryability == "never");
    GLIFI_REQUIRE(put.error->wire_status.has_value());
    GLIFI_REQUIRE(*put.error->wire_status ==
                  static_cast<std::uint16_t>(glifistore::server::ResponseStatus::overloaded));
    client.close();
}

GLIFI_TEST("C++ client safely shares worker-bound connections between threads") {
    constexpr std::size_t workers = 2;
    RunningServer server{workers};
    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);

    std::array<std::string, workers> keys;
    for (std::size_t worker = 0; worker < workers; ++worker) {
        keys[worker] = key_for_worker(worker, workers);
    }
    std::atomic<bool> failed{};
    std::vector<std::thread> threads;
    for (std::size_t worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker] {
            for (std::size_t iteration = 0; iteration < 64; ++iteration) {
                const auto value = "value-" + std::to_string(worker) + '-' + std::to_string(iteration);
                if (!client.put(keys[worker], value).committed()) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                auto loaded = client.get(keys[worker]);
                if (!loaded || text(*loaded) != value) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    GLIFI_REQUIRE(!failed.load(std::memory_order_relaxed));
}

GLIFI_TEST("C++ client pipeline preserves order and enforces one Worker") {
    constexpr std::size_t workers = 2;
    RunningServer server{workers};
    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);

    const auto key = key_for_worker(1, workers);
    GLIFI_REQUIRE(client.worker_for(key) == 1);
    std::vector<std::string> values;
    std::vector<glifistore::client::PipelineRequest> requests;
    values.reserve(32);
    requests.reserve(64);
    for (std::size_t index = 0; index < 32; ++index) {
        values.push_back("pipeline-value-" + std::to_string(index));
        requests.push_back({.opcode = glifistore::client::PipelineOpcode::put,
                            .key = bytes(key),
                            .value = bytes(values.back())});
        requests.push_back({.opcode = glifistore::client::PipelineOpcode::get, .key = bytes(key)});
    }
    auto executed = client.execute_pipeline(requests);
    GLIFI_REQUIRE(executed.has_value());
    GLIFI_REQUIRE(executed->size() == requests.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        GLIFI_REQUIRE((*executed)[index * 2U].succeeded());
        GLIFI_REQUIRE((*executed)[index * 2U + 1U].succeeded());
        GLIFI_REQUIRE(text((*executed)[index * 2U + 1U].value) == values[index]);
    }

    const auto other_key = key_for_worker(0, workers);
    const std::array mixed{
        glifistore::client::PipelineRequest{.opcode = glifistore::client::PipelineOpcode::get,
                                            .key = bytes(key)},
        glifistore::client::PipelineRequest{.opcode = glifistore::client::PipelineOpcode::get,
                                            .key = bytes(other_key)},
    };
    auto rejected = client.execute_pipeline(mixed);
    GLIFI_REQUIRE(!rejected.has_value());
    GLIFI_REQUIRE(rejected.error().code == glifistore::ErrorCode::invalid_argument);

    const std::array invalid_opcode{
        glifistore::client::PipelineRequest{.opcode = static_cast<glifistore::client::PipelineOpcode>(255),
                                            .key = bytes(key)},
    };
    rejected = client.execute_pipeline(invalid_opcode);
    GLIFI_REQUIRE(!rejected.has_value());
    GLIFI_REQUIRE(rejected.error().code == glifistore::ErrorCode::invalid_argument);
    GLIFI_REQUIRE(client.healthy());
}

GLIFI_TEST("C++ client batch groups Workers and restores caller order") {
    constexpr std::size_t workers = 2;
    RunningServer server{workers};
    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);

    const auto key0 = key_for_worker(0, workers);
    const auto key1 = key_for_worker(1, workers);
    GLIFI_REQUIRE(client.worker_for(key0) == 0);
    GLIFI_REQUIRE(client.worker_for(key1) == 1);

    const std::array requests{
        glifistore::client::PipelineRequest{
            .opcode = glifistore::client::PipelineOpcode::put, .key = bytes(key1), .value = bytes("v1")},
        glifistore::client::PipelineRequest{
            .opcode = glifistore::client::PipelineOpcode::put, .key = bytes(key0), .value = bytes("v0")},
        glifistore::client::PipelineRequest{.opcode = glifistore::client::PipelineOpcode::get,
                                            .key = bytes(key1)},
        glifistore::client::PipelineRequest{.opcode = glifistore::client::PipelineOpcode::get,
                                            .key = bytes(key0)},
    };
    auto executed = client.execute_batch(requests);
    GLIFI_REQUIRE(executed.has_value());
    GLIFI_REQUIRE(executed->size() == requests.size());
    for (const auto& response : *executed) {
        GLIFI_REQUIRE(response.succeeded());
    }
    GLIFI_REQUIRE(text((*executed)[2].value) == "v1");
    GLIFI_REQUIRE(text((*executed)[3].value) == "v0");
    client.close();

    auto limited =
        glifistore::client::Client::connect({.port = server.port(), .maximum_pipeline_requests = 1});
    GLIFI_REQUIRE(limited.has_value());
    auto limited_client = std::move(*limited);
    const std::array oversized{
        glifistore::client::PipelineRequest{.opcode = glifistore::client::PipelineOpcode::get,
                                            .key = bytes(key0)},
        glifistore::client::PipelineRequest{.opcode = glifistore::client::PipelineOpcode::get,
                                            .key = bytes(key0)},
    };
    auto rejected = limited_client.execute_batch(oversized);
    GLIFI_REQUIRE(!rejected.has_value());
    GLIFI_REQUIRE(rejected.error().code == glifistore::ErrorCode::resource_exhausted);
}

GLIFI_TEST("C++ client execute_worker_pipelines fans out pre-sharded Worker vectors") {
    constexpr std::size_t workers = 2;
    RunningServer server{workers};
    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);

    const auto key0 = key_for_worker(0, workers);
    const auto key1 = key_for_worker(1, workers);
    using Batch = std::vector<glifistore::client::PipelineRequest>;
    const std::vector<Batch> batches{
        Batch{
            {.opcode = glifistore::client::PipelineOpcode::put, .key = bytes(key0), .value = bytes("a")},
            {.opcode = glifistore::client::PipelineOpcode::get, .key = bytes(key0)},
        },
        Batch{
            {.opcode = glifistore::client::PipelineOpcode::put, .key = bytes(key1), .value = bytes("b")},
            {.opcode = glifistore::client::PipelineOpcode::get, .key = bytes(key1)},
        },
    };
    auto executed = client.execute_worker_pipelines(batches);
    GLIFI_REQUIRE(executed.has_value());
    GLIFI_REQUIRE(executed->size() == workers);
    GLIFI_REQUIRE((*executed)[0].size() == 2);
    GLIFI_REQUIRE((*executed)[1].size() == 2);
    GLIFI_REQUIRE((*executed)[0][0].succeeded());
    GLIFI_REQUIRE((*executed)[0][1].succeeded());
    GLIFI_REQUIRE(text((*executed)[0][1].value) == "a");
    GLIFI_REQUIRE((*executed)[1][0].succeeded());
    GLIFI_REQUIRE((*executed)[1][1].succeeded());
    GLIFI_REQUIRE(text((*executed)[1][1].value) == "b");

    const std::vector<Batch> empty_slots{
        Batch{},
        Batch{
            {.opcode = glifistore::client::PipelineOpcode::get, .key = bytes(key1)},
        },
    };
    auto sparse = client.execute_worker_pipelines(empty_slots);
    GLIFI_REQUIRE(sparse.has_value());
    GLIFI_REQUIRE((*sparse)[0].empty());
    GLIFI_REQUIRE((*sparse)[1].size() == 1);
    GLIFI_REQUIRE((*sparse)[1][0].succeeded());

    const std::vector<Batch> misrouted{
        Batch{
            {.opcode = glifistore::client::PipelineOpcode::get, .key = bytes(key1)},
        },
        Batch{},
    };
    auto rejected = client.execute_worker_pipelines(misrouted);
    GLIFI_REQUIRE(!rejected.has_value());
    GLIFI_REQUIRE(rejected.error().code == glifistore::ErrorCode::invalid_argument);

    const std::vector<Batch> wrong_length{
        Batch{
            {.opcode = glifistore::client::PipelineOpcode::get, .key = bytes(key0)},
        },
    };
    rejected = client.execute_worker_pipelines(wrong_length);
    GLIFI_REQUIRE(!rejected.has_value());
    GLIFI_REQUIRE(rejected.error().code == glifistore::ErrorCode::invalid_argument);
    client.close();
}

GLIFI_TEST("C++ client pipeline preserves indeterminate mutation outcomes after disconnect") {
    DisconnectingPipelineServer server;
    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);

    const std::array requests{
        glifistore::client::PipelineRequest{
            .opcode = glifistore::client::PipelineOpcode::put, .key = bytes("key"), .value = bytes("value")},
        glifistore::client::PipelineRequest{.opcode = glifistore::client::PipelineOpcode::get,
                                            .key = bytes("key")},
        glifistore::client::PipelineRequest{.opcode = glifistore::client::PipelineOpcode::erase,
                                            .key = bytes("key")},
    };
    auto executed = client.execute_pipeline(requests);
    GLIFI_REQUIRE(executed.has_value());
    GLIFI_REQUIRE(executed->size() == requests.size());
    GLIFI_REQUIRE((*executed)[0].outcome == glifistore::client::PipelineOutcome::indeterminate);
    GLIFI_REQUIRE((*executed)[1].outcome == glifistore::client::PipelineOutcome::failed);
    GLIFI_REQUIRE((*executed)[2].outcome == glifistore::client::PipelineOutcome::indeterminate);
    GLIFI_REQUIRE((*executed)[0].error.has_value());
    GLIFI_REQUIRE((*executed)[1].error.has_value());
    GLIFI_REQUIRE((*executed)[2].error.has_value());
    GLIFI_REQUIRE((*executed)[0].error->bytes_sent > 0);
    GLIFI_REQUIRE((*executed)[0].error->retryability == "reconcile_first");
    GLIFI_REQUIRE((*executed)[0].error->mutation_outcome == "indeterminate");
    GLIFI_REQUIRE((*executed)[2].error->retryability == "reconcile_first");
    GLIFI_REQUIRE((*executed)[2].error->mutation_outcome == "indeterminate");
    GLIFI_REQUIRE(client.healthy());
}

GLIFI_TEST("C++ client does not blind-retry BACKUP after request bytes were sent") {
    // Online BACKUP requires a pristine destination. Retrying after a lost OK would
    // hit "destination not empty" and falsely report failure of a completed backup.
    BackupDropResponseServer server;
    auto connected = glifistore::client::Client::connect({.port = server.port(), .request_timeout_ms = 200});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);
    auto backed = client.backup("/tmp/glifistore-backup-drop-litmus");
    GLIFI_REQUIRE(!backed.has_value());
    GLIFI_REQUIRE(backed.error().bytes_sent > 0);
    GLIFI_REQUIRE(backed.error().mutation_outcome == "indeterminate");
    GLIFI_REQUIRE(backed.error().retryability == "reconcile_first");
    GLIFI_REQUIRE(server.backup_requests() == 1);
}

GLIFI_TEST("C++ client treats BACKUP INTERNAL_ERROR as reconcile_first") {
    // Wire INTERNAL_ERROR after a possible committed fenced copy must not advertise
    // new_attempt (same-destination retry would look like the first backup failed).
    BackupInternalErrorServer server;
    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);
    auto backed = client.backup("/tmp/glifistore-backup-internal-error-litmus");
    GLIFI_REQUIRE(!backed.has_value());
    GLIFI_REQUIRE(backed.error().mutation_outcome == "indeterminate");
    GLIFI_REQUIRE(backed.error().retryability == "reconcile_first");
    GLIFI_REQUIRE(server.backup_requests() == 1);
}

GLIFI_TEST("C++ client treats BACKUP validate failure as reconcile_first") {
    // A framed BACKUP response with a mismatched request_id still means the fenced
    // copy may already exist — same polarity as INTERNAL_ERROR / mutate validate-fail.
    BackupWrongRequestIdServer server;
    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);
    auto backed = client.backup("/tmp/glifistore-backup-wrong-id-litmus");
    GLIFI_REQUIRE(!backed.has_value());
    GLIFI_REQUIRE(backed.error().bytes_sent > 0);
    GLIFI_REQUIRE(backed.error().mutation_outcome == "indeterminate");
    GLIFI_REQUIRE(backed.error().retryability == "reconcile_first");
    GLIFI_REQUIRE(server.backup_requests() == 1);
}

GLIFI_TEST("C++ client rejects non-positive request timeout override") {
    RunningServer server;
    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);
    auto rejected = client.get("key", {.timeout = std::chrono::milliseconds{0}});
    GLIFI_REQUIRE(!rejected.has_value());
    GLIFI_REQUIRE(rejected.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("C++ client rejects invalid configuration before network I/O") {
    auto invalid = glifistore::client::Client::connect({.port = 0});
    GLIFI_REQUIRE(!invalid.has_value());
    GLIFI_REQUIRE(invalid.error().code == glifistore::ErrorCode::invalid_argument);

    invalid = glifistore::client::Client::connect({.maximum_pipeline_requests = 0});
    GLIFI_REQUIRE(!invalid.has_value());
    GLIFI_REQUIRE(invalid.error().code == glifistore::ErrorCode::invalid_argument);

    invalid = glifistore::client::Client::connect({.maximum_pipeline_bytes = 1});
    GLIFI_REQUIRE(!invalid.has_value());
    GLIFI_REQUIRE(invalid.error().code == glifistore::ErrorCode::invalid_argument);

    invalid = glifistore::client::Client::connect(
        {.unix_socket_path = "/tmp/glifistore-client-uds-tls-refuse.sock", .tls = {.enable = true}});
    GLIFI_REQUIRE(!invalid.has_value());
    GLIFI_REQUIRE(invalid.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("C++ client exposes HEALTH READY STATS and routing state") {
    RunningServer server;
    auto connected = glifistore::client::Client::connect({.port = server.port()});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);
    GLIFI_REQUIRE(client.routing().algorithm == glifistore::RoutingAlgorithm::fnv1a64_v1 ||
                  client.routing().algorithm == glifistore::RoutingAlgorithm::siphash24_v1);
    auto health = client.health();
    GLIFI_REQUIRE(health.has_value());
    GLIFI_REQUIRE(text(*health) == "GlifiStore/live");
    auto ready = client.ready();
    GLIFI_REQUIRE(ready.has_value());
    GLIFI_REQUIRE(text(*ready) == "GlifiStore/ready");
    auto stats = client.stats();
    GLIFI_REQUIRE(stats.has_value());
    GLIFI_REQUIRE(!stats->empty());
    GLIFI_REQUIRE(text(*stats).starts_with("GlifiStore/stats"));
}

GLIFI_TEST("authz-enabled server emits wire permission_denied status 8") {
    glifistore::server::AuthzPolicy policy;
    policy.bind("mapped.example", glifistore::server::Capability::read);
    auto created = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 16, .worker_count = 1, .authz = std::move(policy)});
    GLIFI_REQUIRE(created.has_value());
    GLIFI_REQUIRE((*created)->start().has_value());

    auto connected = glifistore::client::Client::connect({.port = (*created)->port()});
    GLIFI_REQUIRE(connected.has_value());
    auto client = std::move(*connected);

    // Lifecycle opcodes remain open; data-plane opcodes default-deny unmapped peers.
    const auto denied = client.get("key");
    GLIFI_REQUIRE(!denied.has_value());
    GLIFI_REQUIRE(denied.error().category == "permission_denied");
    GLIFI_REQUIRE(denied.error().retryability == "never");
    GLIFI_REQUIRE(denied.error().wire_status.has_value());
    GLIFI_REQUIRE(*denied.error().wire_status ==
                  static_cast<std::uint16_t>(glifistore::server::ResponseStatus::permission_denied));

    const auto put = client.put("key", "value");
    GLIFI_REQUIRE(!put.committed());
    GLIFI_REQUIRE(put.error.has_value());
    GLIFI_REQUIRE(put.error->category == "permission_denied");
    GLIFI_REQUIRE(put.error->wire_status.has_value());
    GLIFI_REQUIRE(*put.error->wire_status ==
                  static_cast<std::uint16_t>(glifistore::server::ResponseStatus::permission_denied));

    client.close();
    (*created)->request_stop();
    static_cast<void>((*created)->join());
}
