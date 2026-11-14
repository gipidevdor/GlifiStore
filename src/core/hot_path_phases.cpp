#include "glifistore/core/hot_path_phases.hpp"

#if defined(GLIFISTORE_HOT_PATH_PHASES)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace glifistore::hot_path {
namespace {

std::atomic_bool g_enabled{true};

struct ReportAtExit final {
    ReportAtExit() {
        if (const char* flag = std::getenv("GLIFISTORE_HOT_PATH_PHASE_REPORT");
            flag != nullptr && flag[0] != '\0' && flag[0] != '0') {
            std::atexit([] {
                const auto report = format_report();
                if (!report.empty()) {
                    std::fputs(report.c_str(), stderr);
                    std::fflush(stderr);
                }
            });
        }
    }
} g_report_at_exit;

template <std::size_t N> struct alignas(64) PhaseTable final {
    std::array<PhaseBucket, N> buckets{};
};

PhaseTable<static_cast<std::size_t>(GetPhase::count)> g_get{};
PhaseTable<static_cast<std::size_t>(PutPhase::count)> g_put{};
PhaseTable<static_cast<std::size_t>(TcpPhase::count)> g_tcp{};

void observe_bucket(PhaseBucket& bucket, const std::uint64_t elapsed_ns) noexcept {
    bucket.total_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
    bucket.samples.fetch_add(1U, std::memory_order_relaxed);
}

[[nodiscard]] auto snapshot_bucket(const PhaseBucket& bucket) noexcept -> PhaseSnapshot {
    return PhaseSnapshot{
        .total_ns = bucket.total_ns.load(std::memory_order_relaxed),
        .samples = bucket.samples.load(std::memory_order_relaxed),
    };
}

void append_section(std::string& out, const char* title, const auto& name_fn, const auto& snap_fn,
                    const std::size_t count) {
    std::uint64_t section_total = 0;
    for (std::size_t index = 0; index < count; ++index) {
        section_total += snap_fn(index).total_ns;
    }
    out += title;
    out += '\n';
    char line[192];
    for (std::size_t index = 0; index < count; ++index) {
        const auto snap = snap_fn(index);
        const auto mean =
            snap.samples == 0 ? 0.0 : static_cast<double>(snap.total_ns) / static_cast<double>(snap.samples);
        const auto pct =
            section_total == 0
                ? 0.0
                : (100.0 * static_cast<double>(snap.total_ns) / static_cast<double>(section_total));
        std::snprintf(line, sizeof(line), "  %-14s samples=%llu total_ns=%llu mean_ns=%.2f pct=%.1f\n",
                      name_fn(index), static_cast<unsigned long long>(snap.samples),
                      static_cast<unsigned long long>(snap.total_ns), mean, pct);
        out += line;
    }
}

} // namespace

auto now_ns() noexcept -> std::uint64_t {
    using clock = std::chrono::steady_clock;
    const auto elapsed = clock::now().time_since_epoch();
    if (elapsed <= clock::duration::zero()) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

auto enabled() noexcept -> bool {
    return g_enabled.load(std::memory_order_relaxed);
}

void set_enabled(const bool on) noexcept {
    g_enabled.store(on, std::memory_order_relaxed);
}

void reset() noexcept {
    for (auto& bucket : g_get.buckets) {
        bucket.total_ns.store(0, std::memory_order_relaxed);
        bucket.samples.store(0, std::memory_order_relaxed);
    }
    for (auto& bucket : g_put.buckets) {
        bucket.total_ns.store(0, std::memory_order_relaxed);
        bucket.samples.store(0, std::memory_order_relaxed);
    }
    for (auto& bucket : g_tcp.buckets) {
        bucket.total_ns.store(0, std::memory_order_relaxed);
        bucket.samples.store(0, std::memory_order_relaxed);
    }
}

void observe_get(const GetPhase phase, const std::uint64_t elapsed_ns) noexcept {
    if (!enabled() || phase >= GetPhase::count) {
        return;
    }
    observe_bucket(g_get.buckets[static_cast<std::size_t>(phase)], elapsed_ns);
}

void observe_put(const PutPhase phase, const std::uint64_t elapsed_ns) noexcept {
    if (!enabled() || phase >= PutPhase::count) {
        return;
    }
    observe_bucket(g_put.buckets[static_cast<std::size_t>(phase)], elapsed_ns);
}

void observe_tcp(const TcpPhase phase, const std::uint64_t elapsed_ns) noexcept {
    if (!enabled() || phase >= TcpPhase::count) {
        return;
    }
    observe_bucket(g_tcp.buckets[static_cast<std::size_t>(phase)], elapsed_ns);
}

auto snapshot_get(const GetPhase phase) noexcept -> PhaseSnapshot {
    if (phase >= GetPhase::count) {
        return {};
    }
    return snapshot_bucket(g_get.buckets[static_cast<std::size_t>(phase)]);
}

auto snapshot_put(const PutPhase phase) noexcept -> PhaseSnapshot {
    if (phase >= PutPhase::count) {
        return {};
    }
    return snapshot_bucket(g_put.buckets[static_cast<std::size_t>(phase)]);
}

auto snapshot_tcp(const TcpPhase phase) noexcept -> PhaseSnapshot {
    if (phase >= TcpPhase::count) {
        return {};
    }
    return snapshot_bucket(g_tcp.buckets[static_cast<std::size_t>(phase)]);
}

auto format_report() -> std::string {
    std::string out;
    out.reserve(2048);
    out += "GlifiStore hot-path phase attribution (lab instrumentation)\n";
    out += "Claim ceiling: architectural prototype / same-machine lab evidence only.\n";
    append_section(
        out, "[GET]", [](const std::size_t index) { return get_phase_name(static_cast<GetPhase>(index)); },
        [](const std::size_t index) { return snapshot_get(static_cast<GetPhase>(index)); },
        static_cast<std::size_t>(GetPhase::count));
    append_section(
        out, "[PUT/ERASE scopes; some Writer scopes contain Store leaf scopes]",
        [](const std::size_t index) { return put_phase_name(static_cast<PutPhase>(index)); },
        [](const std::size_t index) { return snapshot_put(static_cast<PutPhase>(index)); },
        static_cast<std::size_t>(PutPhase::count));
    append_section(
        out, "[TCP leaf phases; additive; write outcome rows use sample counts]",
        [](const std::size_t index) { return tcp_phase_name(static_cast<TcpPhase>(index)); },
        [](const std::size_t index) { return snapshot_tcp(static_cast<TcpPhase>(index)); },
        static_cast<std::size_t>(TcpPhase::count));
    return out;
}

PhaseScope::PhaseScope(void (*observe)(std::uint8_t, std::uint64_t) noexcept,
                       const std::uint8_t phase) noexcept
    : observe_(observe), phase_(phase), start_ns_(enabled() ? now_ns() : 0) {}

PhaseScope::~PhaseScope() {
    finish();
}

void PhaseScope::finish() noexcept {
    if (observe_ == nullptr || start_ns_ == 0) {
        return;
    }
    const auto end = now_ns();
    const auto elapsed = end >= start_ns_ ? end - start_ns_ : 0U;
    observe_(phase_, elapsed);
    start_ns_ = 0;
}

} // namespace glifistore::hot_path

#endif
