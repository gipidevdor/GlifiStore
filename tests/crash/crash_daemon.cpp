#include "glifistore/client/client.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/store/store.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <netinet/in.h>
#include <optional>
#include <signal.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::string_view kAckKey{"daemon-ack-key"};
constexpr std::string_view kAckValue{"daemon-ack-value"};

enum class Checkpoint : std::uint8_t {
    post_ack,
    pre_commit,
    post_ack_erase,
};

struct Options {
    std::filesystem::path daemon{};
    std::string storage{"durable-sync"};
    Checkpoint checkpoint{Checkpoint::post_ack};
};

[[nodiscard]] auto parse_checkpoint(const std::string_view text) -> std::optional<Checkpoint> {
    if (text == "post-ack") {
        return Checkpoint::post_ack;
    }
    if (text == "pre-commit") {
        return Checkpoint::pre_commit;
    }
    if (text == "post-ack-erase") {
        return Checkpoint::post_ack_erase;
    }
    return std::nullopt;
}

[[nodiscard]] auto checkpoint_name(const Checkpoint checkpoint) -> std::string_view {
    switch (checkpoint) {
    case Checkpoint::post_ack:
        return "post-ack";
    case Checkpoint::pre_commit:
        return "pre-commit";
    case Checkpoint::post_ack_erase:
        return "post-ack-erase";
    }
    return "unknown";
}

[[nodiscard]] auto parse_options(int argc, char** argv) -> std::optional<Options> {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg{argv[index]};
        const auto require_value = [&](const char* flag) -> std::optional<std::string_view> {
            if (arg != flag || index + 1 >= argc) {
                return std::nullopt;
            }
            return std::string_view{argv[++index]};
        };
        if (arg == "--daemon") {
            const auto value = require_value("--daemon");
            if (!value) {
                return std::nullopt;
            }
            options.daemon = *value;
        } else if (arg == "--storage") {
            const auto value = require_value("--storage");
            if (!value) {
                return std::nullopt;
            }
            options.storage = std::string{*value};
        } else if (arg == "--checkpoint") {
            const auto value = require_value("--checkpoint");
            if (!value || !parse_checkpoint(*value)) {
                return std::nullopt;
            }
            options.checkpoint = *parse_checkpoint(*value);
        } else if (arg == "--help" || arg == "-h") {
            return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    if (options.daemon.empty()) {
        return std::nullopt;
    }
    if (options.storage != "durable-sync" && options.storage != "durable-group" &&
        options.storage != "durable-periodic") {
        return std::nullopt;
    }
    return options;
}

void print_usage(const char* program) {
    std::cerr << "usage: " << program
              << " --daemon PATH [--storage durable-sync|durable-group|durable-periodic]\n"
              << "       [--checkpoint post-ack|pre-commit|post-ack-erase]\n";
}

[[nodiscard]] auto run_suffix() -> const std::string& {
    static const auto suffix = std::to_string(static_cast<unsigned long long>(::getpid())) + "-" +
                               std::to_string(static_cast<unsigned long long>(
                                   std::chrono::steady_clock::now().time_since_epoch().count()));
    return suffix;
}

[[nodiscard]] auto reserve_port() -> std::optional<std::uint16_t> {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return std::nullopt;
    }
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = 0;
    static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr));
    if (::bind(descriptor, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
        static_cast<void>(::close(descriptor));
        return std::nullopt;
    }
    socklen_t size = sizeof(endpoint);
    if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&endpoint), &size) != 0) {
        static_cast<void>(::close(descriptor));
        return std::nullopt;
    }
    static_cast<void>(::close(descriptor));
    return ntohs(endpoint.sin_port);
}

[[nodiscard]] auto storage_mode(const std::string& storage) -> glifistore::StorageMode {
    if (storage == "durable-group") {
        return glifistore::StorageMode::durable_group;
    }
    if (storage == "durable-periodic") {
        return glifistore::StorageMode::durable_periodic;
    }
    return glifistore::StorageMode::durable_sync;
}

class DaemonProcess final {
  public:
    DaemonProcess(std::filesystem::path binary, std::filesystem::path data_dir, std::uint16_t port,
                  std::string storage)
        : binary_(std::move(binary)), data_dir_(std::move(data_dir)), port_(port),
          storage_(std::move(storage)) {}

    ~DaemonProcess() {
        stop_kill();
    }

    DaemonProcess(const DaemonProcess&) = delete;
    auto operator=(const DaemonProcess&) -> DaemonProcess& = delete;

