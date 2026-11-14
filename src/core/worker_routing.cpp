#include "glifistore/core/worker_routing.hpp"

#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/little_endian.hpp"

#include <atomic>
#include <cstring>
#include <exception>
#include <limits>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/random.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
#include <unistd.h>
#endif

namespace glifistore {
namespace {

std::atomic<std::uint32_t> g_worker_routing_algorithm{
    static_cast<std::uint32_t>(RoutingAlgorithm::fnv1a64_v1)};
std::atomic<std::uint64_t> g_worker_hash_seed{kDefaultWorkerHashSeed};
std::atomic_uint64_t g_worker_routing_revision{};
std::atomic_flag g_worker_routing_writer = ATOMIC_FLAG_INIT;

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

void put_u32(std::vector<std::byte>& out, const std::uint32_t value) {
    const auto at = out.size();
    out.resize(at + 4U);
    le::put_u32(std::span<std::byte>{out}, at, value);
}

void put_u64(std::vector<std::byte>& out, const std::uint64_t value) {
    const auto at = out.size();
    out.resize(at + 8U);
    le::put_u64(std::span<std::byte>{out}, at, value);
}

auto get_u32(const std::span<const std::byte> in, const std::size_t at) -> std::uint32_t {
    return le::get_u32(in, at);
}

auto get_u64(const std::span<const std::byte> in, const std::size_t at) -> std::uint64_t {
    return le::get_u64(in, at);
}

} // namespace

auto validate_worker_routing_state(const WorkerRoutingState state) -> Status {
    if (state.algorithm == RoutingAlgorithm::fnv1a64_v1) {
        if (state.seed != kDefaultWorkerHashSeed) {
            return fail(ErrorCode::invalid_argument, "fnv1a64-v1 Worker routing requires a zero hash seed");
        }
        return {};
    }
    if (state.algorithm == RoutingAlgorithm::siphash24_v1) {
        return {};
    }
    return fail(ErrorCode::invalid_argument, "unsupported Worker routing algorithm");
}

void set_worker_routing(const RoutingAlgorithm algorithm, const std::uint64_t seed) noexcept {
    set_worker_routing(WorkerRoutingState{.algorithm = algorithm, .seed = seed});
}

void set_worker_routing(const WorkerRoutingState state) noexcept {
    while (g_worker_routing_writer.test_and_set(std::memory_order_acquire)) {
        g_worker_routing_writer.wait(true, std::memory_order_relaxed);
    }
    const auto revision = g_worker_routing_revision.load(std::memory_order_seq_cst);
    if ((revision & 1U) != 0U || revision > std::numeric_limits<std::uint64_t>::max() - 2U) [[unlikely]] {
        std::terminate();
    }
    g_worker_routing_revision.store(revision + 1U, std::memory_order_seq_cst);
    g_worker_routing_algorithm.store(static_cast<std::uint32_t>(state.algorithm), std::memory_order_seq_cst);
    g_worker_hash_seed.store(state.seed, std::memory_order_seq_cst);
    g_worker_routing_revision.store(revision + 2U, std::memory_order_seq_cst);
    g_worker_routing_revision.notify_all();
    g_worker_routing_writer.clear(std::memory_order_release);
    g_worker_routing_writer.notify_one();
}

auto get_worker_routing() noexcept -> WorkerRoutingState {
    for (;;) {
        const auto before = g_worker_routing_revision.load(std::memory_order_seq_cst);
        if ((before & 1U) != 0U) {
            g_worker_routing_revision.wait(before, std::memory_order_acquire);
            continue;
        }
        const WorkerRoutingState state{
            .algorithm =
                static_cast<RoutingAlgorithm>(g_worker_routing_algorithm.load(std::memory_order_seq_cst)),
            .seed = g_worker_hash_seed.load(std::memory_order_seq_cst),
        };
        if (g_worker_routing_revision.load(std::memory_order_seq_cst) == before) {
            return state;
        }
    }
}

auto generate_worker_hash_seed() noexcept -> std::uint64_t {
    std::uint64_t seed = 0;
    if (fill_entropy(&seed, sizeof(seed)) && seed != 0) {
        return seed;
    }
    seed = 0xC3A5C85C97CB3127ULL;
    seed ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&g_worker_hash_seed));
    return seed == 0 ? 1U : seed;
}

auto hash_key_routing(const std::span<const std::byte> key, const WorkerRoutingState state) noexcept
    -> std::uint64_t {
    if (state.algorithm == RoutingAlgorithm::siphash24_v1) {
        return siphash24(key, state.seed, state.seed ^ kWorkerRoutingSipKey1Xor);
    }
    return hash_key(key);
}

auto hash_key_routing(const std::string_view key, const WorkerRoutingState state) noexcept -> std::uint64_t {
    return hash_key_routing({reinterpret_cast<const std::byte*>(key.data()), key.size()}, state);
}

auto hash_key_routing(const std::span<const std::byte> key) noexcept -> std::uint64_t {
    return hash_key_routing(key, get_worker_routing());
}

auto hash_key_routing(const std::string_view key) noexcept -> std::uint64_t {
    return hash_key_routing(key, get_worker_routing());
}

auto encode_init_identity_value(const WorkerRoutingState state) -> std::vector<std::byte> {
    std::vector<std::byte> value;
    value.reserve(state.keyed() ? kWireInitIdentityExtendedBytes : kWireProtocolIdentity.size());
    for (const char character : kWireProtocolIdentity) {
        value.push_back(static_cast<std::byte>(character));
    }
    if (!state.keyed()) {
        return value;
    }
    value.push_back(std::byte{0});
    put_u32(value, static_cast<std::uint32_t>(state.algorithm));
    put_u64(value, state.seed);
    return value;
}

auto decode_init_identity_value(const std::span<const std::byte> value) -> Result<WorkerRoutingState> {
    if (value.size() == kWireProtocolIdentity.size() &&
        std::memcmp(value.data(), kWireProtocolIdentity.data(), kWireProtocolIdentity.size()) == 0) {
        return WorkerRoutingState{};
    }
    if (value.size() != kWireInitIdentityExtendedBytes) {
        return fail(ErrorCode::corrupted_data, "server INIT identity value has unexpected length");
    }
    if (std::memcmp(value.data(), kWireProtocolIdentity.data(), kWireProtocolIdentity.size()) != 0 ||
        value[kWireProtocolIdentity.size()] != std::byte{0}) {
        return fail(ErrorCode::corrupted_data, "server INIT identity prefix is invalid");
    }
    const auto algorithm = static_cast<RoutingAlgorithm>(get_u32(value, kWireProtocolIdentity.size() + 1U));
    const auto seed = get_u64(value, kWireProtocolIdentity.size() + 1U + sizeof(std::uint32_t));
    WorkerRoutingState state{.algorithm = algorithm, .seed = seed};
    if (auto valid = validate_worker_routing_state(state); !valid) {
        return unexpected(valid.error());
    }
    if (!state.keyed()) {
        return fail(ErrorCode::corrupted_data, "server INIT extended identity must use siphash24-v1 routing");
    }
    return state;
}

} // namespace glifistore
