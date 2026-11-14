#include "glifistore/server/socket.hpp"

#include "system_error.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace glifistore::server {
namespace {

[[nodiscard]] auto configure_stream_socket(const int descriptor, const bool tcp) -> Status {
    if (auto nonblocking = set_nonblocking(descriptor); !nonblocking) {
        return nonblocking;
    }
    if (auto close_on_exec = set_close_on_exec(descriptor); !close_on_exec) {
        return close_on_exec;
    }
    const int enabled = 1;
    if (tcp && ::setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0) {
        return system_error("setsockopt(TCP_NODELAY)");
    }
#if defined(SO_NOSIGPIPE)
    if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        return system_error("setsockopt(SO_NOSIGPIPE)");
    }
#endif
    return {};
}

[[nodiscard]] auto configure_client_socket(const int descriptor) -> Status {
    return configure_stream_socket(descriptor, true);
}

[[nodiscard]] auto configure_unix_client_socket(const int descriptor) -> Status {
    return configure_stream_socket(descriptor, false);
}

auto unlink_socket_path(const std::filesystem::path& path) noexcept -> void {
    if (path.empty()) {
        return;
    }
    static_cast<void>(::unlink(path.c_str()));
}

} // namespace

SocketHandle::~SocketHandle() {
    reset();
}

SocketHandle::SocketHandle(SocketHandle&& other) noexcept : descriptor_(other.release()) {}

auto SocketHandle::operator=(SocketHandle&& other) noexcept -> SocketHandle& {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

auto SocketHandle::release() noexcept -> int {
    const auto descriptor = descriptor_;
    descriptor_ = -1;
    return descriptor;
}

void SocketHandle::reset(const int descriptor) noexcept {
    if (descriptor_ >= 0) {
        static_cast<void>(::close(descriptor_));
    }
    descriptor_ = descriptor;
}

auto set_nonblocking(const int descriptor) -> Status {
    const auto flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
        return system_error("fcntl(O_NONBLOCK)");
    }
    return {};
}

auto set_close_on_exec(const int descriptor) -> Status {
    const auto flags = ::fcntl(descriptor, F_GETFD, 0);
    if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
        return system_error("fcntl(FD_CLOEXEC)");
    }
    return {};
}

