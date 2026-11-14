/**
 * @file
 * @brief Process-local Index placement seed, independent of persisted owner routing.
 */
#pragma once

#include <cstdint>

namespace glifistore {

/// Default SwissTable mix seed (Index v1 / ADR 0007). See ADR 0026 for keyed
/// process-lifetime override used as Phase 8 hash-flood resistance.
inline constexpr std::uint64_t kDefaultIndexHashSeed = 0x243F6A8885A308D3ULL;

/// Process-wide Index placement seed. Must be set before the first Index /
/// SwissTableIndex construction in the process (daemon: before Store::open).
/// Changing the seed after Indexes exist is unsupported. Index state is
/// rebuildable (ADR 0004); the seed is not persisted.
void set_index_hash_seed(std::uint64_t seed) noexcept;
[[nodiscard]] auto get_index_hash_seed() noexcept -> std::uint64_t;

/// Best-effort OS entropy for secure-profile default seed generation.
[[nodiscard]] auto generate_index_hash_seed() noexcept -> std::uint64_t;

} // namespace glifistore
