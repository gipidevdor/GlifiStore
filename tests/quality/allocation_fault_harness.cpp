#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/index/index.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/global_manager.hpp"
#include "glifistore/segment/record.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/store/store.hpp"
#include "store/store_internal.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace allocation_fault {

struct ThreadState {
    std::size_t fail_at{std::numeric_limits<std::size_t>::max()};
    std::size_t observed{};
    bool armed{};
    bool fired{};
};

struct Observation {
    std::size_t observed{};
    bool fired{};
};

constinit thread_local ThreadState thread_state;
constinit std::atomic_bool forbid_all{};
constinit std::atomic_bool forbidden_allocation_observed{};
// Cross-thread arm for paired Writer work (Writer runs on a dedicated thread).
constinit std::atomic_size_t process_fail_at{std::numeric_limits<std::size_t>::max()};
constinit std::atomic_size_t process_observed{};

void arm(const std::size_t fail_at) noexcept {
    thread_state = {.fail_at = fail_at, .observed = 0, .armed = true, .fired = false};
}

void arm_process(const std::size_t fail_at) noexcept {
    process_observed.store(0, std::memory_order_relaxed);
    process_fail_at.store(fail_at, std::memory_order_release);
}

void disarm_process() noexcept {
    process_fail_at.store(std::numeric_limits<std::size_t>::max(), std::memory_order_release);
}

auto disarm() noexcept -> Observation {
    const Observation observation{.observed = thread_state.observed, .fired = thread_state.fired};
    thread_state = {};
    return observation;
}

void begin_forbid_all() noexcept {
    forbidden_allocation_observed.store(false, std::memory_order_relaxed);
    forbid_all.store(true, std::memory_order_release);
}

auto end_forbid_all() noexcept -> bool {
    forbid_all.store(false, std::memory_order_release);
    return forbidden_allocation_observed.load(std::memory_order_acquire);
}

auto should_fail() noexcept -> bool {
    if (forbid_all.load(std::memory_order_acquire)) {
        forbidden_allocation_observed.store(true, std::memory_order_release);
        return true;
    }
    const auto process_at = process_fail_at.load(std::memory_order_acquire);
    if (process_at != std::numeric_limits<std::size_t>::max()) {
        const auto allocation = process_observed.fetch_add(1, std::memory_order_relaxed);
        if (allocation == process_at) {
            return true;
        }
    }
    if (!thread_state.armed) {
        return false;
    }
    const auto allocation = thread_state.observed++;
    if (!thread_state.fired && allocation == thread_state.fail_at) {
        thread_state.fired = true;
        return true;
    }
    return false;
}

auto allocate(const std::size_t requested) -> void* {
    if (should_fail()) {
        throw std::bad_alloc{};
    }
    const auto size = requested == 0 ? std::size_t{1} : requested;
    if (void* memory = std::malloc(size); memory != nullptr) {
        return memory;
    }
    throw std::bad_alloc{};
}

auto allocate_aligned(const std::size_t requested, const std::size_t alignment) -> void* {
    if (should_fail()) {
        throw std::bad_alloc{};
    }
    void* memory{};
    const auto size = requested == 0 ? std::size_t{1} : requested;
    if (::posix_memalign(&memory, alignment, size) == 0 && memory != nullptr) {
        return memory;
    }
    throw std::bad_alloc{};
}

} // namespace allocation_fault

void* operator new(const std::size_t size) {
    return allocation_fault::allocate(size);
}

void* operator new[](const std::size_t size) {
    return allocation_fault::allocate(size);
}

void* operator new(const std::size_t size, const std::align_val_t alignment) {
    return allocation_fault::allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](const std::size_t size, const std::align_val_t alignment) {
    return allocation_fault::allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocation_fault::allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocation_fault::allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new(const std::size_t size, const std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return allocation_fault::allocate_aligned(size, static_cast<std::size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](const std::size_t size, const std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    try {
        return allocation_fault::allocate_aligned(size, static_cast<std::size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

#if defined(__cpp_sized_deallocation)
// With sized deallocation disabled, Clang treats these as placement deletes;
// declaring them can break libstdc++'s __builtin_operator_delete calls.
// Keep by-value parameters unqualified: Clang's builtin also rejects const sizes.
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}
#endif

void operator delete(void* memory, std::align_val_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept {
    std::free(memory);
}

#if defined(__cpp_sized_deallocation)
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}
#endif

void operator delete(void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
    std::free(memory);
}