auto TcpListener::bind(const std::string_view address, const std::uint16_t port, const int backlog,
                       const bool reuse_port) -> Result<TcpListener> {
    SocketHandle socket{::socket(AF_INET, SOCK_STREAM, 0)};
    if (!socket.valid()) {
        return system_error("socket");
    }
    if (auto nonblocking = set_nonblocking(socket.descriptor()); !nonblocking) {
        return unexpected(nonblocking.error());
    }
    if (auto close_on_exec = set_close_on_exec(socket.descriptor()); !close_on_exec) {
        return unexpected(close_on_exec.error());
    }
    const int enabled = 1;
    if (::setsockopt(socket.descriptor(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
        return system_error("setsockopt(SO_REUSEADDR)");
    }
    if (reuse_port &&
        ::setsockopt(socket.descriptor(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
        return system_error("setsockopt(SO_REUSEPORT)");
    }

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    const std::string address_text{address};
    if (::inet_pton(AF_INET, address_text.c_str(), &endpoint.sin_addr) != 1) {
        return fail(ErrorCode::invalid_argument, "TCP bind address must be an IPv4 literal");
    }
    if (::bind(socket.descriptor(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
        return system_error("bind");
    }
    if (::listen(socket.descriptor(), backlog) != 0) {
        return system_error("listen");
    }

    sockaddr_in bound{};
    socklen_t bound_size = sizeof(bound);
    if (::getsockname(socket.descriptor(), reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
        return system_error("getsockname");
    }
    return TcpListener{std::move(socket), ntohs(bound.sin_port)};
}

auto TcpListener::accept() const -> Result<std::optional<SocketHandle>> {
    sockaddr_in remote{};
    socklen_t remote_size = sizeof(remote);
#if defined(__linux__)
    const auto accepted_descriptor = ::accept4(descriptor(), reinterpret_cast<sockaddr*>(&remote),
                                               &remote_size, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    const auto accepted_descriptor =
        ::accept(descriptor(), reinterpret_cast<sockaddr*>(&remote), &remote_size);
#endif
    if (accepted_descriptor < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::optional<SocketHandle>{};
        }
        if (errno == EINTR) {
            return std::optional<SocketHandle>{};
        }
        return system_error("accept");
    }
    SocketHandle client{accepted_descriptor};
    if (auto configured = configure_client_socket(client.descriptor()); !configured) {
        return unexpected(configured.error());
    }
    return std::optional<SocketHandle>{std::move(client)};
}

UnixListener::~UnixListener() {
    release_path();
}

UnixListener::UnixListener(UnixListener&& other) noexcept
    : socket_(std::move(other.socket_)), path_(std::move(other.path_)) {
    other.path_.clear();
}

auto UnixListener::operator=(UnixListener&& other) noexcept -> UnixListener& {
    if (this != &other) {
        release_path();
        socket_ = std::move(other.socket_);
        path_ = std::move(other.path_);
        other.path_.clear();
    }
    return *this;
}

void UnixListener::release_path() noexcept {
    if (socket_.valid() && !path_.empty()) {
        unlink_socket_path(path_);
    }
    path_.clear();
}

auto UnixListener::bind(const std::filesystem::path& path, const int backlog) -> Result<UnixListener> {
    if (path.empty()) {
        return fail(ErrorCode::invalid_argument, "Unix socket path must not be empty");
    }
    const auto path_text = path.string();
    if (path_text.size() >= sizeof(sockaddr_un{}.sun_path)) {
        return fail(ErrorCode::invalid_argument, "Unix socket path exceeds sockaddr_un capacity");
    }
    if (path_text.contains('\0')) {
        return fail(ErrorCode::invalid_argument, "Unix socket path must not contain NUL");
    }

    SocketHandle socket{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!socket.valid()) {
        return system_error("socket(AF_UNIX)");
    }
    if (auto nonblocking = set_nonblocking(socket.descriptor()); !nonblocking) {
        return unexpected(nonblocking.error());
    }
    if (auto close_on_exec = set_close_on_exec(socket.descriptor()); !close_on_exec) {
        return unexpected(close_on_exec.error());
    }

    // Remove a stale filesystem socket left by a previous process; ignore missing.
    unlink_socket_path(path);

    sockaddr_un endpoint{};
    endpoint.sun_family = AF_UNIX;
    std::memcpy(endpoint.sun_path, path_text.c_str(), path_text.size() + 1U);
    const auto bind_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path_text.size() + 1U);
    if (::bind(socket.descriptor(), reinterpret_cast<const sockaddr*>(&endpoint), bind_length) != 0) {
        return system_error("bind(AF_UNIX)");
    }
    // Owner-only access; peercred is the authn source when enabled.
    if (::chmod(path_text.c_str(), S_IRUSR | S_IWUSR) != 0) {
        const auto error = system_error("chmod(AF_UNIX)");
        unlink_socket_path(path);
        return error;
    }
    if (::listen(socket.descriptor(), backlog) != 0) {
        const auto error = system_error("listen(AF_UNIX)");
        unlink_socket_path(path);
        return error;
    }
    return UnixListener{std::move(socket), path};
}

auto UnixListener::accept() const -> Result<std::optional<SocketHandle>> {
    sockaddr_un remote{};
    socklen_t remote_size = sizeof(remote);
#if defined(__linux__)
    const auto accepted_descriptor = ::accept4(descriptor(), reinterpret_cast<sockaddr*>(&remote),
                                               &remote_size, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    const auto accepted_descriptor =
        ::accept(descriptor(), reinterpret_cast<sockaddr*>(&remote), &remote_size);
#endif
    if (accepted_descriptor < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return std::optional<SocketHandle>{};
        }
        return system_error("accept(AF_UNIX)");
    }
    SocketHandle client{accepted_descriptor};
    if (auto configured = configure_unix_client_socket(client.descriptor()); !configured) {
        return unexpected(configured.error());
    }
    return std::optional<SocketHandle>{std::move(client)};
}

} // namespace glifistore::server
