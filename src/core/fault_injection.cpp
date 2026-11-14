#include "glifistore/core/fault_injection.hpp"

#if defined(GLIFISTORE_FAULT_INJECTION)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace glifistore::fault {
namespace {

constexpr std::size_t kPendingFailSlots = 4;

struct PendingFail final {
    std::atomic<std::uint8_t> site{0};
    std::atomic<std::uint32_t> remaining{0};
};

struct BlockState final {
    std::mutex mutex;
    std::condition_variable condition;
    std::uint8_t site{0};
    bool armed{};
    bool blocked{};
    bool released{};
};

struct Config final {
    std::atomic<std::uint64_t> rng{0xC0FFEEULL};
    std::atomic<std::uint32_t> yield_percent{25};
    std::atomic<std::uint32_t> sleep_us_max{50};
    PendingFail pending[kPendingFailSlots]{};
    BlockState block{};
};

Config& config() noexcept {
    static Config instance;
    return instance;
}

[[nodiscard]] auto next_u32() noexcept -> std::uint32_t {
    auto& state = config().rng;
    auto x = state.load(std::memory_order_relaxed);
    for (;;) {
        auto y = x;
        y ^= y >> 12;
        y ^= y << 25;
        y ^= y >> 27;
        const auto next = y * 0x2545F4914F6CDD1DULL;
        if (state.compare_exchange_weak(x, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return static_cast<std::uint32_t>(next >> 32);
        }
    }
}

void clear_all_pending() noexcept {
    for (auto& slot : config().pending) {
        slot.remaining.store(0, std::memory_order_relaxed);
        slot.site.store(0, std::memory_order_release);
    }
}

void clear_block() noexcept {
    auto& block = config().block;
    {
        const std::lock_guard lock{block.mutex};
        block.site = 0;
        block.armed = false;
        block.blocked = false;
        block.released = false;
    }
    block.condition.notify_all();
}

} // namespace

void configure(const std::uint64_t seed, const std::uint32_t yield_percent,
               const std::uint32_t sleep_us_max) noexcept {
    config().rng.store(seed == 0 ? 0xC0FFEEULL : seed, std::memory_order_relaxed);
    config().yield_percent.store(yield_percent > 100 ? 100 : yield_percent, std::memory_order_relaxed);
    config().sleep_us_max.store(sleep_us_max, std::memory_order_relaxed);
}

void reset() noexcept {
    configure(0xC0FFEEULL, 25, 50);
    clear_all_pending();
    clear_block();
}

void fail_nth(const Site site, const std::uint32_t n) noexcept {
    const auto site_id = static_cast<std::uint8_t>(site);
    if (n == 0) {
        for (auto& slot : config().pending) {
            if (slot.site.load(std::memory_order_acquire) == site_id) {
                slot.remaining.store(0, std::memory_order_relaxed);
                slot.site.store(0, std::memory_order_release);
            }
        }
        return;
    }
    // Prefer an existing slot for this site, else an empty slot, else overwrite slot 0.
    // Distinct sites may be armed concurrently (up to kPendingFailSlots).
    PendingFail* target = nullptr;
    PendingFail* empty = nullptr;
    for (auto& slot : config().pending) {
        const auto occupied = slot.site.load(std::memory_order_acquire);
        if (occupied == site_id) {
            target = &slot;
            break;
        }
        if (empty == nullptr && occupied == 0) {
            empty = &slot;
        }
    }
    if (target == nullptr) {
        target = empty != nullptr ? empty : &config().pending[0];
    }
    target->remaining.store(n, std::memory_order_relaxed);
    target->site.store(site_id, std::memory_order_release);
}

void fail_once(const Site site) noexcept {
    fail_nth(site, 1);
}

auto consume_fail(const Site site) noexcept -> bool {
    const auto site_id = static_cast<std::uint8_t>(site);
    for (auto& slot : config().pending) {
        if (slot.site.load(std::memory_order_acquire) != site_id) {
            continue;
        }
        auto remaining = slot.remaining.load(std::memory_order_relaxed);
        while (remaining > 0) {
            if (slot.site.load(std::memory_order_acquire) != site_id) {
                break;
            }
            if (slot.remaining.compare_exchange_weak(remaining, remaining - 1, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
                if (remaining == 1) {
                    slot.site.store(0, std::memory_order_release);
                    return true;
                }
                return false;
            }
        }
    }
    return false;
}

void arm_block(const Site site) noexcept {
    auto& block = config().block;
    {
        const std::lock_guard lock{block.mutex};
        block.site = static_cast<std::uint8_t>(site);
        block.armed = true;
        block.blocked = false;
        block.released = false;
    }
    block.condition.notify_all();
}

void release_block(const Site site) noexcept {
    auto& block = config().block;
    {
        const std::lock_guard lock{block.mutex};
        if (block.site != static_cast<std::uint8_t>(site)) {
            return;
        }
        block.released = true;
        block.armed = false;
    }
    block.condition.notify_all();
}

auto wait_until_blocked(const Site site, const std::chrono::milliseconds timeout) -> bool {
    auto& block = config().block;
    std::unique_lock lock{block.mutex};
    return block.condition.wait_for(
        lock, timeout, [&] { return block.blocked && block.site == static_cast<std::uint8_t>(site); });
}

void maybe_block(const Site site) noexcept {
    auto& block = config().block;
    std::unique_lock lock{block.mutex};
    if (!block.armed || block.site != static_cast<std::uint8_t>(site)) {
        return;
    }
    block.blocked = true;
    block.condition.notify_all();
    block.condition.wait(lock, [&] { return block.released; });
    block.blocked = false;
}

void at(const Site) noexcept {
    const auto roll = next_u32() % 100U;
    const auto yield_percent = config().yield_percent.load(std::memory_order_relaxed);
    if (roll >= yield_percent) {
        return;
    }
    std::this_thread::yield();
    const auto sleep_max = config().sleep_us_max.load(std::memory_order_relaxed);
    if (sleep_max == 0) {
        return;
    }
    const auto delay = next_u32() % (sleep_max + 1U);
    if (delay != 0) {
        std::this_thread::sleep_for(std::chrono::microseconds{delay});
    }
}

} // namespace glifistore::fault

#endif
