#include "../integration/server_reactor_test_support.hpp"
#include "glifistore/client/client.hpp"
#include "glifistore/core/fault_injection.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/server/tls.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(GLIFISTORE_HAS_TLS) && GLIFISTORE_HAS_TLS
#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

#if defined(GLIFISTORE_HAS_TLS) && GLIFISTORE_HAS_TLS

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-tls-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        path_ = created;
    }
    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

auto write_self_signed_material(const std::filesystem::path& directory) -> bool {
    const auto key = directory / "server.key";
    const auto cert = directory / "server.crt";
    const auto command = std::string{"openssl req -x509 -newkey rsa:2048 -nodes -keyout '"} + key.string() +
                         "' -out '" + cert.string() + "' -days 1 -subj '/CN=localhost' >/dev/null 2>&1";
    return std::system(command.c_str()) == 0 && std::filesystem::is_regular_file(key) &&
           std::filesystem::is_regular_file(cert);
}

auto send_all_ssl(SSL* ssl, const std::span<const std::byte> data) -> bool {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto written = SSL_write(ssl, data.data() + static_cast<std::ptrdiff_t>(sent),
                                       static_cast<int>(data.size() - sent));
        if (written <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

auto receive_exact_ssl(SSL* ssl, const std::span<std::byte> output) -> bool {
    std::size_t received = 0;
    while (received < output.size()) {
        const auto count = SSL_read(ssl, output.data() + static_cast<std::ptrdiff_t>(received),
                                    static_cast<int>(output.size() - received));
        if (count <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(count);
    }
    return true;
}

auto receive_response_ssl(SSL* ssl) -> std::vector<std::byte> {
    std::array<std::byte, glifistore::server::kResponseHeaderBytes> header{};
    if (!receive_exact_ssl(ssl, header)) {
        return {};
    }
    std::uint32_t frame_size = 0;
    for (std::size_t byte = 0; byte < 4; ++byte) {
        frame_size |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[byte])) << (byte * 8U);
    }
    if (frame_size < header.size() || frame_size > glifistore::server::kMaxFrameBytes) {
        return {};
    }
    std::vector<std::byte> frame(frame_size);
    std::copy(header.begin(), header.end(), frame.begin());
    if (!receive_exact_ssl(ssl, std::span<std::byte>{frame}.subspan(header.size()))) {
        return {};
    }
    return frame;
}

#endif

} // namespace

GLIFI_TEST("tls config validation fails closed without cert and key") {
    glifistore::server::TlsConfig incomplete{.certificate_file = "cert.pem"};
    const auto missing_key = glifistore::server::validate_tls_config(incomplete);
    GLIFI_REQUIRE(!missing_key.has_value());

    glifistore::server::TlsConfig ca_only{.client_ca_file = "ca.pem"};
    const auto missing_pair = glifistore::server::validate_tls_config(ca_only);
    GLIFI_REQUIRE(!missing_pair.has_value());

    GLIFI_REQUIRE(glifistore::server::validate_tls_config({}).has_value());
}

GLIFI_TEST("dual listen rejects identical cleartext and tls ports") {
    glifistore::server::ReactorConfig config{
        .port = 7379,
        .tls_port = 7379,
        .tls =
            {
                .certificate_file = "/tmp/glifistore-missing-cert.pem",
                .private_key_file = "/tmp/glifistore-missing-key.pem",
            },
    };
    const auto server = glifistore::server::Server::create(config);
    GLIFI_REQUIRE(!server.has_value());
    GLIFI_REQUIRE(server.error().message.find("must differ") != std::string::npos);
}

GLIFI_TEST("tls_port without TLS certificate configuration fails closed") {
    glifistore::server::ReactorConfig config{
        .port = 0,
        .tls_port = 0,
    };
    const auto server = glifistore::server::Server::create(config);
    GLIFI_REQUIRE(!server.has_value());
    GLIFI_REQUIRE(server.error().message.find("tls_port requires") != std::string::npos);
}

