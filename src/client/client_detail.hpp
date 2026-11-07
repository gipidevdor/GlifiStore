#pragma once

#include "glifistore/client/client.hpp"
#include "glifistore/core/little_endian.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/tls.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <poll.h>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace glifistore::client::detail {

using Clock = std::chrono::steady_clock;

class Socket final {
  public:
    Socket() = default;

    explicit Socket(const int descriptor) noexcept : descriptor_(descriptor) {}

    ~Socket() {
        reset();
    }

    Socket(Socket&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}

    auto operator=(Socket&& other) noexcept -> Socket& {
        if (this != &other) {
            reset();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    Socket(const Socket&) = delete;
    auto operator=(const Socket&) -> Socket& = delete;

    [[nodiscard]] auto get() const noexcept -> int {
        return descriptor_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return descriptor_ >= 0;
    }

    void reset() noexcept {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
            descriptor_ = -1;
        }
    }

  private:
    int descriptor_{-1};
};

struct OwnedResponse {
    server::ResponseStatus status{server::ResponseStatus::internal_error};
    std::uint64_t request_id{};
    std::uint32_t owner_worker{server::kNoWorker};
    std::uint32_t worker_count{};
    std::uint64_t routing_epoch{};
    std::vector<std::byte> value;
};

struct ExchangeFailure {
    Error error;
    std::size_t request_bytes_sent{};
};

using ExchangeResult = std::variant<OwnedResponse, ExchangeFailure>;

[[nodiscard]] inline auto error_message(const int error_number) -> std::string {
    return std::error_code{error_number, std::generic_category()}.message();
}

[[nodiscard]] inline auto system_error(const ErrorCode code, const std::string_view operation) -> Error {
    const auto error_number = errno;
    return {code, std::string{operation} + ": " + error_message(error_number)};
}

[[nodiscard]] inline auto wait_for(const int descriptor, const short events, const Clock::time_point deadline)
    -> Status {
    for (;;) {
        const auto now = Clock::now();
        if (now >= deadline) {
            return fail(ErrorCode::unavailable, "TCP operation timed out");
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto bounded_timeout =
            std::clamp<std::int64_t>(remaining.count(), 1, std::numeric_limits<int>::max());
        const auto timeout = static_cast<int>(bounded_timeout);
        pollfd event{.fd = descriptor, .events = events, .revents = 0};
        const auto result = ::poll(&event, 1, timeout);
        if (result > 0) {
            if ((event.revents & events) != 0) {
                return {};
            }
            return fail(ErrorCode::io_error, "TCP connection closed while waiting for I/O");
        }
        if (result == 0) {
            return fail(ErrorCode::unavailable, "TCP operation timed out");
        }
        if (errno != EINTR) {
            return unexpected(system_error(ErrorCode::io_error, "poll"));
        }
    }
}

[[nodiscard]] inline auto connect_socket(const ClientConfig& config) -> Result<Socket> {
    if (!config.unix_socket_path.empty()) {
        if (config.tls.enable) {
            return fail(ErrorCode::invalid_argument, "TLS is not supported on AF_UNIX clients");
        }
        if (config.unix_socket_path.size() >= sizeof(sockaddr_un{}.sun_path)) {
            return fail(ErrorCode::invalid_argument, "unix socket path exceeds sockaddr_un capacity");
        }
        Socket socket{::socket(AF_UNIX, SOCK_STREAM, 0)};
        if (!socket) {
            return unexpected(system_error(ErrorCode::io_error, "socket"));
        }
        const auto flags = ::fcntl(socket.get(), F_GETFL, 0);
        if (flags < 0 || ::fcntl(socket.get(), F_SETFL, flags | O_NONBLOCK) < 0 ||
            ::fcntl(socket.get(), F_SETFD, FD_CLOEXEC) < 0) {
            return unexpected(system_error(ErrorCode::io_error, "fcntl"));
        }
#if defined(SO_NOSIGPIPE)
        const int enabled = 1;
        static_cast<void>(::setsockopt(socket.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)));
#endif
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, config.unix_socket_path.data(), config.unix_socket_path.size());
        const auto deadline = Clock::now() + std::chrono::milliseconds{config.connect_timeout_ms};
        if (::connect(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            return socket;
        }
        if (errno != EINPROGRESS) {
            return unexpected(system_error(ErrorCode::unavailable, "connect"));
        }
        auto ready = wait_for(socket.get(), POLLOUT, deadline);
        if (!ready) {
            return unexpected(ready.error());
        }
        int socket_error{};
        socklen_t socket_error_size = sizeof(socket_error);
        if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) != 0) {
            return unexpected(system_error(ErrorCode::io_error, "getsockopt"));
        }
        if (socket_error != 0) {
            return fail(ErrorCode::unavailable, std::string{"connect: "} + error_message(socket_error));
        }
        return socket;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses{};
    const auto service = std::to_string(config.port);
    const auto resolved = ::getaddrinfo(config.host.c_str(), service.c_str(), &hints, &addresses);
    if (resolved != 0) {
        return fail(ErrorCode::unavailable,
                    std::string{"cannot resolve server address: "} + ::gai_strerror(resolved));
    }
    const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard{addresses, &::freeaddrinfo};
    Error last_error{ErrorCode::unavailable, "no server address could be connected"};
    const auto deadline = Clock::now() + std::chrono::milliseconds{config.connect_timeout_ms};
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        Socket socket{::socket(address->ai_family, address->ai_socktype, address->ai_protocol)};
        if (!socket) {
            last_error = system_error(ErrorCode::io_error, "socket");
            continue;
        }
        const auto flags = ::fcntl(socket.get(), F_GETFL, 0);
        if (flags < 0 || ::fcntl(socket.get(), F_SETFL, flags | O_NONBLOCK) < 0 ||
            ::fcntl(socket.get(), F_SETFD, FD_CLOEXEC) < 0) {
            last_error = system_error(ErrorCode::io_error, "fcntl");
            continue;
        }
        const int enabled = 1;
        static_cast<void>(::setsockopt(socket.get(), IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)));
