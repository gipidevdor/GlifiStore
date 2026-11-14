#include "crash_checkpoint.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/store_backup.hpp"
#include "glifistore/persistence/store_verify.hpp"
#include "glifistore/store/store.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

[[nodiscard]] auto bytes(std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
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
              << " [--data-dir PATH] [--backup-dir PATH] [--checkpoint-dir PATH]\n";
}

void run_worker(const Options& options) {
    glifistore::crash::CheckpointState checkpoint{
        .checkpoint_dir = options.checkpoint_dir,
        .kill_at = options.boundary,
    };
    auto opened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = options.data_dir,
        .durable_open_mode = glifistore::DurableOpenMode::create_new,
        .filesystem_hooks = checkpoint.hooks(),
    });
    if (!opened) {
        throw std::runtime_error(opened.error().message);
    }
    auto& store = **opened;
    if (!store.put("keep", bytes("alive"))) {
        throw std::runtime_error("put failed");
    }
    std::error_code ec;
    std::filesystem::create_directories(options.backup_dir.parent_path(), ec);
    if (ec) {
        throw std::runtime_error("create backup parent failed");
    }
    const auto backed = store.backup_to(options.backup_dir);
    if (!backed) {
        throw std::runtime_error(backed.error().message);
    }
    if (!store.close()) {
        throw std::runtime_error("close failed");
    }
}

[[nodiscard]] auto verify_after_kill(const Options& options) -> bool {
    const auto verified_backup = glifistore::verify_durable_store_path(options.backup_dir);
    if (options.boundary == "copy_backup_segment" || options.boundary == "copy_backup_segment#1") {
        if (verified_backup.has_value()) {
            std::cerr << "incomplete backup unexpectedly verified after kill at " << options.boundary << '\n';
            return false;
        }
        if (std::filesystem::exists(options.backup_dir / glifistore::kManifestFilename)) {
            std::cerr << "manifest present after mid-segment kill\n";
            return false;
        }
    } else if (options.boundary == "copy_backup_manifest" || options.boundary == "sync_backup_destination") {
        // Files may be complete; promotion still requires an explicit verify that may pass.
        // Source health is the hard requirement for these later boundaries.
    } else {
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
        std::cerr << "source key missing after backup kill\n";
        return false;
    }
    if (!(*reopened)->close()) {
        std::cerr << "source close failed after reopen\n";
        return false;
    }
    return true;
}

[[nodiscard]] auto run_case(Options options) -> bool {
    if (options.data_dir.empty()) {
        options.data_dir = std::filesystem::temp_directory_path() /
                           ("glifistore-crash-backup-" + crash_run_suffix()) / "store";
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

    const std::vector<std::string> boundaries =
        options->boundary.empty() ? std::vector<std::string>{} : std::vector<std::string>{options->boundary};
    // Default matrix when only defaults were requested (no explicit dirs/boundary override beyond
    // the struct default): run the incomplete-copy boundary plus later seams for source health.
    const bool use_matrix = argc == 1;
    const std::vector<std::string> matrix{"copy_backup_segment", "copy_backup_manifest",
                                          "sync_backup_destination"};
    const auto& cases = use_matrix ? matrix : boundaries;

    bool success = true;
    for (const auto& boundary : cases) {
        Options one = *options;
        one.boundary = boundary;
        std::cout << "# crash-backup boundary=" << boundary << '\n';
        if (!run_case(one)) {
            success = false;
        }
    }
    return success ? 0 : 1;
}
