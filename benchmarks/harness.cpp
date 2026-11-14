#include "harness.hpp"

#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#elif defined(__linux__)
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <limits>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace glifistore::bench {

namespace {
bool g_cpu_pin_applied = false;
} // namespace

[[nodiscard]] auto cpu_pin_applied() noexcept -> bool {
    return g_cpu_pin_applied;
}

bool try_cpu_pin(const bool requested) {
    g_cpu_pin_applied = false;
    if (!requested) {
        return false;
    }
#if defined(__linux__)
    cpu_set_t cpuset{};
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0) {
        g_cpu_pin_applied = true;
        return true;
    }
    return false;
#else
    (void)requested;
    return false;
#endif
}

auto process_memory_snapshot() noexcept -> ResourceSample {
#if defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    const auto status =
        task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count);
    if (status != KERN_SUCCESS) {
        return {};
    }
    const auto resident = static_cast<std::size_t>(info.resident_size);
    const auto physical_peak =
        info.ledger_phys_footprint_peak > 0 ? static_cast<std::size_t>(info.ledger_phys_footprint_peak) : 0U;
    return {.rss_before_bytes = resident,
            .rss_after_bytes = resident,
            .peak_rss_bytes = static_cast<std::size_t>(info.resident_size_peak),
            .physical_footprint_bytes = static_cast<std::size_t>(info.phys_footprint),
            .peak_physical_footprint_bytes = physical_peak,
            .reusable_bytes = static_cast<std::size_t>(info.reusable),
            .internal_bytes = static_cast<std::size_t>(info.internal),
            .compressed_bytes = static_cast<std::size_t>(info.compressed)};
#elif defined(__linux__)
    std::size_t resident{};
    if (auto* statm = std::fopen("/proc/self/statm", "r"); statm != nullptr) {
        unsigned long ignored_pages{};
        unsigned long resident_pages{};
        if (std::fscanf(statm, "%lu %lu", &ignored_pages, &resident_pages) == 2) {
            const auto page_size = ::sysconf(_SC_PAGESIZE);
            if (page_size > 0) {
                resident = static_cast<std::size_t>(resident_pages) * static_cast<std::size_t>(page_size);
            }
        }
        static_cast<void>(std::fclose(statm));
    }
    rusage usage{};
    const auto peak =
        ::getrusage(RUSAGE_SELF, &usage) == 0 ? static_cast<std::size_t>(usage.ru_maxrss) * 1024U : 0U;
    return {.rss_before_bytes = resident,
            .rss_after_bytes = resident,
            .peak_rss_bytes = peak,
            .physical_footprint_bytes = resident,
            .peak_physical_footprint_bytes = peak};
#else
    return {};
#endif
}

auto allocator_memory_snapshot() noexcept -> AllocatorMemorySample {
#if defined(__APPLE__)
    malloc_statistics_t statistics{};
    // The SDK contract explicitly defines nullptr as the aggregate of all
    // zones; the default zone alone can omit allocator-private subzones.
    malloc_zone_statistics(nullptr, &statistics);
    return {.available = true,
            .blocks_in_use = static_cast<std::size_t>(statistics.blocks_in_use),
            .bytes_in_use = static_cast<std::size_t>(statistics.size_in_use),
            .peak_bytes_in_use = static_cast<std::size_t>(statistics.max_size_in_use),
            .bytes_reserved = static_cast<std::size_t>(statistics.size_allocated)};
#elif defined(__linux__) && defined(__GLIBC__)
    const auto statistics = ::mallinfo2();
    const auto arena_bytes = static_cast<std::size_t>(statistics.arena);
    const auto mapped_bytes = static_cast<std::size_t>(statistics.hblkhd);
    return {.available = true,
            .bytes_in_use = static_cast<std::size_t>(statistics.uordblks),
            .bytes_reserved = arena_bytes > std::numeric_limits<std::size_t>::max() - mapped_bytes
                                  ? std::numeric_limits<std::size_t>::max()
                                  : arena_bytes + mapped_bytes};
#else
    return {};
#endif
}

auto allocator_pressure_relief() noexcept -> AllocatorPressureReliefSample {
#if defined(__APPLE__)
    const auto released = malloc_zone_pressure_relief(nullptr, 0);
    return {.available = true,
            .released = released != 0,
            .exact_released_bytes = true,
            .released_bytes = released};
#elif defined(__linux__) && defined(__GLIBC__)
    // glibc reports only whether pages were released, not their exact count.
    return {.available = true, .released = ::malloc_trim(0) != 0};
#else
    return {};
#endif
}

} // namespace glifistore::bench