#if defined(SO_NOSIGPIPE)
        static_cast<void>(::setsockopt(socket.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)));
#endif
        if (::connect(socket.get(), address->ai_addr, address->ai_addrlen) == 0) {
            return socket;
        }
        if (errno != EINPROGRESS) {
            last_error = system_error(ErrorCode::unavailable, "connect");
            continue;
        }
        auto ready = wait_for(socket.get(), POLLOUT, deadline);
        if (!ready) {
            last_error = ready.error();
            continue;
        }
        int socket_error{};
        socklen_t socket_error_size = sizeof(socket_error);
        if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) != 0) {
            last_error = system_error(ErrorCode::io_error, "getsockopt");
            continue;
        }
        if (socket_error != 0) {
            last_error = {ErrorCode::unavailable, std::string{"connect: "} + error_message(socket_error)};
            continue;
        }
        return socket;
    }
    return unexpected(std::move(last_error));
}

struct WorkerConnection {
    explicit WorkerConnection(const std::uint32_t index) : worker(index) {
        input.reserve(std::size_t{64} * 1024U);
    }

    void reset() noexcept {
        tls.reset();
        socket.reset();
        input.clear();
        input_offset = 0;
    }

    std::uint32_t worker{};
    std::mutex mutex;
    Socket socket;
    std::unique_ptr<server::TlsSession> tls;
    std::vector<std::byte> input;
    std::size_t input_offset{};
};

[[nodiscard]] inline auto send_frame(WorkerConnection& connection, const std::span<const std::byte> frame,
                                     const Clock::time_point deadline)
    -> std::variant<std::size_t, ExchangeFailure> {
    std::size_t sent{};
    while (sent < frame.size()) {
        if (connection.tls) {
            auto written = connection.tls->write(frame.data() + sent, frame.size() - sent);
            if (!written) {
                return ExchangeFailure{written.error(), sent};
            }
            if (written->kind == server::TlsIoKind::would_block ||
                written->kind == server::TlsIoKind::want_read ||
                written->kind == server::TlsIoKind::want_write) {
                auto ready =
                    wait_for(connection.socket.get(), static_cast<short>(POLLIN | POLLOUT), deadline);
                if (ready) {
                    continue;
                }
                return ExchangeFailure{ready.error(), sent};
            }
            if (written->kind == server::TlsIoKind::closed) {
                return ExchangeFailure{{ErrorCode::io_error, "server closed the TLS connection"}, sent};
            }
            sent += written->bytes;
            continue;
        }
#if defined(MSG_NOSIGNAL)
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        const auto count =
            ::send(connection.socket.get(), frame.data() + sent, frame.size() - sent, send_flags);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            auto ready = wait_for(connection.socket.get(), POLLOUT, deadline);
            if (ready) {
                continue;
            }
            return ExchangeFailure{ready.error(), sent};
        }
        return ExchangeFailure{system_error(ErrorCode::io_error, "send"), sent};
    }
    return sent;
}

