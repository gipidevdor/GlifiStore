#include "glifistore/server/peercred.hpp"
#include "glifistore/server/socket.hpp"
#include "test.hpp"

#include <cstring>
#include <filesystem>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

class SocketTemporaryDirectory final {
  public:
    SocketTemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-peercred-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        path_ = created;
    }

    ~SocketTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

} // namespace

GLIFI_TEST("peercred principal uses unix:uid= prefix") {
    const glifistore::server::PeerCredentials credentials{.uid = 1000, .gid = 100, .pid = 42};
    GLIFI_REQUIRE(glifistore::server::peercred_principal(credentials) == "unix:uid=1000");
    GLIFI_REQUIRE(glifistore::server::peercred_principal_prefix() == "unix:uid=");
}

GLIFI_TEST("peercred supported matches known Unix platforms") {
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    GLIFI_REQUIRE(glifistore::server::peercred_supported());
#else
    GLIFI_REQUIRE(!glifistore::server::peercred_supported());
#endif
}

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
GLIFI_TEST("unix listener accept yields peer credentials for local connector") {
    SocketTemporaryDirectory temporary;
    const auto socket_path = temporary.path() / "glifistore.sock";
    auto listener = glifistore::server::UnixListener::bind(socket_path);
    GLIFI_REQUIRE(listener.has_value());

    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    GLIFI_REQUIRE(client >= 0);
    sockaddr_un endpoint{};
    endpoint.sun_family = AF_UNIX;
    const auto path_text = socket_path.string();
    GLIFI_REQUIRE(path_text.size() < sizeof(endpoint.sun_path));
    std::memcpy(endpoint.sun_path, path_text.c_str(), path_text.size() + 1U);
    GLIFI_REQUIRE(::connect(client, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0);

    auto accepted = listener->accept();
    GLIFI_REQUIRE(accepted.has_value());
    GLIFI_REQUIRE(accepted->has_value());
    auto credentials = glifistore::server::peer_credentials((**accepted).descriptor());
    GLIFI_REQUIRE(credentials.has_value());
    GLIFI_REQUIRE(credentials->uid == static_cast<std::uint32_t>(::geteuid()));
    GLIFI_REQUIRE(credentials->gid == static_cast<std::uint32_t>(::getegid()));
    const auto principal = glifistore::server::peercred_principal(*credentials);
    GLIFI_REQUIRE(principal ==
                  std::string{glifistore::server::peercred_principal_prefix()} + std::to_string(::geteuid()));
#if defined(__linux__)
    GLIFI_REQUIRE(credentials->pid == static_cast<std::uint32_t>(::getpid()));
#endif
    static_cast<void>(::close(client));
}
#endif
