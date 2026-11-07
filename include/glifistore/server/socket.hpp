/**
 * @file
 * @brief Move-only socket/listener ownership and nonblocking transport setup.
 */
#pragma once

#include "glifistore/core/error.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace glifistore::server {

/**
 * @brief Move-only descriptor owner. release() transfers responsibility; descriptor() only borrows it.
 */
class SocketHandle final {
  public:
    SocketHandle() = default;

    explicit SocketHandle(int descriptor) noexcept : descriptor_(descriptor) {}

    ~SocketHandle();

    SocketHandle(const SocketHandle&) = delete;
    auto operator=(const SocketHandle&) -> SocketHandle& = delete;
    SocketHandle(SocketHandle&& other) noexcept;
    auto operator=(SocketHandle&& other) noexcept -> SocketHandle&;

    [[nodiscard]] auto descriptor() const noexcept -> int {
        return descriptor_;
    }

    [[nodiscard]] auto valid() const noexcept -> bool {
        return descriptor_ >= 0;
    }

    /**
     * @brief Detach the descriptor without closing it; the caller becomes its sole owner.
     */
    [[nodiscard]] auto release() noexcept -> int;
    /**
     * @brief Close the current descriptor and adopt the replacement, or become empty for -1.
     */
    void reset(int descriptor = -1) noexcept;

  private:
    int descriptor_{-1};
};

/**
 * @brief Owning nonblocking TCP listener; accepted sockets transfer ownership to the caller.
 */
class TcpListener final {
  public:
    TcpListener() = default;

    [[nodiscard]] static auto bind(std::string_view address, std::uint16_t port, int backlog = 512,
                                   bool reuse_port = false) -> Result<TcpListener>;
    /// @brief Transfer an accepted socket; successful nullopt means no connection is ready.
    [[nodiscard]] auto accept() const -> Result<std::optional<SocketHandle>>;

    [[nodiscard]] auto descriptor() const noexcept -> int {
        return socket_.descriptor();
    }

    [[nodiscard]] auto port() const noexcept -> std::uint16_t {
        return port_;
    }

  private:
    TcpListener(SocketHandle socket, std::uint16_t port) noexcept : socket_(std::move(socket)), port_(port) {}

    SocketHandle socket_;
    std::uint16_t port_{};
};

/// AF_UNIX stream listener (Phase 8 / ADR 0029). One acceptor per process; path is
/// unlinked on bind (stale) and again when the owning listener is destroyed.
class UnixListener final {
  public:
    UnixListener() = default;
    ~UnixListener();

    UnixListener(const UnixListener&) = delete;
    auto operator=(const UnixListener&) -> UnixListener& = delete;
    UnixListener(UnixListener&& other) noexcept;
    auto operator=(UnixListener&& other) noexcept -> UnixListener&;

    [[nodiscard]] static auto bind(const std::filesystem::path& path, int backlog = 512)
        -> Result<UnixListener>;
    /// @brief Transfer an accepted socket; successful nullopt means no connection is ready.
    [[nodiscard]] auto accept() const -> Result<std::optional<SocketHandle>>;

    [[nodiscard]] auto descriptor() const noexcept -> int {
        return socket_.descriptor();
    }

    [[nodiscard]] auto path() const noexcept -> const std::filesystem::path& {
        return path_;
    }

  private:
    UnixListener(SocketHandle socket, std::filesystem::path path) noexcept
        : socket_(std::move(socket)), path_(std::move(path)) {}

    void release_path() noexcept;

    SocketHandle socket_;
    std::filesystem::path path_{};
};

[[nodiscard]] auto set_nonblocking(int descriptor) -> Status;
[[nodiscard]] auto set_close_on_exec(int descriptor) -> Status;

} // namespace glifistore::server