[[nodiscard]] inline auto receive_exact(WorkerConnection& connection, const std::span<std::byte> output,
                                        const Clock::time_point deadline) -> Status {
    std::size_t received{};
    while (received < output.size()) {
        if (connection.tls) {
            auto read = connection.tls->read(output.data() + received, output.size() - received);
            if (!read) {
                return unexpected(read.error());
            }
            if (read->kind == server::TlsIoKind::would_block || read->kind == server::TlsIoKind::want_read ||
                read->kind == server::TlsIoKind::want_write) {
                auto ready =
                    wait_for(connection.socket.get(), static_cast<short>(POLLIN | POLLOUT), deadline);
                if (ready) {
                    continue;
                }
                return ready;
            }
            if (read->kind == server::TlsIoKind::closed) {
                return fail(ErrorCode::io_error, "server closed the TLS connection");
            }
            received += read->bytes;
            continue;
        }
        const auto count =
            ::recv(connection.socket.get(), output.data() + received, output.size() - received, 0);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            return fail(ErrorCode::io_error, "server closed the TCP connection");
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            auto ready = wait_for(connection.socket.get(), POLLIN, deadline);
            if (ready) {
                continue;
            }
            return ready;
        }
        return unexpected(system_error(ErrorCode::io_error, "receive"));
    }
    return {};
}

[[nodiscard]] inline auto receive_response(WorkerConnection& connection, const ClientConfig& config,
                                           const Clock::time_point deadline,
                                           const std::size_t request_bytes_sent) -> ExchangeResult {
    std::array<std::byte, sizeof(std::uint32_t)> size_bytes{};
    if (auto received = receive_exact(connection, size_bytes, deadline); !received) {
        return ExchangeFailure{received.error(), request_bytes_sent};
    }
    const auto frame_size = static_cast<std::size_t>(le::get_u32(size_bytes, 0));
    if (frame_size < server::kResponseHeaderBytes || frame_size > config.maximum_frame_bytes) {
        return ExchangeFailure{{ErrorCode::corrupted_data, "server response size is outside client limits"},
                               request_bytes_sent};
    }
    std::vector<std::byte> frame(frame_size);
    std::ranges::copy(size_bytes, frame.begin());
    if (auto received =
            receive_exact(connection, std::span<std::byte>{frame}.subspan(size_bytes.size()), deadline);
        !received) {
        return ExchangeFailure{received.error(), request_bytes_sent};
    }
    auto decoded = server::decode_response(frame, config.maximum_frame_bytes);
    if (!decoded || !decoded->complete || decoded->consumed != frame.size()) {
        return ExchangeFailure{{ErrorCode::corrupted_data, "server returned an invalid response frame"},
                               request_bytes_sent};
    }
    return OwnedResponse{.status = decoded->frame.status,
                         .request_id = decoded->frame.request_id,
                         .owner_worker = decoded->frame.owner_worker,
                         .worker_count = decoded->frame.worker_count,
                         .routing_epoch = decoded->frame.routing_epoch,
                         .value = {decoded->frame.value.begin(), decoded->frame.value.end()}};
}

[[nodiscard]] inline auto exchange(WorkerConnection& connection, const std::span<const std::byte> request,
                                   const ClientConfig& config, const Clock::time_point deadline)
    -> ExchangeResult {
    auto sent = send_frame(connection, request, deadline);
    if (const auto* failure = std::get_if<ExchangeFailure>(&sent)) {
        return *failure;
    }
    const auto request_bytes_sent = std::get<std::size_t>(sent);
    return receive_response(connection, config, deadline, request_bytes_sent);
}