GLIFI_TEST("tls build reports backend availability") {
    const auto backend = glifistore::server::tls_backend_name();
    GLIFI_REQUIRE(!backend.empty());
    if (glifistore::server::tls_build_enabled()) {
        GLIFI_REQUIRE(backend != "disabled");
#if defined(__OpenBSD__)
        // ADR 0020 / Phase 2.6: OpenBSD secure-profile builds must use system LibreSSL.
        GLIFI_REQUIRE(backend == "LibreSSL");
#endif
    } else {
        GLIFI_REQUIRE(backend == "disabled");
        glifistore::server::TlsConfig requested{
            .certificate_file = "/tmp/missing-cert.pem",
            .private_key_file = "/tmp/missing-key.pem",
        };
        const auto status = glifistore::server::validate_tls_config(requested);
        GLIFI_REQUIRE(!status.has_value());
        GLIFI_REQUIRE(status.error().message.find("without TLS") != std::string::npos);
    }
}

#if defined(GLIFISTORE_HAS_TLS) && GLIFISTORE_HAS_TLS

GLIFI_TEST("tls server accepts handshake and serves protocol ping") {
    TemporaryDirectory directory;
    if (!write_self_signed_material(directory.path())) {
        // OpenSSL CLI unavailable in the environment; skip without failing the suite.
        return;
    }

    glifistore::server::ReactorConfig config{
        .port = 0,
        .worker_count = 1,
        .tls =
            {
                .certificate_file = directory.path() / "server.crt",
                .private_key_file = directory.path() / "server.key",
            },
    };
    auto server = glifistore::server::Server::create(config);
    GLIFI_REQUIRE(server.has_value());
    GLIFI_REQUIRE((*server)->start().has_value());
    const auto port = (*server)->port();
    GLIFI_REQUIRE(port != 0);

    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x10100000L
    auto* ctx = SSL_CTX_new(TLS_client_method());
#else
    auto* ctx = SSL_CTX_new(SSLv23_client_method());
#endif
    GLIFI_REQUIRE(ctx != nullptr);
#if defined(TLS1_3_VERSION)
    GLIFI_REQUIRE(SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) == 1);
#endif
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    const auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
    GLIFI_REQUIRE(fd >= 0);
    timeval timeout{.tv_sec = 2, .tv_usec = 0};
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr));
    GLIFI_REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0);

    auto* ssl = SSL_new(ctx);
    GLIFI_REQUIRE(ssl != nullptr);
    GLIFI_REQUIRE(SSL_set_fd(ssl, fd) == 1);
    GLIFI_REQUIRE(SSL_connect(ssl) == 1);

    const auto ping = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 42,
    });
    GLIFI_REQUIRE(ping.has_value());
    GLIFI_REQUIRE(send_all_ssl(ssl, *ping));
    const auto frame = receive_response_ssl(ssl);
    const auto decoded = glifistore::server::decode_response(frame);
    GLIFI_REQUIRE(decoded.has_value());
    GLIFI_REQUIRE(decoded->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(decoded->frame.request_id == 42);

    SSL_free(ssl);
    static_cast<void>(::close(fd));
    SSL_CTX_free(ctx);
    (*server)->request_stop();
    GLIFI_REQUIRE((*server)->join().has_value());
}

GLIFI_TEST("tls post-accept drain serves coalesced INIT without a second client kick") {
    // After SSL_accept, INIT bytes may already sit in OpenSSL with no fresh ET
    // readable edge. adopt_connection must SSL_read once or bootstrap hangs.
    TemporaryDirectory directory;
    if (!write_self_signed_material(directory.path())) {
        return;
    }

    glifistore::server::ReactorConfig config{
        .port = 0,
        .worker_count = 1,
        .tls =
            {
                .certificate_file = directory.path() / "server.crt",
                .private_key_file = directory.path() / "server.key",
            },
    };
    auto server = glifistore::server::Server::create(config);
    GLIFI_REQUIRE(server.has_value());
    GLIFI_REQUIRE((*server)->start().has_value());
    const auto port = (*server)->port();
    GLIFI_REQUIRE(port != 0);

    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x10100000L
    auto* ctx = SSL_CTX_new(TLS_client_method());
#else
    auto* ctx = SSL_CTX_new(SSLv23_client_method());
#endif
    GLIFI_REQUIRE(ctx != nullptr);
#if defined(TLS1_3_VERSION)
    GLIFI_REQUIRE(SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) == 1);
