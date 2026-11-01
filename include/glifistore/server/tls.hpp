#pragma once

#include "glifistore/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace glifistore::server {

// Secure-profile TLS settings (ADR 0020). Empty paths mean "not configured".
// When any TLS path is set, certificate_file and private_key_file are both
// required. A non-empty client_ca_file enables mTLS (ADR 0021 hook).
// Optional crl_file enables fail-closed CRL checking for mTLS peers.
// ocsp_fail_closed requires crl_file (live AIA OCSP HTTP is unsupported).
struct TlsConfig {
    std::filesystem::path certificate_file{};
    std::filesystem::path private_key_file{};
    std::filesystem::path client_ca_file{};
    std::filesystem::path crl_file{};
    bool ocsp_fail_closed{};
    // Handshake budget after accept; zero uses the library default (10s).
    std::uint32_t handshake_timeout_ms{10'000};

    [[nodiscard]] auto requested() const noexcept -> bool {
        return !certificate_file.empty() || !private_key_file.empty() || !client_ca_file.empty() ||
               !crl_file.empty() || ocsp_fail_closed;
    }
    [[nodiscard]] auto mtls_enabled() const noexcept -> bool {
        return !client_ca_file.empty();
    }
    [[nodiscard]] auto crl_enabled() const noexcept -> bool {
        return !crl_file.empty();
    }
};

[[nodiscard]] auto validate_tls_config(const TlsConfig& config) -> Status;

// Client-side TLS connect settings (ADR 0020). Empty paths mean "not configured".
// When enable is true, hostname/SNI verification is on unless insecure_skip_verify
// (lab escape only). Optional certificate_file+private_key_file enable mTLS.
struct ClientTlsConfig {
    bool enable{false};
    std::filesystem::path ca_file{};
    std::filesystem::path certificate_file{};
    std::filesystem::path private_key_file{};
    // SNI / hostname verification name. Empty uses the TCP peer host string.
    std::string server_name{};
    bool insecure_skip_verify{false};
    std::uint32_t handshake_timeout_ms{10'000};

    [[nodiscard]] auto requested() const noexcept -> bool {
        return enable;
    }
};

[[nodiscard]] auto validate_client_tls_config(const ClientTlsConfig& config) -> Status;

[[nodiscard]] constexpr auto tls_build_enabled() noexcept -> bool {
#if defined(GLIFISTORE_HAS_TLS) && GLIFISTORE_HAS_TLS
    return true;
#else
    return false;
#endif
}

[[nodiscard]] auto tls_backend_name() -> std::string;

enum class TlsIoKind {
    ok,
    // Deprecated umbrella: treat as needing both directions (safe for older callers).
    would_block,
    want_read,
    want_write,
    closed,
};

struct TlsIoResult {
    TlsIoKind kind{TlsIoKind::ok};
    std::size_t bytes{};
};

[[nodiscard]] inline auto tls_io_blocked(const TlsIoKind kind) noexcept -> bool {
    return kind == TlsIoKind::would_block || kind == TlsIoKind::want_read || kind == TlsIoKind::want_write;
}

[[nodiscard]] inline auto tls_io_needs_read(const TlsIoKind kind) noexcept -> bool {
    return kind == TlsIoKind::want_read || kind == TlsIoKind::would_block;
}

class TlsSession;

// Process-wide TLS context: certificates, protocol policy (TLS 1.3+), optional client CA.
class TlsContext final {
  public:
    [[nodiscard]] static auto create(const TlsConfig& config) -> Result<std::shared_ptr<TlsContext>>;
    [[nodiscard]] static auto create_client(const ClientTlsConfig& config)
        -> Result<std::shared_ptr<TlsContext>>;
    ~TlsContext();

    TlsContext(const TlsContext&) = delete;
    auto operator=(const TlsContext&) -> TlsContext& = delete;
    TlsContext(TlsContext&&) = delete;
    auto operator=(TlsContext&&) -> TlsContext& = delete;

    // Performs a server handshake on an already-accepted, non-blocking socket.
    // On success the session owns the TLS state; the caller retains SocketHandle ownership.
    [[nodiscard]] auto accept_socket(int descriptor) const -> Result<std::unique_ptr<TlsSession>>;
    // Allocate TLS state without waiting for peer progress. The Reactor drives
    // TlsSession::accept_handshake from its poller and owns the deadline/slot.
    [[nodiscard]] auto begin_accept_socket(int descriptor) const -> Result<std::unique_ptr<TlsSession>>;

    // Performs a client handshake on an already-connected, non-blocking socket.
    // server_name is used for SNI and hostname verification (unless insecure).
    [[nodiscard]] auto connect_socket(int descriptor, std::string_view server_name) const
        -> Result<std::unique_ptr<TlsSession>>;

    [[nodiscard]] auto mtls_enabled() const noexcept -> bool {
        return mtls_enabled_;
    }
    [[nodiscard]] auto crl_enabled() const noexcept -> bool {
        return crl_enabled_;
    }
    [[nodiscard]] auto ocsp_fail_closed() const noexcept -> bool {
        return ocsp_fail_closed_;
    }
    [[nodiscard]] auto client_mode() const noexcept -> bool {
        return client_mode_;
    }

  private:
    TlsContext();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool mtls_enabled_{};
    bool crl_enabled_{};
    bool ocsp_fail_closed_{};
    bool client_mode_{};
    // Backend-only state also exists in TLS-off builds of this public type.
    [[maybe_unused]] bool insecure_skip_verify_{};
    [[maybe_unused]] std::uint32_t handshake_timeout_ms_{10'000};
};

class TlsSession final {
  public:
    TlsSession() = default;
    ~TlsSession();

    TlsSession(const TlsSession&) = delete;
    auto operator=(const TlsSession&) -> TlsSession& = delete;
    TlsSession(TlsSession&& other) noexcept;
    auto operator=(TlsSession&& other) noexcept -> TlsSession&;

    [[nodiscard]] auto valid() const noexcept -> bool;
    // Stable mTLS principal (URI SAN → DNS SAN → CN). Empty when mTLS was not
    // required or the peer certificate had no usable identity.
    [[nodiscard]] auto peer_principal() const noexcept -> std::string_view;
    // Bytes already decrypted and waiting in OpenSSL (SSL_pending). Used to decide
    // post-accept drains when the poller will not see a fresh readable edge.
    [[nodiscard]] auto pending() const noexcept -> std::size_t;
    [[nodiscard]] auto accept_handshake() -> Result<TlsIoResult>;
    [[nodiscard]] auto read(std::byte* data, std::size_t size) -> Result<TlsIoResult>;
    [[nodiscard]] auto write(const std::byte* data, std::size_t size) -> Result<TlsIoResult>;

  private:
    friend class TlsContext;
    struct Impl;
    explicit TlsSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace glifistore::server
