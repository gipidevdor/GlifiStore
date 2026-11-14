#pragma once

#include "glifistore/store/store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace glifistore::test {
namespace stateful_store_detail {

[[nodiscard]] inline auto bytes(const std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

[[nodiscard]] inline auto value_string(const OwnedValue& value) -> std::string {
    return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-state-model-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error{"state-machine temporary directory creation failed"};
        }
        root_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] auto store_path() const -> std::filesystem::path {
        return root_ / "store";
    }

  private:
    std::filesystem::path root_;
};

[[noreturn]] inline void invariant_failure(const std::uint64_t seed, const std::size_t operation,
                                           const std::string_view detail) {
    throw std::runtime_error{"stateful Store model seed=" + std::to_string(seed) +
                             " operation=" + std::to_string(operation) + ": " + std::string{detail}};
}

} // namespace stateful_store_detail

// A bounded durable Store state machine shared by deterministic property tests
// and libFuzzer. The input format is deliberately compact: opcode, key selector,
// then an optional bounded value. Every step is checked against an independent
// map and every close/reopen crosses the real persistence-v1 recovery path.
inline void run_stateful_store_model(const std::span<const std::uint8_t> input,
                                     const std::uint64_t seed = 0) {
    using namespace stateful_store_detail;
    constexpr std::array<std::string_view, 8> keys{
        "k0", "k1", "k2", "k3", "k4", "k5", "k6", "k7",
    };
    constexpr std::size_t maximum_operations = 32;
    constexpr std::size_t maximum_value_bytes = 24;

    TemporaryDirectory temporary;
    StoreConfig config{
        .worker_config = {.explicit_count = 2},
        .storage_mode = StorageMode::durable_sync,
        .data_directory = temporary.store_path(),
        .durable_open_mode = DurableOpenMode::create_new,
        .maintenance = {.mode = MaintenanceMode::disabled},
    };
    auto opened = Store::open(config);
    if (!opened) {
        invariant_failure(seed, 0, "initial open failed");
    }
    auto store = std::move(*opened);
    std::unordered_map<std::string, std::string> model;
    std::size_t cursor{};

    const auto check_model = [&](const std::size_t operation) {
        if (auto verified = store->verify_index(); !verified) {
            invariant_failure(seed, operation, "verify_index rejected reachable state");
        }
        for (const auto key : keys) {
            const auto expected = model.find(std::string{key});
            auto actual = store->get(key);
            if (expected == model.end()) {
                if (actual || actual.error().code != ErrorCode::not_found) {
                    invariant_failure(seed, operation, "missing-key outcome diverged from model");
                }
                continue;
            }
            if (!actual || value_string(*actual) != expected->second) {
                invariant_failure(seed, operation, "stored value diverged from model");
            }
        }
    };

    for (std::size_t operation = 0; operation < maximum_operations && cursor + 2U <= input.size();
         ++operation) {
        const auto opcode = input[cursor++] % 6U;
        const auto key = keys[input[cursor++] % keys.size()];
        switch (opcode) {
        case 0: {
            const auto available = input.size() - cursor;
            const auto requested =
                available == 0 ? std::size_t{} : input[cursor++] % (maximum_value_bytes + 1U);
            const auto value_size = std::min(requested, input.size() - cursor);
            const std::string value{reinterpret_cast<const char*>(input.data() + cursor), value_size};
            cursor += value_size;
            if (auto status = store->put(key, bytes(value)); !status) {
                invariant_failure(seed, operation, "PUT failed within bounded model");
            }
            model.insert_or_assign(std::string{key}, value);
            break;
        }
        case 1: {
            const auto expected = model.find(std::string{key});
            auto actual = store->get(key);
            if (expected == model.end()) {
                if (actual || actual.error().code != ErrorCode::not_found) {
                    invariant_failure(seed, operation, "GET missing-key outcome diverged");
                }
            } else if (!actual || value_string(*actual) != expected->second) {
                invariant_failure(seed, operation, "GET value diverged");
            }
            break;
        }
        case 2:
            if (auto status = store->erase(key); !status && (model.contains(std::string{key}) ||
                                                             status.error().code != ErrorCode::not_found)) {
                invariant_failure(seed, operation, "ERASE outcome diverged from model");
            }
            model.erase(std::string{key});
            break;
        case 3:
            if (auto compacted = store->compact(); !compacted) {
                invariant_failure(seed, operation, "COMPACT failed within bounded model");
            }
            break;
        case 4: {
            if (auto status = store->close(); !status) {
                invariant_failure(seed, operation, "CLOSE failed within bounded model");
            }
            store.reset();
            config.durable_open_mode = DurableOpenMode::open_existing;
            auto reopened = Store::open(config);
            if (!reopened) {
                invariant_failure(seed, operation, "REOPEN failed after successful CLOSE");
            }
            store = std::move(*reopened);
            break;
        }
        case 5:
            if (auto verified = store->verify_index(); !verified) {
                invariant_failure(seed, operation, "VERIFY rejected reachable state");
            }
            break;
        default:
            invariant_failure(seed, operation, "decoder produced an invalid opcode");
        }
        check_model(operation);
    }
    check_model(maximum_operations);
    if (auto status = store->close(); !status) {
        invariant_failure(seed, maximum_operations, "final CLOSE failed");
    }
}

} // namespace glifistore::test
