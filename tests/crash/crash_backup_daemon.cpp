#include "crash_checkpoint.hpp"
#include "glifistore/client/client.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/store_verify.hpp"
#include "glifistore/store/store.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <netinet/in.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

[[nodiscard]] auto bytes(std::string_view text) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

[[nodiscard]] auto value_string(const glifistore::OwnedValue& value) -> std::string_view {
    return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

[[nodiscard]] auto crash_run_suffix() -> std::string {
    return std::to_string(static_cast<unsigned long long>(::getpid())) + "-" +
           std::to_string(
               static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
}

struct Options {
    std::filesystem::path daemon{};
    std::string boundary{"copy_backup_segment"};
    bool boundary_set{false};
    std::filesystem::path data_dir{};
    std::filesystem::path backup_dir{};
    std::filesystem::path checkpoint_dir{};
};

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
        } else if (arg == "--boundary") {
            const auto value = require_value("--boundary");
            if (!value) {
                return std::nullopt;
            }
            options.boundary = std::string{*value};
            options.boundary_set = true;
        } else if (arg == "--data-dir") {
            const auto value = require_value("--data-dir");
            if (!value) {
                return std::nullopt;
            }
            options.data_dir = *value;
        } else if (arg == "--backup-dir") {
            const auto value = require_value("--backup-dir");
            if (!value) {
                return std::nullopt;
            }
            options.backup_dir = *value;
        } else if (arg == "--checkpoint-dir") {
            const auto value = require_value("--checkpoint-dir");
            if (!value) {
                return std::nullopt;
            }
            options.checkpoint_dir = *value;
        } else if (arg == "--help") {
            return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    if (options.daemon.empty()) {
        return std::nullopt;
    }
    return options;
}

void print_usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " --daemon PATH"
              << " [--boundary copy_backup_segment|copy_backup_manifest|sync_backup_destination]"
              << " [--data-dir PATH] [--backup-dir PATH] [--checkpoint-dir PATH]\n"
              << "  Wire BACKUP process-kill matrix against a real glifistored exec"
              << " (GLIFISTORE_CRASH_TEST hooks).\n";
}

[[nodiscard]] auto pick_port() -> std::uint16_t {
    const auto socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0) {
        return 0;
    }
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    endpoint.sin_port = 0;
    if (::bind(socket, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
        ::close(socket);
        return 0;
    }
    socklen_t length = sizeof(endpoint);
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&endpoint), &length) != 0) {
        ::close(socket);
        return 0;
    }
    ::close(socket);
    return ntohs(endpoint.sin_port);
}

[[nodiscard]] auto wait_for_listen(const std::uint16_t port, const int timeout_ms = 15'000) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout_ms};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto socket = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket >= 0) {
            sockaddr_in endpoint{};
            endpoint.sin_family = AF_INET;
            endpoint.sin_port = htons(port);
            static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr));
            if (::connect(socket, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0) {
                ::close(socket);
                return true;
            }
            ::close(socket);
        }
        ::usleep(20'000);
    }
    return false;
}

[[nodiscard]] auto seed_store(const std::filesystem::path& data_dir) -> bool {
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = data_dir,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
    });
    if (!opened) {
        std::cerr << "seed open failed: " << opened.error().message << '\n';
        return false;
    }
    const auto put = (*opened)->put("keep", bytes("alive"));
    if (!put) {
        std::cerr << "seed put failed\n";
        return false;
    }
    if (!(*opened)->close()) {
        std::cerr << "seed close failed\n";
        return false;
    }
    return true;
}

[[nodiscard]] auto verify_after_kill(const Options& options) -> bool {
    const auto verified_backup = glifistore::verify_durable_store_path(options.backup_dir);
    if (options.boundary == "copy_backup_segment" || options.boundary == "copy_backup_segment#1") {
        if (verified_backup.has_value()) {
            std::cerr << "incomplete daemon backup unexpectedly verified after kill at " << options.boundary
                      << '\n';
            return false;
        }
        if (std::filesystem::exists(options.backup_dir / glifistore::kManifestFilename)) {
            std::cerr << "manifest present after mid-segment glifistored BACKUP kill\n";
            return false;
        }
    } else if (options.boundary != "copy_backup_manifest" && options.boundary != "sync_backup_destination") {
        std::cerr << "unsupported boundary: " << options.boundary << '\n';
        return false;
    }

    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = options.data_dir,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    if (!reopened) {
        std::cerr << "source reopen failed: " << reopened.error().message << '\n';
        return false;
    }
    const auto got = (*reopened)->get("keep");
    if (!got || value_string(*got) != "alive") {
        std::cerr << "source key missing after glifistored BACKUP kill\n";
        return false;
    }
    return (*reopened)->close().has_value();
}

