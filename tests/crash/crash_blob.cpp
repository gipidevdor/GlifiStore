#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/store/store.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace glifistore;

auto config(const std::filesystem::path& path, const std::string_view policy) -> StoreConfig {
    StoreConfig cfg;
    cfg.data_directory = path;
    cfg.enable_blobs = true;
    cfg.worker_config.explicit_count = 1;
    cfg.storage_mode = policy == "sync"    ? StorageMode::durable_sync
                       : policy == "group" ? StorageMode::durable_group
                                           : StorageMode::durable_periodic;
    cfg.maintenance.mode = MaintenanceMode::disabled;
    cfg.durable_limits.reserved_free_bytes = 0;
    cfg.durable_periodic.sync_interval_ms = 60'000;
    cfg.durable_periodic.batch.reset();
    return cfg;
}

[[noreturn]] void crash_now() noexcept {
    static_cast<void>(::kill(::getpid(), SIGKILL));
    ::_exit(125);
}

struct Checkpoint {
    std::string target;
    std::atomic_uint64_t seen{};
    std::uint64_t occurrence{1};

    static void after(void* opaque, FilesystemOperation operation) {
        auto& self = *static_cast<Checkpoint*>(opaque);
        if (filesystem_operation_name(operation) == self.target &&
            self.seen.fetch_add(1) + 1 == self.occurrence)
            crash_now();
    }
};

auto worker(const std::filesystem::path& path, const std::string_view policy, const std::string_view boundary)
    -> int {
    Checkpoint checkpoint{.target = std::string{boundary}};
    if (const auto hash = checkpoint.target.find('#'); hash != std::string::npos) {
        checkpoint.occurrence = std::stoull(checkpoint.target.substr(hash + 1));
        checkpoint.target.resize(hash);
    }
    auto cfg = config(path, policy);
    cfg.durable_open_mode = DurableOpenMode::open_existing;
    cfg.filesystem_hooks.context = &checkpoint;
    cfg.filesystem_hooks.after = &Checkpoint::after;
    auto store = Store::open(cfg);
    if (!store) {
        std::cerr << store.error().message << '\n';
        return 2;
    }
    std::vector<std::byte> value(2 * kBlobChunkBytes + 17, std::byte{0x5a});
    auto result = (*store)->put("key", value);
    if (!result) {
        std::cerr << result.error().message << '\n';
        return 3;
    }
    if (boundary == "ack")
        crash_now();
    if (auto flushed = (*store)->flush(); !flushed) {
        std::cerr << flushed.error().message << '\n';
        return 4;
    }
    if (boundary == "flush")
        crash_now();
    std::cerr << "checkpoint not reached: " << boundary << '\n';
    return 5;
}

auto run_case(const std::string& executable, const std::string& policy, const std::string& boundary,
              const std::size_t index) -> bool {
    const auto path = std::filesystem::temp_directory_path() /
                      ("glifi-blob-crash-" + std::to_string(::getpid()) + "-" + std::to_string(index));

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{path};

    std::filesystem::remove_all(path);
    auto cfg = config(path, policy);
    {
        auto store = Store::open(cfg);
        if (!store) {
            std::cerr << store.error().message << '\n';
            return false;
        }
        const std::array old{std::byte{'o'}, std::byte{'l'}, std::byte{'d'}};
        if (!(*store)->put("key", old) || !(*store)->flush() || !(*store)->close())
            return false;
    }
    const auto child = ::fork();
    if (child < 0)
        return false;
    if (child == 0) {
        ::execl(executable.c_str(), executable.c_str(), "--worker", path.c_str(), policy.c_str(),
                boundary.c_str(), static_cast<char*>(nullptr));
        ::_exit(126);
    }
    int status{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    for (;;) {
        const auto waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child)
            break;
        if (waited < 0)
            return false;
        if (std::chrono::steady_clock::now() >= deadline) {
            static_cast<void>(::kill(child, SIGKILL));
            static_cast<void>(::waitpid(child, &status, 0));
            std::cerr << "worker timeout " << policy << ' ' << boundary << '\n';
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        std::cerr << "wrong worker status " << status << '\n';
        return false;
    }
    auto store = Store::open(cfg);
    if (!store) {
        std::cerr << "recovery failed: " << store.error().message << '\n';
        return false;
    }
    auto value = (*store)->get("key");
    if (!value) {
        std::cerr << value.error().message << '\n';
        return false;
    }
    const bool old = value->bytes.size() == 3 && value->bytes.front() == std::byte{'o'};
    const bool complete = value->bytes.size() == 2 * kBlobChunkBytes + 17 &&
                          std::ranges::all_of(value->view(), [](auto b) { return b == std::byte{0x5a}; });
    if (!old && !complete)
        return false;
    const bool payload_phase = boundary.starts_with("write_blob") || boundary == "sync_blob_file" ||
                               boundary == "install_blob_file" || boundary == "sync_blob_directory" ||
                               boundary == "link_blob_file" || boundary == "write_manifest" ||
                               boundary == "sync_manifest" || boundary == "rename_manifest" ||
                               boundary == "sync_directory";
    if (payload_phase && !old)
        return false;
    if ((boundary == "flush" || (boundary == "ack" && policy != "periodic")) && !complete)
        return false;
    std::cout << "PASS blob crash " << policy << ' ' << boundary << " -> "
              << (complete ? "complete" : "previous") << '\n';
    return true;
}
} // namespace

auto main(int argc, char** argv) -> int try {
    if (argc == 5 && std::string_view{argv[1]} == "--worker")
        return worker(argv[2], argv[3], argv[4]);
    const auto executable = std::filesystem::absolute(argv[0]).string();
    const std::array boundaries{"write_manifest",
                                "sync_manifest",
                                "rename_manifest",
                                "sync_directory",
                                "write_blob_chunk#1",
                                "write_blob_chunk#2",
                                "write_blob_chunk#3",
                                "sync_blob_file",
                                "link_blob_file",
                                "install_blob_file",
                                "sync_blob_directory",
                                "write_record",
                                "sync_record",
                                "write_commit_slot",
                                "sync_commit_slot",
                                "ack",
                                "flush"};
    std::size_t index{};
    for (const auto policy : {"sync", "group", "periodic"})
        for (const auto boundary : boundaries)
            if (!run_case(executable, policy, boundary, index++))
                return 1;
    std::cout << index << " blob crash cases passed; E2 process termination only\n";
    return 0;
} catch (const std::exception& error) {

    std::cerr << error.what() << '\n';
    return 1;
}