#endif
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    const auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
    GLIFI_REQUIRE(fd >= 0);
    timeval timeout{.tv_sec = 2, .tv_usec = 0};
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr));
    GLIFI_REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0);

    auto* ssl = SSL_new(ctx);
    GLIFI_REQUIRE(ssl != nullptr);
    GLIFI_REQUIRE(SSL_set_fd(ssl, fd) == 1);

    const auto init = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::init,
        .request_id = 1,
    });
    GLIFI_REQUIRE(init.has_value());

    // Drive handshake then immediately push INIT (and often the same flight as
    // the client Finished under TLS 1.3). Do not write a second kick afterward.
    GLIFI_REQUIRE(SSL_connect(ssl) == 1);
    GLIFI_REQUIRE(send_all_ssl(ssl, *init));

    const auto frame = receive_response_ssl(ssl);
    const auto decoded = glifistore::server::decode_response(frame);
    GLIFI_REQUIRE(decoded.has_value());
    GLIFI_REQUIRE(decoded->frame.request_id == 1);
    GLIFI_REQUIRE(decoded->frame.status == glifistore::server::ResponseStatus::ok);

    SSL_free(ssl);
    static_cast<void>(::close(fd));
    SSL_CTX_free(ctx);
    (*server)->request_stop();
    GLIFI_REQUIRE((*server)->join().has_value());
}

GLIFI_TEST("tls WANT_READ during response flush still delivers PUT OK") {
    // Injected SSL_write WANT_READ must not leave a committed ACK stranded on an
    // edge-triggered poller (especially if write-only interest was armed).
    TemporaryDirectory directory;
    if (!write_self_signed_material(directory.path())) {
        return;
    }

    glifistore::server::ReactorConfig config{
        .port = 0,
        .worker_count = 1,
        .tls =
            {
                .certificate_file = directory.path() / "server.crt",
                .private_key_file = directory.path() / "server.key",
            },
    };
    auto server = glifistore::server::Server::create(config);
    GLIFI_REQUIRE(server.has_value());
    GLIFI_REQUIRE((*server)->start().has_value());
    const auto port = (*server)->port();
    GLIFI_REQUIRE(port != 0);

    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x10100000L
    auto* ctx = SSL_CTX_new(TLS_client_method());
#else
    auto* ctx = SSL_CTX_new(SSLv23_client_method());
#endif
    GLIFI_REQUIRE(ctx != nullptr);
#if defined(TLS1_3_VERSION)
    GLIFI_REQUIRE(SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) == 1);
#endif
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    const auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
    GLIFI_REQUIRE(fd >= 0);
    timeval timeout{.tv_sec = 2, .tv_usec = 0};
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr));
    GLIFI_REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0);

    auto* ssl = SSL_new(ctx);
    GLIFI_REQUIRE(ssl != nullptr);
    GLIFI_REQUIRE(SSL_set_fd(ssl, fd) == 1);
    GLIFI_REQUIRE(SSL_connect(ssl) == 1);

    const auto init = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::init,
        .request_id = 1,
    });
    GLIFI_REQUIRE(init.has_value());
    GLIFI_REQUIRE(send_all_ssl(ssl, *init));
    const auto init_frame = receive_response_ssl(ssl);
    const auto init_decoded = glifistore::server::decode_response(init_frame);
    GLIFI_REQUIRE(init_decoded.has_value());
    GLIFI_REQUIRE(init_decoded->frame.status == glifistore::server::ResponseStatus::ok);

    const auto bind = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::bind_worker,
        .request_id = 2,
        .target_worker = 0,
    });
    GLIFI_REQUIRE(bind.has_value());
    GLIFI_REQUIRE(send_all_ssl(ssl, *bind));
    const auto bind_frame = receive_response_ssl(ssl);
    const auto bind_decoded = glifistore::server::decode_response(bind_frame);
    GLIFI_REQUIRE(bind_decoded.has_value());
    GLIFI_REQUIRE(bind_decoded->frame.status == glifistore::server::ResponseStatus::ok);

    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::tls_write_want_read);
    const std::string_view key = "tls-want-read-key";
    const std::string_view value = "tls-want-read-value";
    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 3,
        .key = {reinterpret_cast<const std::byte*>(key.data()), key.size()},
        .value = {reinterpret_cast<const std::byte*>(value.data()), value.size()},
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(send_all_ssl(ssl, *put));

    const auto put_frame = receive_response_ssl(ssl);
    glifistore::fault::reset();
    const auto put_decoded = glifistore::server::decode_response(put_frame);
    GLIFI_REQUIRE(put_decoded.has_value());
    GLIFI_REQUIRE(put_decoded->frame.request_id == 3);
    GLIFI_REQUIRE(put_decoded->frame.status == glifistore::server::ResponseStatus::ok);

    // Half-close after ACK: confirms the connection survived WANT_READ without
    // write-only arming that would strand a later flush.
    GLIFI_REQUIRE(::shutdown(fd, SHUT_WR) == 0);

    SSL_free(ssl);
    static_cast<void>(::close(fd));
    SSL_CTX_free(ctx);
    (*server)->request_stop();
    GLIFI_REQUIRE((*server)->join().has_value());
}