[[nodiscard]] inline auto exchange(WorkerConnection& connection, const std::span<const std::byte> request,
                                   const ClientConfig& config) -> ExchangeResult {
    const auto deadline = Clock::now() + std::chrono::milliseconds{config.request_timeout_ms};
    return exchange(connection, request, config, deadline);
}

[[nodiscard]] inline auto category_for(const ErrorCode code) -> std::string {
    switch (code) {
    case ErrorCode::invalid_argument:
    case ErrorCode::record_too_large:
        return "invalid_argument";
    case ErrorCode::not_found:
        return "not_found";
    case ErrorCode::resource_exhausted:
        return "overloaded";
    case ErrorCode::unavailable:
        return "unavailable";
    case ErrorCode::io_error:
        return "transport";
    case ErrorCode::corrupted_data:
        return "protocol";
    case ErrorCode::internal_error:
    default:
        return "internal";
    }
}

[[nodiscard]] auto
enrich_error(Error error, const std::string_view operation, const std::optional<std::uint64_t> request_id,
             const std::optional<std::uint32_t> worker, const std::optional<std::uint64_t> routing_epoch,
             const std::size_t bytes_sent = 0, const std::optional<std::uint16_t> wire_status = std::nullopt,
             const bool mutation = false, const bool indeterminate = false) -> Error {
    if (error.category.empty()) {
        error.category = category_for(error.code);
    }
    error.operation = std::string{operation};
    error.request_id = request_id;
    error.worker = worker;
    error.routing_epoch = routing_epoch;
    error.bytes_sent = bytes_sent;
    if (wire_status.has_value()) {
        error.wire_status = wire_status;
    }
    if (mutation) {
        error.mutation_outcome = indeterminate ? "indeterminate" : "rejected";
        error.retryability = portable_retryability(error.category, bytes_sent > 0, indeterminate);
        if (bytes_sent > 0 && error.category == "transport") {
            error.retryability = "reconcile_first";
        }
    } else {
        error.retryability = portable_retryability(error.category, false, false);
    }
    return error;
}

[[nodiscard]] inline auto response_error(const server::ResponseStatus status) -> Error {
    return error_from_wire_status(static_cast<std::uint16_t>(status));
}

[[nodiscard]] inline auto as_bytes(const std::string_view value) noexcept -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

[[nodiscard]] inline auto is_mutation(const PipelineOpcode opcode) noexcept -> bool {
    return opcode == PipelineOpcode::put || opcode == PipelineOpcode::erase;
}

[[nodiscard]] inline auto operation_name(const server::RequestOpcode opcode) noexcept -> std::string_view {
    switch (opcode) {
    case server::RequestOpcode::init:
        return "init";
    case server::RequestOpcode::ping:
        return "ping";
    case server::RequestOpcode::get:
        return "get";
    case server::RequestOpcode::put:
        return "put";
    case server::RequestOpcode::erase:
        return "erase";
    case server::RequestOpcode::bind_worker:
        return "bind_worker";
    case server::RequestOpcode::health:
        return "health";
    case server::RequestOpcode::ready:
        return "ready";
    case server::RequestOpcode::stats:
        return "stats";
    case server::RequestOpcode::backup:
        return "backup";
    }
    return "unknown";
}

[[nodiscard]] inline auto operation_name(const PipelineOpcode opcode) noexcept -> std::string_view {
    switch (opcode) {
    case PipelineOpcode::get:
        return "get";
    case PipelineOpcode::put:
        return "put";
    case PipelineOpcode::erase:
        return "erase";
    }
    return "unknown";
}

[[nodiscard]] inline auto is_supported(const PipelineOpcode opcode) noexcept -> bool {
    return opcode == PipelineOpcode::get || opcode == PipelineOpcode::put || opcode == PipelineOpcode::erase;
}

