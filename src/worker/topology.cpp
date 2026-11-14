#include "glifistore/worker/topology.hpp"

#include "glifistore/core/types.hpp"

#include <algorithm>
#include <thread>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

#if defined(__linux__)
#include <sys/sysinfo.h>
#endif

#if defined(__linux__) || defined(__OpenBSD__)
#include <unistd.h>
#endif

namespace glifistore {

static_assert(kDefaultMinimumMemoryPerWorker == kSegmentSizeBytes);

auto WorkerCountPolicy::choose(const WorkerTopology& topology, const WorkerCountConfig& config) noexcept
    -> std::size_t {
    if (config.explicit_count.has_value()) {
        return std::clamp(*config.explicit_count, std::size_t{1},
                          std::max(std::size_t{1}, config.maximum_workers));
    }
    const auto cores = std::max(std::size_t{1}, std::min(topology.physical_cores, topology.available_cpus));
    const auto after_reservation =
        cores > config.reserved_cores ? cores - config.reserved_cores : std::size_t{1};
    auto count = std::min(after_reservation, std::max(std::size_t{1}, config.maximum_workers));
    if (topology.available_memory_bytes != 0 && config.minimum_memory_per_worker != 0) {
        const auto memory_limited =
            std::max(std::size_t{1}, topology.available_memory_bytes / config.minimum_memory_per_worker);
        count = std::min(count, memory_limited);
    }
    return std::max(std::size_t{1}, count);
}

auto detect_worker_topology() noexcept -> WorkerTopology {
    WorkerTopology result{};
    result.logical_cpus = std::max(1U, std::thread::hardware_concurrency());
    result.physical_cores = result.logical_cpus;
    result.available_cpus = result.logical_cpus;

#if defined(__APPLE__)
    std::size_t size = sizeof(result.physical_cores);
    static_cast<void>(sysctlbyname("hw.physicalcpu", &result.physical_cores, &size, nullptr, 0));
    std::uint64_t memory{};
    size = sizeof(memory);
    if (sysctlbyname("hw.memsize", &memory, &size, nullptr, 0) == 0) {
        result.available_memory_bytes = static_cast<std::size_t>(memory);
    }
#elif defined(__FreeBSD__)
    std::size_t size = sizeof(result.physical_cores);
    static_cast<void>(sysctlbyname("kern.smp.cores", &result.physical_cores, &size, nullptr, 0));
    std::uint64_t memory{};
    size = sizeof(memory);
    if (sysctlbyname("hw.physmem", &memory, &size, nullptr, 0) == 0) {
        result.available_memory_bytes = static_cast<std::size_t>(memory);
    }
#elif defined(__OpenBSD__)
    const auto available = sysconf(_SC_NPROCESSORS_ONLN);
    if (available > 0) {
        result.available_cpus = static_cast<std::size_t>(available);
    }
#elif defined(__linux__)
    const auto available = sysconf(_SC_NPROCESSORS_ONLN);
    if (available > 0) {
        result.available_cpus = static_cast<std::size_t>(available);
    }
    struct sysinfo info{};
    if (sysinfo(&info) == 0) {
        result.available_memory_bytes = static_cast<std::size_t>(info.freeram) * info.mem_unit;
    }
#endif
    result.physical_cores = std::max(std::size_t{1}, result.physical_cores);
    result.available_cpus = std::max(std::size_t{1}, result.available_cpus);
    return result;
}

} // namespace glifistore
