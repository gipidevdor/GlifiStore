#include "glifistore/index/index_hash_seed.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/random.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
#include <unistd.h>
#endif

namespace glifistore {
namespace {

std::atomic<std::uint64_t> g_index_hash_seed{kDefaultIndexHashSeed};

[[nodiscard]] auto fill_entropy(void* buffer, const std::size_t length) noexcept -> bool {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    return ::getentropy(buffer, length) == 0;
#elif defined(__linux__)
    auto* out = static_cast<std::uint8_t*>(buffer);
    std::size_t filled = 0;
    while (filled < length) {
        const auto got = ::getrandom(out + filled, length - filled, 0);
        if (got < 0) {
            return false;
        }
        filled += static_cast<std::size_t>(got);
    }
    return true;
#else
    static_cast<void>(buffer);
    static_cast<void>(length);
    return false;
#endif
}

} // namespace

void set_index_hash_seed(const std::uint64_t seed) noexcept {
    g_index_hash_seed.store(seed, std::memory_order_relaxed);
}

auto get_index_hash_seed() noexcept -> std::uint64_t {
    return g_index_hash_seed.load(std::memory_order_relaxed);
}

auto generate_index_hash_seed() noexcept -> std::uint64_t {
    std::uint64_t seed = 0;
    if (fill_entropy(&seed, sizeof(seed)) && seed != 0) {
        return seed;
    }
    // Fail soft only for the generator: mix a non-crypto fallback so we still
    // leave the published Index v1 constant when entropy is unavailable.
    seed = kDefaultIndexHashSeed ^ 0xA5A5A5A5A5A5A5A5ULL;
    seed ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&g_index_hash_seed));
    return seed == 0 ? 1U : seed;
}

} // namespace glifistore
