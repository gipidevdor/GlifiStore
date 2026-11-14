/**
 * @file
 * @brief Versioned owner-routing identity shared by persisted catalogs and wire bootstrap.
 */
#pragma once

#include "glifistore/core/error.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace glifistore {

/**
 * @brief Persisted/wire routing identifiers; numeric values are compatibility data.
 */
enum class RoutingAlgorithm : std::uint32_t {
    fnv1a64_v1 = 1,
    siphash24_v1 = 2,
};

inline constexpr std::uint64_t kDefaultWorkerHashSeed = 0;
inline constexpr std::uint64_t kWorkerRoutingSipKey1Xor = 0x6a09e667f3bcc909ULL;
inline constexpr std::string_view kWireProtocolIdentity = "GlifiStore/2";
inline constexpr std::size_t kWireInitIdentityExtendedBytes =
    kWireProtocolIdentity.size() + 1U + sizeof(std::uint32_t) + sizeof(std::uint64_t);

/**
 * @brief Versioned owner-routing parameters; persisted identity must match the state used to hash keys.
 */
struct WorkerRoutingState {
    RoutingAlgorithm algorithm{RoutingAlgorithm::fnv1a64_v1};
    std::uint64_t seed{kDefaultWorkerHashSeed};

    [[nodiscard]] auto keyed() const noexcept -> bool {
        return algorithm == RoutingAlgorithm::siphash24_v1;
    }

    auto operator==(const WorkerRoutingState&) const -> bool = default;
};

/**
 * @brief Update process-default owner routing; does not reshard an already opened Store.
 */
void set_worker_routing(RoutingAlgorithm algorithm, std::uint64_t seed) noexcept;
/**
 * @brief Publish one consistent process-default algorithm/seed pair.
 */
void set_worker_routing(WorkerRoutingState state) noexcept;
/**
 * @brief Read a consistent process-default routing pair.
 */
[[nodiscard]] auto get_worker_routing() noexcept -> WorkerRoutingState;
/**
 * @brief Obtain a nonzero routing seed from platform entropy, with an address-mixed fallback.
 * @note The fallback does not provide a cryptographic entropy guarantee.
 */
[[nodiscard]] auto generate_worker_hash_seed() noexcept -> std::uint64_t;

/**
 * @brief Hash binary key bytes using a snapshot of the process-default routing identity.
 */
[[nodiscard]] auto hash_key_routing(std::span<const std::byte> key) noexcept -> std::uint64_t;
/**
 * @brief Hash binary key bytes using a snapshot of the process-default routing identity.
 */
[[nodiscard]] auto hash_key_routing(std::string_view key) noexcept -> std::uint64_t;
/**
 * @brief Hash binary key bytes with explicit immutable routing state, independent of process defaults.
 */
[[nodiscard]] auto hash_key_routing(std::span<const std::byte> key, WorkerRoutingState state) noexcept
    -> std::uint64_t;
/**
 * @brief Hash binary key bytes with explicit immutable routing state, independent of process defaults.
 */
[[nodiscard]] auto hash_key_routing(std::string_view key, WorkerRoutingState state) noexcept -> std::uint64_t;

/**
 * @brief Encode the wire bootstrap identity and versioned routing extension into owning bytes.
 */
[[nodiscard]] auto encode_init_identity_value(WorkerRoutingState state) -> std::vector<std::byte>;
/**
 * @brief Validate/decode the bootstrap identity and routing metadata; input is borrowed only for the call.
 */
[[nodiscard]] auto decode_init_identity_value(std::span<const std::byte> value) -> Result<WorkerRoutingState>;
/**
 * @brief Accept supported algorithms; FNV routing requires the fixed zero seed.
 */
[[nodiscard]] auto validate_worker_routing_state(WorkerRoutingState state) -> Status;

} // namespace glifistore