    [[nodiscard]] auto start() -> bool {
        if (pid_ > 0) {
            return false;
        }
        const pid_t child = ::fork();
        if (child < 0) {
            std::cerr << "fork failed: " << std::strerror(errno) << '\n';
            return false;
        }
        if (child == 0) {
            const auto port_text = std::to_string(port_);
            const char* argv[] = {
                binary_.c_str(),   "--bind",      "127.0.0.1",      "--port",         port_text.c_str(),
                "--workers",       "1",           "--storage-mode", storage_.c_str(), "--data-dir",
                data_dir_.c_str(), "--open-mode", "open-or-create", "--quiet",        nullptr};
            ::execv(binary_.c_str(), const_cast<char* const*>(argv));
            std::cerr << "execv failed: " << std::strerror(errno) << '\n';
            std::_Exit(127);
        }
        pid_ = child;
        return true;
    }

    void stop_kill() {
        if (pid_ <= 0) {
            return;
        }
        static_cast<void>(::kill(pid_, SIGKILL));
        int status = 0;
        static_cast<void>(::waitpid(pid_, &status, 0));
        pid_ = -1;
    }

    [[nodiscard]] auto port() const noexcept -> std::uint16_t {
        return port_;
    }

  private:
    std::filesystem::path binary_;
    std::filesystem::path data_dir_;
    std::uint16_t port_{};
    std::string storage_;
    pid_t pid_{-1};
};

[[nodiscard]] auto connect_socket(const std::uint16_t port) -> int {
    const auto socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0) {
        return -1;
    }
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr));
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
        static_cast<void>(::close(socket));
        return -1;
    }
    return socket;
}

[[nodiscard]] auto send_all(const int socket, const std::span<const std::byte> data) -> bool {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto written = ::send(socket, data.data() + sent, data.size() - sent, 0);
        if (written <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

[[nodiscard]] auto receive_exact(const int socket, const std::span<std::byte> output) -> bool {
    std::size_t received = 0;
    while (received < output.size()) {
        const auto count = ::recv(socket, output.data() + received, output.size() - received, 0);
        if (count <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(count);
    }
    return true;
}

[[nodiscard]] auto discard_response(const int socket) -> bool {
    std::array<std::byte, glifistore::server::kResponseHeaderBytes> header{};
    if (!receive_exact(socket, header)) {
        return false;
    }
    const auto frame_size = static_cast<std::size_t>(
        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[0])) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[1])) << 8U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[2])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[3])) << 24U));
    if (frame_size < header.size()) {
        return false;
    }
    std::vector<std::byte> tail(frame_size - header.size());
    return receive_exact(socket, tail);
}

[[nodiscard]] auto initialize_wire_session(const int socket) -> bool {
    const auto init = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::init,
        .request_id = 1,
    });
    if (!init || !send_all(socket, *init) || !discard_response(socket)) {
        return false;
    }
    const auto bind = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::bind_worker,
        .request_id = 2,
        .target_worker = 0,
    });
    return bind.has_value() && send_all(socket, *bind) && discard_response(socket);
}

[[nodiscard]] auto wait_for_socket(const std::uint16_t port, const int timeout_ms = 10'000) -> int {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout_ms};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto socket = connect_socket(port);
        if (socket >= 0) {
            return socket;
        }
        ::usleep(20'000);
    }
    return -1;
}