GLIFI_TEST("tls WANT_WRITE during response flush still delivers PUT OK") {
    // Injected SSL_write WANT_WRITE must not leave a committed ACK stranded on an
    // edge-triggered poller (especially if write interest was not re-armed).
    TemporaryDirectory directory;
    if (!write_self_signed_material(directory.path())) {
        return;
    }

    glifistore::server::ReactorConfig config{
        .port = 0,
        .worker_count = 1,
        .tls =
            {
                .certificate_file = directory.path() / "server.crt",
                .private_key_file = directory.path() / "server.key",
            },
    };
    auto server = glifistore::server::Server::create(config);
    GLIFI_REQUIRE(server.has_value());
    GLIFI_REQUIRE((*server)->start().has_value());
    const auto port = (*server)->port();
    GLIFI_REQUIRE(port != 0);

    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x10100000L
    auto* ctx = SSL_CTX_new(TLS_client_method());
#else
    auto* ctx = SSL_CTX_new(SSLv23_client_method());
#endif
    GLIFI_REQUIRE(ctx != nullptr);
#if defined(TLS1_3_VERSION)
    GLIFI_REQUIRE(SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) == 1);
#endif
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    const auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
    GLIFI_REQUIRE(fd >= 0);
    timeval timeout{.tv_sec = 2, .tv_usec = 0};
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr));
    GLIFI_REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0);

    auto* ssl = SSL_new(ctx);
    GLIFI_REQUIRE(ssl != nullptr);
    GLIFI_REQUIRE(SSL_set_fd(ssl, fd) == 1);
    GLIFI_REQUIRE(SSL_connect(ssl) == 1);

    const auto init = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::init,
        .request_id = 1,
    });
    GLIFI_REQUIRE(init.has_value());
    GLIFI_REQUIRE(send_all_ssl(ssl, *init));
    const auto init_frame = receive_response_ssl(ssl);
    const auto init_decoded = glifistore::server::decode_response(init_frame);
    GLIFI_REQUIRE(init_decoded.has_value());
    GLIFI_REQUIRE(init_decoded->frame.status == glifistore::server::ResponseStatus::ok);

    const auto bind = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::bind_worker,
        .request_id = 2,
        .target_worker = 0,
    });
    GLIFI_REQUIRE(bind.has_value());
    GLIFI_REQUIRE(send_all_ssl(ssl, *bind));
    const auto bind_frame = receive_response_ssl(ssl);
    const auto bind_decoded = glifistore::server::decode_response(bind_frame);
    GLIFI_REQUIRE(bind_decoded.has_value());
    GLIFI_REQUIRE(bind_decoded->frame.status == glifistore::server::ResponseStatus::ok);

    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::tls_write_want_write);
    const std::string_view key = "tls-want-write-key";
    const std::string_view value = "tls-want-write-value";
    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 3,
        .key = {reinterpret_cast<const std::byte*>(key.data()), key.size()},
        .value = {reinterpret_cast<const std::byte*>(value.data()), value.size()},
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(send_all_ssl(ssl, *put));

    const auto put_frame = receive_response_ssl(ssl);
    glifistore::fault::reset();
    const auto put_decoded = glifistore::server::decode_response(put_frame);
    GLIFI_REQUIRE(put_decoded.has_value());
    GLIFI_REQUIRE(put_decoded->frame.request_id == 3);
    GLIFI_REQUIRE(put_decoded->frame.status == glifistore::server::ResponseStatus::ok);

    // Half-close after ACK: confirms the connection survived WANT_WRITE without
    // stranding a later flush.
    GLIFI_REQUIRE(::shutdown(fd, SHUT_WR) == 0);

    SSL_free(ssl);
    static_cast<void>(::close(fd));
    SSL_CTX_free(ctx);
    (*server)->request_stop();
    GLIFI_REQUIRE((*server)->join().has_value());
}