[[nodiscard]] auto run_case(Options options) -> bool {
    if (options.data_dir.empty()) {
        options.data_dir = std::filesystem::temp_directory_path() /
                           ("glifistore-crash-backup-daemon-" + crash_run_suffix()) / "store";
    }
    if (options.backup_dir.empty()) {
        options.backup_dir = options.data_dir.parent_path() / "backup";
    }
    if (options.checkpoint_dir.empty()) {
        options.checkpoint_dir = options.data_dir.parent_path() / "checkpoints";
    }

    std::error_code ignored;
    std::filesystem::remove_all(options.data_dir.parent_path(), ignored);
    std::filesystem::create_directories(options.checkpoint_dir);
    glifistore::crash::remove_checkpoint_markers(options.checkpoint_dir);

    if (!seed_store(options.data_dir)) {
        return false;
    }

    const auto port = pick_port();
    if (port == 0) {
        std::cerr << "failed to reserve ephemeral port\n";
        return false;
    }

    const pid_t child = ::fork();
    if (child < 0) {
        std::cerr << "fork failed: " << std::strerror(errno) << '\n';
        return false;
    }
    if (child == 0) {
        ::setenv("GLIFISTORE_CRASH_TEST", "1", 1);
        ::setenv("GLIFISTORE_CRASH_KILL_AT", options.boundary.c_str(), 1);
        ::setenv("GLIFISTORE_CRASH_CHECKPOINT_DIR", options.checkpoint_dir.c_str(), 1);
        const auto port_text = std::to_string(port);
        const char* argv[] = {
            options.daemon.c_str(),   "--bind",      "127.0.0.1",      "--port",       port_text.c_str(),
            "--shard-pairs",          "1",           "--storage-mode", "durable-sync", "--data-dir",
            options.data_dir.c_str(), "--open-mode", "open-existing",  "--quiet",      nullptr};
        ::execv(options.daemon.c_str(), const_cast<char* const*>(argv));
        std::cerr << "execv failed: " << std::strerror(errno) << '\n';
        std::_Exit(127);
    }

    if (!wait_for_listen(port)) {
        std::cerr << "timed out waiting for glifistored listen\n";
        ::kill(child, SIGKILL);
        int status = 0;
        ::waitpid(child, &status, 0);
        return false;
    }

    auto connected = glifistore::client::Client::connect({
        .port = port,
        .request_timeout_ms = 60'000,
    });
    if (!connected) {
        std::cerr << "client connect failed: " << connected.error().message << '\n';
        ::kill(child, SIGKILL);
        int status = 0;
        ::waitpid(child, &status, 0);
        return false;
    }

    // Kick wire BACKUP; the daemon should self-SIGKILL at the filesystem boundary.
    std::thread backup_thread{[&] { static_cast<void>(connected->backup(options.backup_dir.string())); }};

    if (!glifistore::crash::wait_for_checkpoint(options.checkpoint_dir, options.boundary)) {
        std::cerr << "timed out waiting for checkpoint " << options.boundary << '\n';
        ::kill(child, SIGKILL);
        int status = 0;
        ::waitpid(child, &status, 0);
        backup_thread.join();
        return false;
    }
    ::kill(child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);
    backup_thread.join();

    const bool ok = verify_after_kill(options);
    std::filesystem::remove_all(options.data_dir.parent_path(), ignored);
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    auto options = parse_options(argc, argv);
    if (!options) {
        print_usage(argv[0]);
        return 2;
    }

    const std::vector<std::string> matrix{"copy_backup_segment", "copy_backup_manifest",
                                          "sync_backup_destination"};
    const std::vector<std::string> single{options->boundary};
    const auto& selected = options->boundary_set ? single : matrix;

    bool success = true;
    for (const auto& boundary : selected) {
        Options one = *options;
        one.boundary = boundary;
        std::cout << "# crash-backup-daemon boundary=" << boundary << '\n';
        if (!run_case(one)) {
            success = false;
        }
    }
    return success ? 0 : 1;
}