[[nodiscard]] auto wait_for_client(const std::uint16_t port, const int timeout_ms = 10'000)
    -> std::optional<glifistore::client::Client> {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout_ms};
    while (std::chrono::steady_clock::now() < deadline) {
        auto connected = glifistore::client::Client::connect(
            {.host = "127.0.0.1", .port = port, .connect_timeout_ms = 200, .request_timeout_ms = 2'000});
        if (connected) {
            return std::move(*connected);
        }
        ::usleep(20'000);
    }
    return std::nullopt;
}

[[nodiscard]] auto value_text(const std::span<const std::byte> value) -> std::string_view {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] auto verify_store_key(const std::filesystem::path& data_dir, const std::string& storage,
                                    const bool expect_present) -> bool {
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex,
        .storage_mode = storage_mode(storage),
        .data_directory = data_dir,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    if (!opened) {
        std::cerr << "Store reopen failed: " << opened.error().message << '\n';
        return false;
    }
    const auto value = (*opened)->get(kAckKey);
    if (expect_present) {
        if (!value) {
            std::cerr << "acknowledged key missing after SIGKILL: " << value.error().message << '\n';
            return false;
        }
        if (value_text(value->bytes) != kAckValue) {
            std::cerr << "acknowledged key recovered with unexpected value\n";
            return false;
        }
    } else if (value) {
        std::cerr << "pre-commit mutation became visible after SIGKILL\n";
        return false;
    } else if (value.error().code != glifistore::ErrorCode::not_found) {
        std::cerr << "unexpected reopen error: " << value.error().message << '\n';
        return false;
    }
    if (auto closed = (*opened)->close(); !closed) {
        std::cerr << "Store close failed: " << closed.error().message << '\n';
        return false;
    }
    return true;
}

[[nodiscard]] auto verify_wire_get(const std::uint16_t port, const bool expect_present) -> bool {
    auto client = wait_for_client(port);
    if (!client) {
        std::cerr << "timed out waiting for restarted glifistored\n";
        return false;
    }
    const auto get = client->get(kAckKey);
    if (expect_present) {
        if (!get) {
            std::cerr << "wire GET after restart failed: " << get.error().message << '\n';
            return false;
        }
        if (value_text(*get) != kAckValue) {
            std::cerr << "wire GET after restart returned unexpected value\n";
            return false;
        }
    } else if (get) {
        std::cerr << "wire GET found key that should be absent\n";
        return false;
    } else if (get.error().code != glifistore::ErrorCode::not_found) {
        std::cerr << "wire GET returned unexpected error: " << get.error().message << '\n';
        return false;
    }
    client->close();
    return true;
}

[[nodiscard]] auto run_checkpoint(const Options& options) -> bool {
    const auto root = std::filesystem::temp_directory_path() /
                      ("glifistore-crash-daemon-" + run_suffix() + "-" + options.storage + "-" +
                       std::string{checkpoint_name(options.checkpoint)});
    const auto data_dir = root / "store";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(data_dir);

    const auto port = reserve_port();
    if (!port) {
        std::cerr << "failed to reserve a loopback port\n";
        return false;
    }

    DaemonProcess daemon{options.daemon, data_dir, *port, options.storage};
    if (!daemon.start()) {
        return false;
    }

    const bool expect_present = options.checkpoint == Checkpoint::post_ack;
    if (options.checkpoint == Checkpoint::pre_commit) {
        const auto socket = wait_for_socket(daemon.port());
        if (socket < 0) {
            std::cerr << "timed out waiting for glifistored to accept connections\n";
            return false;
        }
        if (!initialize_wire_session(socket)) {
            std::cerr << "failed to initialize wire session\n";
            static_cast<void>(::close(socket));
            return false;
        }
        const auto put = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::put,
            .request_id = 3,
            .key = {reinterpret_cast<const std::byte*>(kAckKey.data()), kAckKey.size()},
            .value = {reinterpret_cast<const std::byte*>(kAckValue.data()), kAckValue.size()},
        });
        if (!put || !send_all(socket, *put)) {
            std::cerr << "failed to send pre-commit PUT frame\n";
            static_cast<void>(::close(socket));
            return false;
        }
        static_cast<void>(::close(socket));
        daemon.stop_kill();
    } else {
        auto client = wait_for_client(daemon.port());
        if (!client) {
            std::cerr << "timed out waiting for glifistored to accept connections\n";
            return false;
        }

        const auto put = client->put(kAckKey, kAckValue);
        if (!put.committed()) {
            std::cerr << "wire PUT was not acknowledged as committed";
            if (put.error) {
                std::cerr << ": " << put.error->message;
            }
            std::cerr << '\n';
            return false;
        }

        if (options.checkpoint == Checkpoint::post_ack_erase) {
            const auto erased = client->erase(kAckKey);
            if (!erased.committed()) {
                std::cerr << "wire ERASE was not acknowledged as committed";
                if (erased.error) {
                    std::cerr << ": " << erased.error->message;
                }
                std::cerr << '\n';
                return false;
            }
        }
        client->close();

        if (options.storage == "durable-periodic" && options.checkpoint == Checkpoint::post_ack) {
            ::usleep(3'000'000);
        }

        daemon.stop_kill();
    }

    if (!verify_store_key(data_dir, options.storage, expect_present)) {
        return false;
    }

    DaemonProcess restarted{options.daemon, data_dir, *port, options.storage};
    if (!restarted.start()) {
        return false;
    }
    if (!verify_wire_get(restarted.port(), expect_present)) {
        return false;
    }
    restarted.stop_kill();

    std::filesystem::remove_all(root, ignored);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    auto options = parse_options(argc, argv);
    if (!options) {
        print_usage(argc > 0 ? argv[0] : "glifistore_crash_daemon");
        return 2;
    }
    std::cout << "# crash-daemon storage=" << options->storage
              << " checkpoint=" << checkpoint_name(options->checkpoint) << " SIGKILL\n";
    if (!run_checkpoint(*options)) {
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