GLIFI_TEST("tls server create rejects missing certificate files") {
    glifistore::server::ReactorConfig config{
        .port = 0,
        .tls =
            {
                .certificate_file = "/tmp/glifistore-missing-cert.pem",
                .private_key_file = "/tmp/glifistore-missing-key.pem",
            },
    };
    const auto server = glifistore::server::Server::create(config);
    GLIFI_REQUIRE(!server.has_value());
}

GLIFI_TEST("dual cleartext and TLS listeners serve protocol independently") {
    TemporaryDirectory directory;
    if (!write_self_signed_material(directory.path())) {
        return;
    }

    glifistore::server::ReactorConfig config{
        .port = 0,
        .worker_count = 1,
        .tls_port = 0,
        .tls =
            {
                .certificate_file = directory.path() / "server.crt",
                .private_key_file = directory.path() / "server.key",
            },
    };
    auto server = glifistore::server::Server::create(config);
    GLIFI_REQUIRE(server.has_value());
    GLIFI_REQUIRE((*server)->start().has_value());
    const auto cleartext_port = (*server)->cleartext_port();
    const auto tls_port = (*server)->tls_port();
    GLIFI_REQUIRE(cleartext_port != 0);
    GLIFI_REQUIRE(tls_port != 0);
    GLIFI_REQUIRE(cleartext_port != tls_port);
    GLIFI_REQUIRE((*server)->port() == cleartext_port);

    // Cleartext PING on --port.
    {
        const auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
        GLIFI_REQUIRE(fd >= 0);
        timeval timeout{.tv_sec = 2, .tv_usec = 0};
        static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(cleartext_port);
        static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr));
        GLIFI_REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0);

        const auto ping = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::ping,
            .request_id = 7,
        });
        GLIFI_REQUIRE(ping.has_value());
        std::size_t sent = 0;
        while (sent < ping->size()) {
            const auto written =
                ::send(fd, ping->data() + static_cast<std::ptrdiff_t>(sent), ping->size() - sent, 0);
            GLIFI_REQUIRE(written > 0);
            sent += static_cast<std::size_t>(written);
        }
        std::array<std::byte, glifistore::server::kResponseHeaderBytes> header{};
        std::size_t received = 0;
        while (received < header.size()) {
            const auto count = ::recv(fd, header.data() + static_cast<std::ptrdiff_t>(received),
                                      header.size() - received, 0);
            GLIFI_REQUIRE(count > 0);
            received += static_cast<std::size_t>(count);
        }
        std::uint32_t frame_size = 0;
        for (std::size_t byte = 0; byte < 4; ++byte) {
            frame_size |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[byte]))
                          << (byte * 8U);
        }
        std::vector<std::byte> frame(frame_size);
        std::copy(header.begin(), header.end(), frame.begin());
        received = header.size();
        while (received < frame.size()) {
            const auto count =
                ::recv(fd, frame.data() + static_cast<std::ptrdiff_t>(received), frame.size() - received, 0);
            GLIFI_REQUIRE(count > 0);
            received += static_cast<std::size_t>(count);
        }
        const auto decoded = glifistore::server::decode_response(frame);
        GLIFI_REQUIRE(decoded.has_value());
        GLIFI_REQUIRE(decoded->frame.status == glifistore::server::ResponseStatus::ok);
        GLIFI_REQUIRE(decoded->frame.request_id == 7);
        static_cast<void>(::close(fd));
    }

    // TLS PING on --tls-port (must not accept cleartext protocol bytes as TLS).
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x10100000L
    auto* ctx = SSL_CTX_new(TLS_client_method());
#else
    auto* ctx = SSL_CTX_new(SSLv23_client_method());
#endif
    GLIFI_REQUIRE(ctx != nullptr);