[[nodiscard]] inline auto wire_opcode(const PipelineOpcode opcode) noexcept -> server::RequestOpcode {
    switch (opcode) {
    case PipelineOpcode::get:
        return server::RequestOpcode::get;
    case PipelineOpcode::put:
        return server::RequestOpcode::put;
    case PipelineOpcode::erase:
        return server::RequestOpcode::erase;
    }
    return server::RequestOpcode::get;
}

[[nodiscard]] inline auto receive_buffered_response(WorkerConnection& connection, const ClientConfig& config,
                                                    const Clock::time_point deadline,
                                                    const std::size_t request_bytes_sent) -> ExchangeResult {
    for (;;) {
        const auto available = connection.input.size() - connection.input_offset;
        if (available >= sizeof(std::uint32_t)) {
            const std::span<const std::byte> pending{connection.input.data() + connection.input_offset,
                                                     available};
            const auto frame_size = static_cast<std::size_t>(le::get_u32(pending, 0));
            if (frame_size < server::kResponseHeaderBytes || frame_size > config.maximum_frame_bytes) {
                return ExchangeFailure{
                    {ErrorCode::corrupted_data, "server response size is outside client limits"},
                    request_bytes_sent};
            }
            if (available >= frame_size) {
                auto decoded = server::decode_response(pending.first(frame_size), config.maximum_frame_bytes);
                if (!decoded || !decoded->complete || decoded->consumed != frame_size) {
                    return ExchangeFailure{
                        {ErrorCode::corrupted_data, "server returned an invalid response frame"},
                        request_bytes_sent};
                }
                OwnedResponse response{.status = decoded->frame.status,
                                       .request_id = decoded->frame.request_id,
                                       .owner_worker = decoded->frame.owner_worker,
                                       .worker_count = decoded->frame.worker_count,
                                       .routing_epoch = decoded->frame.routing_epoch,
                                       .value = {decoded->frame.value.begin(), decoded->frame.value.end()}};
                connection.input_offset += frame_size;
                if (connection.input_offset == connection.input.size()) {
                    connection.input.clear();
                    connection.input_offset = 0;
                }
                return response;
            }
        }

        if (connection.input_offset > 0) {
            connection.input.erase(connection.input.begin(),
                                   connection.input.begin() +
                                       static_cast<std::ptrdiff_t>(connection.input_offset));
            connection.input_offset = 0;
        }
        std::array<std::byte, std::size_t{64} * 1024U> chunk;
        if (connection.tls) {
            auto read = connection.tls->read(chunk.data(), chunk.size());
            if (!read) {
                return ExchangeFailure{read.error(), request_bytes_sent};
            }
            if (read->kind == server::TlsIoKind::would_block || read->kind == server::TlsIoKind::want_read ||
                read->kind == server::TlsIoKind::want_write) {
                auto ready =
                    wait_for(connection.socket.get(), static_cast<short>(POLLIN | POLLOUT), deadline);
                if (ready) {
                    continue;
                }
                return ExchangeFailure{ready.error(), request_bytes_sent};
            }
            if (read->kind == server::TlsIoKind::closed) {
                return ExchangeFailure{{ErrorCode::io_error, "server closed the TLS connection"},
                                       request_bytes_sent};
            }
            connection.input.insert(connection.input.end(), chunk.begin(),
                                    chunk.begin() + static_cast<std::ptrdiff_t>(read->bytes));
            continue;
        }
        const auto count = ::recv(connection.socket.get(), chunk.data(), chunk.size(), 0);
        if (count > 0) {
            connection.input.insert(connection.input.end(), chunk.begin(),
                                    chunk.begin() + static_cast<std::ptrdiff_t>(count));
            continue;
        }
        if (count == 0) {
            return ExchangeFailure{{ErrorCode::io_error, "server closed the TCP connection"},
                                   request_bytes_sent};
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            auto ready = wait_for(connection.socket.get(), POLLIN, deadline);
            if (ready) {
                continue;
            }
            return ExchangeFailure{ready.error(), request_bytes_sent};
        }
        return ExchangeFailure{system_error(ErrorCode::io_error, "receive"), request_bytes_sent};
    }
}

struct Metadata {
    std::uint32_t worker_count{};
    std::uint64_t routing_epoch{};
    WorkerRoutingState routing{};
};

} // namespace glifistore::client::detail
