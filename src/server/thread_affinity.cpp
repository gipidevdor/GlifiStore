#include "glifistore/server/thread_affinity.hpp"

#include <climits>
#include <cstdio>
#include <pthread.h>

#if defined(__OpenBSD__) || defined(__FreeBSD__)
#include <pthread_np.h>
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace glifistore::server {
namespace {

void set_thread_name(const std::size_t executor_id) noexcept {
    char name[16]{};
    static_cast<void>(std::snprintf(name, sizeof(name), "glifi-%zu", executor_id));
#if defined(__APPLE__)
    static_cast<void>(pthread_setname_np(name));
#elif defined(__linux__)
    static_cast<void>(pthread_setname_np(pthread_self(), name));
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), name);
#endif
}

} // namespace

auto configure_executor_thread(const std::size_t executor_id, const bool affinity_requested) noexcept
    -> ExecutorAffinityResult {
    set_thread_name(executor_id);
    if (!affinity_requested) {
        return {};
    }
#if defined(__linux__)
    cpu_set_t allowed{};
    CPU_ZERO(&allowed);
    if (::sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        return {.mode = ExecutorAffinityMode::unavailable};
    }
    std::size_t available_index{};
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(static_cast<unsigned>(cpu), &allowed)) {
            continue;
        }
        if (available_index++ != executor_id) {
            continue;
        }
        cpu_set_t selected{};
        CPU_ZERO(&selected);
        CPU_SET(static_cast<unsigned>(cpu), &selected);
        if (::pthread_setaffinity_np(pthread_self(), sizeof(selected), &selected) != 0) {
            return {.mode = ExecutorAffinityMode::unavailable};
        }
        return {.mode = ExecutorAffinityMode::pinned, .cpu = cpu};
    }
    return {.mode = ExecutorAffinityMode::unavailable};
#elif defined(__APPLE__)
    thread_affinity_policy_data_t policy{
        static_cast<integer_t>(executor_id % static_cast<std::size_t>(INT_MAX) + 1U)};
    const auto result =
        ::thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_AFFINITY_POLICY,
                            reinterpret_cast<thread_policy_t>(&policy), THREAD_AFFINITY_POLICY_COUNT);
    return {.mode =
                result == KERN_SUCCESS ? ExecutorAffinityMode::advisory : ExecutorAffinityMode::unavailable};
#else
    return {.mode = ExecutorAffinityMode::unavailable};
#endif
}

auto affinity_mode_name(const ExecutorAffinityMode mode) noexcept -> std::string_view {
    switch (mode) {
    case ExecutorAffinityMode::disabled:
        return "disabled";
    case ExecutorAffinityMode::unavailable:
        return "unavailable";
    case ExecutorAffinityMode::advisory:
        return "advisory";
    case ExecutorAffinityMode::pinned:
        return "pinned";
    }
    return "unknown";
}

} // namespace glifistore::server