#if defined(TLS1_3_VERSION)
    GLIFI_REQUIRE(SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) == 1);
#endif
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    const auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
    GLIFI_REQUIRE(fd >= 0);
    timeval timeout{.tv_sec = 2, .tv_usec = 0};
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(tls_port);
    static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr));
    GLIFI_REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0);

    auto* ssl = SSL_new(ctx);
    GLIFI_REQUIRE(ssl != nullptr);
    GLIFI_REQUIRE(SSL_set_fd(ssl, fd) == 1);
    GLIFI_REQUIRE(SSL_connect(ssl) == 1);

    const auto ping = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 8,
    });
    GLIFI_REQUIRE(ping.has_value());
    GLIFI_REQUIRE(send_all_ssl(ssl, *ping));
    const auto frame = receive_response_ssl(ssl);
    const auto decoded = glifistore::server::decode_response(frame);
    GLIFI_REQUIRE(decoded.has_value());
    GLIFI_REQUIRE(decoded->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(decoded->frame.request_id == 8);

    SSL_free(ssl);
    static_cast<void>(::close(fd));
    SSL_CTX_free(ctx);
    (*server)->request_stop();
    GLIFI_REQUIRE((*server)->join().has_value());
}

GLIFI_TEST("tls CRL config fails closed without mTLS and rejects empty CRL") {
    TemporaryDirectory directory;
    if (!write_self_signed_material(directory.path())) {
        return;
    }
    const auto crl_path = directory.path() / "clients.crl";
    {
        std::ofstream out{crl_path};
        out << "not-a-pem-crl\n";
    }

    glifistore::server::TlsConfig missing_ca{
        .certificate_file = directory.path() / "server.crt",
        .private_key_file = directory.path() / "server.key",
        .crl_file = crl_path,
    };
    const auto need_ca = glifistore::server::validate_tls_config(missing_ca);
    GLIFI_REQUIRE(!need_ca.has_value());
    GLIFI_REQUIRE(need_ca.error().message.find("tls-client-ca") != std::string::npos);

    glifistore::server::TlsConfig ocsp_only{
        .certificate_file = directory.path() / "server.crt",
        .private_key_file = directory.path() / "server.key",
        .client_ca_file = directory.path() / "server.crt",
        .ocsp_fail_closed = true,
    };
    const auto need_crl = glifistore::server::validate_tls_config(ocsp_only);
    GLIFI_REQUIRE(!need_crl.has_value());
    GLIFI_REQUIRE(need_crl.error().message.find("tls-crl") != std::string::npos);

    glifistore::server::TlsConfig with_bad_crl{
        .certificate_file = directory.path() / "server.crt",
        .private_key_file = directory.path() / "server.key",
        .client_ca_file = directory.path() / "server.crt",
        .crl_file = crl_path,
        .ocsp_fail_closed = true,
    };
    const auto valid_paths = glifistore::server::validate_tls_config(with_bad_crl);
    GLIFI_REQUIRE(valid_paths.has_value());
    const auto created = glifistore::server::TlsContext::create(with_bad_crl);
    GLIFI_REQUIRE(!created.has_value());
    GLIFI_REQUIRE(created.error().message.find("CRL") != std::string::npos);
}

