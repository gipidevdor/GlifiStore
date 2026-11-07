/**
 * @file
 * @brief Key hashing and owner selection; routing and Index placement have distinct seeds.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace glifistore {

/// Pure FNV-1a-64. Ownership uses hash_key_routing (ADR 0030) which may be SipHash.
/// Index placement uses a separate process mix seed (ADR 0026).
inline constexpr std::uint64_t kFnv1a64Offset = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ULL;

/**
 * @brief Compute pure FNV-1a-64 over binary bytes; this is not keyed owner routing.
 */
[[nodiscard]] inline auto hash_key(std::span<const std::byte> key) noexcept -> std::uint64_t {
    std::uint64_t hash = kFnv1a64Offset;
    for (const auto byte : key) {
        hash ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(byte));
        hash *= kFnv1a64Prime;
    }
    return hash;
}

/**
 * @brief Binary-safe string-view overload of pure FNV-1a-64.
 */
[[nodiscard]] inline auto hash_key(std::string_view key) noexcept -> std::uint64_t {
    return hash_key({reinterpret_cast<const std::byte*>(key.data()), key.size()});
}

/// SipHash-2-4 (Aumasson/Bernstein). Independent implementation for siphash24-v1 routing
/// (ADR 0030). Attribution: THIRD_PARTY_NOTICES.md. Not a copy of a third-party source tree.
[[nodiscard]] inline auto siphash24(std::span<const std::byte> key, std::uint64_t k0,
                                    std::uint64_t k1) noexcept -> std::uint64_t {
    auto rotl = [](std::uint64_t value, unsigned shift) noexcept -> std::uint64_t {
        return (value << shift) | (value >> (64U - shift));
    };
    auto sipround = [&](std::uint64_t& v0, std::uint64_t& v1, std::uint64_t& v2, std::uint64_t& v3) noexcept {
        v0 += v1;
        v1 = rotl(v1, 13);
        v1 ^= v0;
        v0 = rotl(v0, 32);
        v2 += v3;
        v3 = rotl(v3, 16);
        v3 ^= v2;
        v0 += v3;
        v3 = rotl(v3, 21);
        v3 ^= v0;
        v2 += v1;
        v1 = rotl(v1, 17);
        v1 ^= v2;
        v2 = rotl(v2, 32);
    };

    std::uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
    std::uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
    std::uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
    std::uint64_t v3 = k1 ^ 0x7465646279746573ULL;

    const auto* bytes = reinterpret_cast<const unsigned char*>(key.data());
    const auto length = key.size();
    std::size_t offset = 0;
    while (offset + 8 <= length) {
        std::uint64_t message = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            message |= static_cast<std::uint64_t>(bytes[offset + i]) << (8U * i);
        }
        v3 ^= message;
        sipround(v0, v1, v2, v3);
        sipround(v0, v1, v2, v3);
        v0 ^= message;
        offset += 8;
    }

    std::uint64_t message = static_cast<std::uint64_t>(length) << 56U;
    for (std::size_t i = 0; offset + i < length; ++i) {
        message |= static_cast<std::uint64_t>(bytes[offset + i]) << (8U * i);
    }
    v3 ^= message;
    sipround(v0, v1, v2, v3);
    sipround(v0, v1, v2, v3);
    v0 ^= message;

    v2 ^= 0xffU;
    sipround(v0, v1, v2, v3);
    sipround(v0, v1, v2, v3);
    sipround(v0, v1, v2, v3);
    sipround(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

/**
 * @brief Compute SipHash-2-4 using the supplied two-word key.
 */
[[nodiscard]] inline auto hash_key_keyed(std::span<const std::byte> key, std::uint64_t k0,
                                         std::uint64_t k1) noexcept -> std::uint64_t {
    return siphash24(key, k0, k1);
}

/**
 * @brief Binary-safe string-view overload of keyed SipHash-2-4.
 */
[[nodiscard]] inline auto hash_key_keyed(std::string_view key, std::uint64_t k0, std::uint64_t k1) noexcept
    -> std::uint64_t {
    return hash_key_keyed({reinterpret_cast<const std::byte*>(key.data()), key.size()}, k0, k1);
}

/**
 * @brief Reduce an already computed owner hash modulo worker_count.
 * @pre worker_count > 0.
 */
[[nodiscard]] inline auto route_worker(std::uint64_t key_hash, std::size_t worker_count) noexcept
    -> std::size_t {
    return static_cast<std::size_t>(key_hash % worker_count);
}

} // namespace glifistore

#include "glifistore/core/worker_routing.hpp"

namespace glifistore {

/**
 * @brief Route using the process-default owner hash.
 * @pre worker_count > 0; use explicit Store routing when a Store has its own identity.
 */
[[nodiscard]] inline auto route_worker(std::string_view key, std::size_t worker_count) noexcept
    -> std::size_t {
    return route_worker(hash_key_routing(key), worker_count);
}

/**
 * @brief Borrowed binary key plus precomputed owner-routing hash; the source key must outlive its use.
 */
struct HashedKey {
    std::string_view key{};
    std::uint64_t hash{};

    [[nodiscard]] static auto compute(std::string_view key) noexcept -> HashedKey {
        return HashedKey{key, hash_key_routing(key)};
    }
};

/// Alias used by Store hot-path helpers: key bytes plus a routing/index hash computed once.
using PrehashedKey = HashedKey;
using KeyView = std::string_view;

} // namespace glifistore
