#include "glifistore/client/client.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/server/tls.hpp"
#include "test.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

#if defined(GLIFISTORE_HAS_TLS) && GLIFISTORE_HAS_TLS

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-client-tls-XXXXXX").string();
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

#endif

} // namespace

GLIFI_TEST("client tls options fail closed when TLS is unavailable or incomplete") {
    glifistore::client::ClientConfig incomplete{
        .host = "127.0.0.1",
        .tls =
            {
                .enable = true,
                .cert_file = "client.crt",
            },
    };
    const auto opened = glifistore::client::Client::connect(incomplete);
    GLIFI_REQUIRE(!opened.has_value());
}

#if defined(GLIFISTORE_HAS_TLS) && GLIFISTORE_HAS_TLS

GLIFI_TEST("client connect over TLS can ping") {
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

    auto client = glifistore::client::Client::connect({
        .host = "127.0.0.1",
        .port = port,
        .tls =
            {
                .enable = true,
                .ca_file = (directory.path() / "server.crt").string(),
                .server_name = "localhost",
            },
    });
    GLIFI_REQUIRE(client.has_value());
    const auto payload = std::string_view{"tls-ping"};
    const auto echoed = client->ping({reinterpret_cast<const std::byte*>(payload.data()), payload.size()});
    GLIFI_REQUIRE(echoed.has_value());
    GLIFI_REQUIRE(echoed->size() == payload.size());
    GLIFI_REQUIRE(std::string_view(reinterpret_cast<const char*>(echoed->data()), echoed->size()) == payload);
    client->close();
    (*server)->request_stop();
}

#else

GLIFI_TEST("client TLS request reports build without TLS support") {
    glifistore::client::ClientConfig requested{
        .host = "127.0.0.1",
        .port = 1,
        .tls = {.enable = true},
    };
    const auto opened = glifistore::client::Client::connect(requested);
    GLIFI_REQUIRE(!opened.has_value());
    GLIFI_REQUIRE(opened.error().message.find("without TLS") != std::string::npos);
}

#endif