GLIFI_TEST("tls CRL rejects revoked mTLS client certificate") {
    TemporaryDirectory directory;
    const auto root = directory.path();
    const auto ca_key = root / "ca.key";
    const auto ca_crt = root / "ca.crt";
    const auto server_key = root / "server.key";
    const auto server_crt = root / "server.crt";
    const auto client_key = root / "client.key";
    const auto client_csr = root / "client.csr";
    const auto client_crt = root / "client.crt";
    const auto crl_path = root / "clients.crl";
    const auto index = root / "index.txt";
    const auto serial = root / "serial";
    const auto crlnumber = root / "crlnumber";
    const auto openssl_cnf = root / "openssl.cnf";

    {
        std::ofstream cfg{openssl_cnf};
        cfg << "[ ca ]\n"
               "default_ca = CA_default\n"
               "[ CA_default ]\n"
               "dir = "
            << root.string()
            << "\n"
               "database = $dir/index.txt\n"
               "new_certs_dir = $dir\n"
               "certificate = $dir/ca.crt\n"
               "serial = $dir/serial\n"
               "crlnumber = $dir/crlnumber\n"
               "private_key = $dir/ca.key\n"
               "default_md = sha256\n"
               "default_days = 1\n"
               "default_crl_days = 1\n"
               "policy = policy_any\n"
               "[ policy_any ]\n"
               "commonName = supplied\n";
    }
    {
        std::ofstream out{index};
    }
    {
        std::ofstream out{serial};
        out << "1000\n";
    }
    {
        std::ofstream out{crlnumber};
        out << "1000\n";
    }

    const auto run = [](const std::string& command) -> bool { return std::system(command.c_str()) == 0; };
    if (!run("openssl req -x509 -newkey rsa:2048 -nodes -keyout '" + ca_key.string() + "' -out '" +
             ca_crt.string() + "' -days 1 -subj '/CN=glifistore-test-ca' >/dev/null 2>&1")) {
        return;
    }
    if (!run("openssl req -newkey rsa:2048 -nodes -keyout '" + server_key.string() + "' -out '" +
             (root / "server.csr").string() + "' -subj '/CN=localhost' >/dev/null 2>&1")) {
        return;
    }
    if (!run("openssl x509 -req -in '" + (root / "server.csr").string() + "' -CA '" + ca_crt.string() +
             "' -CAkey '" + ca_key.string() + "' -CAcreateserial -out '" + server_crt.string() +
             "' -days 1 >/dev/null 2>&1")) {
        return;
    }
    if (!run("openssl req -newkey rsa:2048 -nodes -keyout '" + client_key.string() + "' -out '" +
             client_csr.string() + "' -subj '/CN=revoked.example' >/dev/null 2>&1")) {
        return;
    }
    if (!run("openssl ca -batch -config '" + openssl_cnf.string() + "' -in '" + client_csr.string() +
             "' -out '" + client_crt.string() + "' -keyfile '" + ca_key.string() + "' -cert '" +
             ca_crt.string() + "' >/dev/null 2>&1")) {
        return;
    }
    if (!run("openssl ca -batch -config '" + openssl_cnf.string() + "' -revoke '" + client_crt.string() +
             "' -keyfile '" + ca_key.string() + "' -cert '" + ca_crt.string() + "' >/dev/null 2>&1")) {
        return;
    }
    if (!run("openssl ca -batch -config '" + openssl_cnf.string() + "' -gencrl -out '" + crl_path.string() +
             "' -keyfile '" + ca_key.string() + "' -cert '" + ca_crt.string() + "' >/dev/null 2>&1")) {
        return;
    }

    glifistore::server::ReactorConfig config{
        .port = 0,
        .worker_count = 1,
        .tls =
            {
                .certificate_file = server_crt,
                .private_key_file = server_key,
                .client_ca_file = ca_crt,
                .crl_file = crl_path,
                .ocsp_fail_closed = true,
            },
        .security_audit_events = true,
    };
    auto server = glifistore::server::Server::create(config);
    GLIFI_REQUIRE(server.has_value());
    GLIFI_REQUIRE((*server)->start().has_value());
    const auto port = (*server)->port();
    GLIFI_REQUIRE(port != 0);

    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x10100000L
    auto* ctx = SSL_CTX_new(TLS_client_method());
#else
    auto* ctx = SSL_CTX_new(SSLv23_client_method());
#endif
    GLIFI_REQUIRE(ctx != nullptr);
#if defined(TLS1_3_VERSION)
    GLIFI_REQUIRE(SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) == 1);
