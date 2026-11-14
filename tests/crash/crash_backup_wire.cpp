#include "crash_checkpoint.hpp"
#include "glifistore/client/client.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/store_verify.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/store/store.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

[[nodiscard]] auto value_string(const glifistore::OwnedValue& value) -> std::string_view {
    return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

[[nodiscard]] auto crash_run_suffix() -> std::string {
    return std::to_string(static_cast<unsigned long long>(::getpid())) + "-" +
           std::to_string(
               static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
}

struct Options {
    std::string boundary{"copy_backup_segment"};
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
        if (arg == "--boundary") {
            const auto value = require_value("--boundary");
            if (!value) {
                return std::nullopt;
            }
            options.boundary = std::string{*value};
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
    return options;
}

void print_usage(const char* argv0) {
    std::cerr << "usage: " << argv0
              << " [--boundary copy_backup_segment|copy_backup_manifest|sync_backup_destination]"
              << " [--data-dir PATH] [--backup-dir PATH] [--checkpoint-dir PATH]\n"
              << "  Wire BACKUP process-kill matrix (in-process Server + Client; not glifistored exec).\n";
}

void run_worker(const Options& options) {
    glifistore::crash::CheckpointState checkpoint{
        .checkpoint_dir = options.checkpoint_dir,
        .kill_at = options.boundary,
    };

    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4}, {.worker_config = {.explicit_count = 1},
                                                .storage_mode = glifistore::StorageMode::durable_sync,
                                                .data_directory = options.data_dir,
                                                .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                                .filesystem_hooks = checkpoint.hooks()});
    if (!opened) {
        throw std::runtime_error(opened.error().message);
    }
    auto& server = **opened;
    if (!server.start()) {
        throw std::runtime_error("server start failed");
    }

    auto connected = glifistore::client::Client::connect({
        .port = server.port(),
        .request_timeout_ms = 60'000,
    });
    if (!connected) {
        throw std::runtime_error(connected.error().message);
    }
    auto& client = *connected;
    if (!client.put("keep", "alive").committed()) {
        throw std::runtime_error("wire put failed");
    }

    std::error_code ec;
    std::filesystem::create_directories(options.backup_dir.parent_path(), ec);
    if (ec) {
        throw std::runtime_error("create backup parent failed");
    }

    // Wire opcode BACKUP (not Store::backup_to): exercises reactor dispatch + admission fence.
    auto backed = client.backup(options.backup_dir.string());
    if (!backed) {
        throw std::runtime_error(backed.error().message);
    }
    server.request_stop();
    if (!server.join()) {
        throw std::runtime_error("server join failed");
    }
}

[[nodiscard]] auto verify_after_kill(const Options& options) -> bool {
    const auto verified_backup = glifistore::verify_durable_store_path(options.backup_dir);
    if (options.boundary == "copy_backup_segment" || options.boundary == "copy_backup_segment#1") {
        if (verified_backup.has_value()) {
            std::cerr << "incomplete wire backup unexpectedly verified after kill at " << options.boundary
                      << '\n';
            return false;
        }
        if (std::filesystem::exists(options.backup_dir / glifistore::kManifestFilename)) {
            std::cerr << "manifest present after mid-segment wire BACKUP kill\n";
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
        std::cerr << "source key missing after wire BACKUP kill\n";
        return false;
    }
    if (!(*reopened)->close()) {
        std::cerr << "source close failed after reopen\n";
        return false;
    }

    // Restart an in-process Server on the same data dir and read via wire GET.
    auto restarted =
        glifistore::server::Server::create({.port = 0, .maximum_connections = 4},
                                           {.worker_config = {.explicit_count = 1},
                                            .storage_mode = glifistore::StorageMode::durable_sync,
                                            .data_directory = options.data_dir,
                                            .durable_open_mode = glifistore::DurableOpenMode::open_existing});
    if (!restarted) {
        std::cerr << "server restart failed: " << restarted.error().message << '\n';
        return false;
    }
    auto& server = **restarted;
    if (!server.start()) {
        std::cerr << "server restart start failed\n";
        return false;
    }
    auto connected = glifistore::client::Client::connect({.port = server.port()});
    if (!connected) {
        std::cerr << "client reconnect failed: " << connected.error().message << '\n';
        return false;
    }
    const auto wire_got = connected->get("keep");
    if (!wire_got) {
        std::cerr << "wire GET after restart failed: " << wire_got.error().message << '\n';
        server.request_stop();
        static_cast<void>(server.join());
        return false;
    }
    const auto wire_text =
        std::string_view{reinterpret_cast<const char*>(wire_got->data()), wire_got->size()};
    if (wire_text != "alive") {
        std::cerr << "wire GET after restart returned unexpected value\n";
        server.request_stop();
        static_cast<void>(server.join());
        return false;
    }
    server.request_stop();
    if (!server.join()) {
        std::cerr << "server restart join failed\n";
        return false;
    }
    return true;
}

[[nodiscard]] auto run_case(Options options) -> bool {
    if (options.data_dir.empty()) {
        options.data_dir = std::filesystem::temp_directory_path() /
                           ("glifistore-crash-backup-wire-" + crash_run_suffix()) / "store";
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

    const pid_t child = ::fork();
    if (child < 0) {
        std::cerr << "fork failed: " << std::strerror(errno) << '\n';
        return false;
    }
    if (child == 0) {
        try {
            run_worker(options);
            std::cerr << "worker completed without hitting kill boundary " << options.boundary << '\n';
            std::_Exit(2);
        } catch (const std::exception& exception) {
            std::cerr << "worker failed: " << exception.what() << '\n';
            std::_Exit(1);
        }
    }

    if (!glifistore::crash::wait_for_checkpoint(options.checkpoint_dir, options.boundary)) {
        std::cerr << "timed out waiting for checkpoint " << options.boundary << '\n';
        ::kill(child, SIGKILL);
        int status = 0;
        ::waitpid(child, &status, 0);
        return false;
    }
    ::kill(child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);

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

    const bool use_matrix = argc == 1;
    const std::vector<std::string> matrix{"copy_backup_segment", "copy_backup_manifest",
                                          "sync_backup_destination"};
    const std::vector<std::string> single{options->boundary};
    const auto& cases = use_matrix ? matrix : single;

    bool success = true;
    for (const auto& boundary : cases) {
        Options one = *options;
        one.boundary = boundary;
        std::cout << "# crash-backup-wire boundary=" << boundary << '\n';
        if (!run_case(one)) {
            success = false;
        }
    }
    return success ? 0 : 1;
}
