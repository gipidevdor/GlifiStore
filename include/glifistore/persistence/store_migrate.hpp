/**
 * @file
 * @brief Offline logical resharding into a separate destination with resumable checkpoints.
 */
#pragma once

#include "glifistore/persistence/store_verify.hpp"
#include "glifistore/store/config.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace glifistore {

/**
 * @brief Offline logical rewrite progress, checkpoint identity, and source/destination verification.
 */
struct DurableStoreMigrateReport {
    std::filesystem::path source;
    std::filesystem::path destination;
    std::filesystem::path checkpoint;
    std::size_t source_worker_count{};
    std::size_t target_worker_count{};
    std::uint64_t keys_copied{};
    std::uint64_t keys_skipped{};
    std::uint64_t bytes_copied{};
    bool resumed{};
    DurableStoreVerifyReport source_verification{};
    DurableStoreVerifyReport destination_verification{};
};

// Offline fail-closed Worker reshard / logical rewrite: verify source, copy live key/value/expiry
// state into a new destination Store with target_worker_count, checkpoint after each put, verify
// destination. Source is never mutated. Destination must be empty unless a matching sibling
// checkpoint (<destination>.migrate-state) allows resume. Live/hot migration is unsupported.
// destination_hooks is a test/fault-injection seam applied only to the destination Store open.
[[nodiscard]] auto
migrate_durable_store(const std::filesystem::path& source, const std::filesystem::path& destination,
                      std::size_t target_worker_count, bool scan_records = true,
                      const DurableResourceLimits& limits = {}, FilesystemHooks destination_hooks = {})
    -> Result<DurableStoreMigrateReport>;

[[nodiscard]] inline auto migrate_checkpoint_path(const std::filesystem::path& destination)
    -> std::filesystem::path {
    return std::filesystem::path{destination.string() + ".migrate-state"};
}

} // namespace glifistore