#endif
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    GLIFI_REQUIRE(SSL_CTX_use_certificate_file(ctx, client_crt.string().c_str(), SSL_FILETYPE_PEM) == 1);
    GLIFI_REQUIRE(SSL_CTX_use_PrivateKey_file(ctx, client_key.string().c_str(), SSL_FILETYPE_PEM) == 1);

    const auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
    GLIFI_REQUIRE(fd >= 0);
    timeval timeout{.tv_sec = 2, .tv_usec = 0};
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr));
    GLIFI_REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0);

    auto* ssl = SSL_new(ctx);
    GLIFI_REQUIRE(ssl != nullptr);
    GLIFI_REQUIRE(SSL_set_fd(ssl, fd) == 1);
    const auto connected = SSL_connect(ssl);
    // Server fail-closes revoked peers; the client handshake may fail or the
    // first read/write may fail depending on TLS stack timing.
    if (connected == 1) {
        const auto ping = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::ping,
            .request_id = 99,
        });
        GLIFI_REQUIRE(ping.has_value());
        const auto sent = send_all_ssl(ssl, *ping);
        if (sent) {
            const auto frame = receive_response_ssl(ssl);
            GLIFI_REQUIRE(frame.empty());
        }
    }

    SSL_free(ssl);
    static_cast<void>(::close(fd));
    SSL_CTX_free(ctx);

    // Give the acceptor a moment to record the auth deny.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    auto stats = (*server)->stats_report();
    GLIFI_REQUIRE(stats.has_value());
    GLIFI_REQUIRE(stats->find("tls_crl=1") != std::string::npos);
    GLIFI_REQUIRE(stats->find("tls_ocsp_fail_closed=1") != std::string::npos);
    GLIFI_REQUIRE(stats->find("auth_denies=") != std::string::npos);

    (*server)->request_stop();
    GLIFI_REQUIRE((*server)->join().has_value());
}

GLIFI_TEST("tls stalled handshake leaves established sessions responsive") {
    TemporaryDirectory directory;
    GLIFI_REQUIRE(write_self_signed_material(directory.path()));
    auto server = glifistore::server::Server::create({
        .port = 0,
        .maximum_connections = 8,
        .tls = {.certificate_file = directory.path() / "server.crt",
                .private_key_file = directory.path() / "server.key",
                .handshake_timeout_ms = 1'500},
    });
    GLIFI_REQUIRE(server.has_value());
    GLIFI_REQUIRE((*server)->start().has_value());
    auto client = glifistore::client::Client::connect({
        .port = (*server)->port(),
        .request_timeout_ms = 500,
        .tls = {.enable = true,
                .ca_file = (directory.path() / "server.crt").string(),
                .server_name = "localhost"},
    });
    GLIFI_REQUIRE(client.has_value());
    glifistore::server::SocketHandle stalled{
        glifistore::test::server_reactor_support::connect_to((*server)->port())};
    GLIFI_REQUIRE(stalled.valid());
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    const auto ping = client->ping();
    GLIFI_REQUIRE(ping.has_value());
    stalled.reset();
    (*server)->request_stop();
    GLIFI_REQUIRE((*server)->join().has_value());
}

GLIFI_TEST("tls pending handshake has bounded slots deadline and shutdown") {
    TemporaryDirectory directory;
    GLIFI_REQUIRE(write_self_signed_material(directory.path()));
    auto server = glifistore::server::Server::create({
        .port = 0,
        .maximum_connections = 1,
        .tls = {.certificate_file = directory.path() / "server.crt",
                .private_key_file = directory.path() / "server.key",
                .handshake_timeout_ms = 250},
    });
    GLIFI_REQUIRE(server.has_value());
    GLIFI_REQUIRE((*server)->start().has_value());
    const auto connect_raw = [&] {
        return glifistore::server::SocketHandle{
            glifistore::test::server_reactor_support::connect_to((*server)->port())};
    };
    auto stalled = connect_raw();
    GLIFI_REQUIRE(stalled.valid());
    const auto adopted_by = std::chrono::steady_clock::now() + std::chrono::milliseconds{150};
    while ((*server)->active_connections_per_executor()[0] == 0 &&
           std::chrono::steady_clock::now() < adopted_by) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    GLIFI_REQUIRE((*server)->active_connections_per_executor()[0] == 1);
    auto excess = connect_raw();
    GLIFI_REQUIRE(excess.valid());
    char byte{};
    GLIFI_REQUIRE(::recv(excess.descriptor(), &byte, 1, 0) == 0);
    const auto expired_by = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while ((*server)->active_connections_per_executor()[0] != 0 &&
           std::chrono::steady_clock::now() < expired_by) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE((*server)->active_connections_per_executor()[0] == 0);
    auto replacement = connect_raw();
    GLIFI_REQUIRE(replacement.valid());
    (*server)->request_stop();
    GLIFI_REQUIRE((*server)->join().has_value());
}

#endif
