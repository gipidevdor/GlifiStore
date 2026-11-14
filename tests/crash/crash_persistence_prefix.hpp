#pragma once

// Process-kill scenario for strict sealed-prefix durable compaction (ADR 0044): one Worker with three sealed
// Segments and an active one, of which a copy budget selects only the oldest two. It lives beside
// crash_persistence.cpp, which dispatches to it, so that harness stays under the structure line budget.

#include "glifistore/persistence/filesystem.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace glifistore::crash::prefix_scenario {

/// Scenario name accepted by `glifistore_crash_persistence --scenario`.
inline constexpr std::string_view kScenario{"compact-prefix"};

/// Creates the Store: PUT, overwrite, ERASE and TTL history over S1..S3 (sealed) and S4 (active), including
/// tombstones and expired puts inside the oldest two Segments and after them (ADR 0044 closure cases).
void seed(const std::filesystem::path& data_dir);

/// Opens the seeded Store with `hooks` and runs one compaction whose copy budget selects exactly the oldest
/// two sealed Segments (k = 2 of n = 3, one output).
void run_worker(const std::filesystem::path& data_dir, const FilesystemHooks& hooks);

/// Reopens twice after the worker was killed at `boundary`. Requires exactly one of the two embedded
/// authorities (the old one until the next Manifest is renamed), a recovery-safe namespace after the first
/// open and a clean one after the second, the complete sequential model each time, the exact files of that
/// authority, and, when seeded in this process, the sealed Segment after the prefix byte-for-byte untouched.
/// Explains any violation on stderr and returns false.
[[nodiscard]] auto verify(const std::filesystem::path& data_dir, std::string_view boundary) -> bool;

/// The persistent transitions of the transaction, named as in `FilesystemOperation` with their occurrence.
[[nodiscard]] auto boundaries() -> std::vector<std::string>;

} // namespace glifistore::crash::prefix_scenario
