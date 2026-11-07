#include "glifistore/store/config.hpp"
#include "glifistore/store/store.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

[[nodiscard]] auto bytes(const std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

[[nodiscard]] auto temporary_directory() -> std::filesystem::path {
    static std::atomic<std::uint64_t> counter{0};
    return std::filesystem::temp_directory_path() /
           ("glifistore-pgo-" + std::to_string(static_cast<unsigned long>(::getpid())) + '-' +
            std::to_string(counter.fetch_add(1U, std::memory_order_relaxed)));
}

void durable_training_loop(const std::filesystem::path& data_dir, const std::size_t operations) {
    {
        auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                               .storage_mode = glifistore::StorageMode::durable_sync,
                                               .data_directory = data_dir,
                                               .durable_open_mode = glifistore::DurableOpenMode::create_new});
        if (!opened) {
            throw std::runtime_error("durable PGO training failed to create store");
        }
        for (std::size_t index = 0; index < operations; ++index) {
            const auto key = std::string{"pgo-key-"} + std::to_string(index % 128);
            const auto value = std::string{"pgo-value-"} + std::to_string(index);
            if (!(*opened)->put(key, bytes(value)).has_value()) {
                throw std::runtime_error("durable PGO training put failed");
            }
            const auto read = (*opened)->get(key);
            if (!read || read->bytes.size() != value.size()) {
                throw std::runtime_error("durable PGO training get failed");
            }
            if ((index % 512) == 0 && !(*opened)->verify_index().has_value()) {
                throw std::runtime_error("durable PGO training verify_index failed");
            }
        }
    }

    auto reopened =
        glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                 .storage_mode = glifistore::StorageMode::durable_sync,
                                 .data_directory = data_dir,
                                 .durable_open_mode = glifistore::DurableOpenMode::open_existing});
    if (!reopened) {
        throw std::runtime_error("durable PGO training failed to reopen store");
    }
    for (std::size_t index = 0; index < 128; ++index) {
        const auto key = std::string{"pgo-key-"} + std::to_string(index);
        if (!(*reopened)->get(key).has_value()) {
            throw std::runtime_error("durable PGO training recovery read failed");
        }
    }
    if (!(*reopened)->verify_index().has_value()) {
        throw std::runtime_error("durable PGO training recovery verify_index failed");
    }
}

} // namespace

int main(int argc, char** argv) {
    std::size_t operations = 4'096;
    if (argc > 1) {
        operations = static_cast<std::size_t>(std::stoull(argv[1]));
    }
    try {
        const auto data_dir = temporary_directory();
        durable_training_loop(data_dir, operations);
        std::error_code ignored;
        std::filesystem::remove_all(data_dir, ignored);
        std::cout << "# durable PGO training operations=" << operations << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "glifistore_pgo_durable: fatal: " << exception.what() << '\n';
        return 1;
    }
}
