#pragma once

// Optional hot-path phase attribution for lab measurements.
//
// Default / production builds: every GS_PHASE_* macro is a compile-time no-op.
// Enable with -DGLIFISTORE_HOT_PATH_PHASES=ON (CMake). Counters are preallocated,
// shard/thread-local friendly atomics — no global mutexes and no hot-path text logging.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace glifistore::hot_path {

enum class GetPhase : std::uint8_t {
    admit = 0,
    hash,
    route,
    lease_adopt,
    index_lookup,
    value_copy,
    release,
    count,
};

enum class PutPhase : std::uint8_t {
    admit = 0,
    encode_copy,
    index_publish,
    enqueue,
    worker_apply,
    publish,
    ack,
    batch_collect,
    store_apply,
    generation_build,
    generation_publish,
    delta_flat_spine_copy,
    delta_directory_spine_copy,
    delta_page_clone,
    delta_chunk_clone,
    delta_block_clone,
    delta_record_store,
    generation_shell_allocate,
    completion_delivery,
    completion_notify,
    completion_adopt,
    completion_accounting,
    completion_response,
    completion_pipeline_resume,
    completion_socket_flush,
    count,
};

enum class TcpPhase : std::uint8_t {
    decode = 0,
    route,
    store_op,
    encode,
    socket_write,
    socket_write_progress,
    socket_write_would_block,
    socket_write_partial,
    socket_write_interrupted,
    poller_update,
    count,
};

[[nodiscard]] constexpr auto get_phase_name(const GetPhase phase) noexcept -> const char* {
    switch (phase) {
    case GetPhase::admit:
        return "admit";
    case GetPhase::hash:
        return "hash";
    case GetPhase::route:
        return "route";
    case GetPhase::lease_adopt:
        return "lease_adopt";
    case GetPhase::index_lookup:
        return "index_lookup";
    case GetPhase::value_copy:
        return "value_copy";
    case GetPhase::release:
        return "release";
    case GetPhase::count:
        break;
    }
    return "unknown";
}

[[nodiscard]] constexpr auto put_phase_name(const PutPhase phase) noexcept -> const char* {
    switch (phase) {
    case PutPhase::admit:
        return "admit";
    case PutPhase::encode_copy:
        return "encode_copy";
    case PutPhase::index_publish:
        return "index_publish";
    case PutPhase::enqueue:
        return "enqueue";
    case PutPhase::worker_apply:
        return "worker_apply";
    case PutPhase::publish:
        return "publish";
    case PutPhase::ack:
        return "ack";
    case PutPhase::batch_collect:
        return "batch_collect";
    case PutPhase::store_apply:
        return "store_apply";
    case PutPhase::generation_build:
        return "generation_build";
    case PutPhase::generation_publish:
        return "generation_publish";
    case PutPhase::delta_flat_spine_copy:
        return "delta_flat_spine_copy";
    case PutPhase::delta_directory_spine_copy:
        return "delta_directory_spine_copy";
    case PutPhase::delta_page_clone:
        return "delta_page_clone";
    case PutPhase::delta_chunk_clone:
        return "delta_chunk_clone";
    case PutPhase::delta_block_clone:
        return "delta_block_clone";
    case PutPhase::delta_record_store:
        return "delta_record_store";
    case PutPhase::generation_shell_allocate:
        return "generation_shell_allocate";
    case PutPhase::completion_delivery:
        return "completion_delivery";
    case PutPhase::completion_notify:
        return "completion_notify";
    case PutPhase::completion_adopt:
        return "completion_adopt";
    case PutPhase::completion_accounting:
        return "completion_accounting";
    case PutPhase::completion_response:
        return "completion_response";
    case PutPhase::completion_pipeline_resume:
        return "completion_pipeline_resume";
    case PutPhase::completion_socket_flush:
        return "completion_socket_flush";
    case PutPhase::count:
        break;
    }
    return "unknown";
}

[[nodiscard]] constexpr auto tcp_phase_name(const TcpPhase phase) noexcept -> const char* {
    switch (phase) {
    case TcpPhase::decode:
        return "decode";
    case TcpPhase::route:
        return "route";
    case TcpPhase::store_op:
        return "store_op";
    case TcpPhase::encode:
        return "encode";
    case TcpPhase::socket_write:
        return "socket_write";
    case TcpPhase::socket_write_progress:
        return "write_progress";
    case TcpPhase::socket_write_would_block:
        return "write_would_block";
    case TcpPhase::socket_write_partial:
        return "write_partial";
    case TcpPhase::socket_write_interrupted:
        return "write_interrupted";
    case TcpPhase::poller_update:
        return "poller_update";
    case TcpPhase::count:
        break;
    }
    return "unknown";
}

struct alignas(64) PhaseBucket final {
    std::atomic<std::uint64_t> total_ns{};
    std::atomic<std::uint64_t> samples{};
};

struct PhaseSnapshot final {
    std::uint64_t total_ns{};
    std::uint64_t samples{};
};

#if defined(GLIFISTORE_HOT_PATH_PHASES)

[[nodiscard]] auto enabled() noexcept -> bool;
void set_enabled(bool on) noexcept;
void reset() noexcept;
void observe_get(GetPhase phase, std::uint64_t elapsed_ns) noexcept;
void observe_put(PutPhase phase, std::uint64_t elapsed_ns) noexcept;
void observe_tcp(TcpPhase phase, std::uint64_t elapsed_ns) noexcept;
[[nodiscard]] auto snapshot_get(GetPhase phase) noexcept -> PhaseSnapshot;
[[nodiscard]] auto snapshot_put(PutPhase phase) noexcept -> PhaseSnapshot;
[[nodiscard]] auto snapshot_tcp(TcpPhase phase) noexcept -> PhaseSnapshot;
[[nodiscard]] auto format_report() -> std::string;

class PhaseScope final {
  public:
    explicit PhaseScope(void (*observe)(std::uint8_t, std::uint64_t) noexcept,
                        const std::uint8_t phase) noexcept;
    ~PhaseScope();
    void finish() noexcept;

    PhaseScope(const PhaseScope&) = delete;
    auto operator=(const PhaseScope&) -> PhaseScope& = delete;

  private:
    void (*observe_)(std::uint8_t, std::uint64_t) noexcept {};
    std::uint8_t phase_{};
    std::uint64_t start_ns_{};
};

[[nodiscard]] auto now_ns() noexcept -> std::uint64_t;

inline void observe_get_u8(const std::uint8_t phase, const std::uint64_t elapsed_ns) noexcept {
    observe_get(static_cast<GetPhase>(phase), elapsed_ns);
}

inline void observe_put_u8(const std::uint8_t phase, const std::uint64_t elapsed_ns) noexcept {
    observe_put(static_cast<PutPhase>(phase), elapsed_ns);
}

inline void observe_tcp_u8(const std::uint8_t phase, const std::uint64_t elapsed_ns) noexcept {
    observe_tcp(static_cast<TcpPhase>(phase), elapsed_ns);
}

#else

inline auto enabled() noexcept -> bool {
    return false;
}

inline void set_enabled(bool) noexcept {}

inline void reset() noexcept {}

inline void observe_get(GetPhase, std::uint64_t) noexcept {}

inline void observe_put(PutPhase, std::uint64_t) noexcept {}

inline void observe_tcp(TcpPhase, std::uint64_t) noexcept {}

inline auto snapshot_get(GetPhase) noexcept -> PhaseSnapshot {
    return {};
}

inline auto snapshot_put(PutPhase) noexcept -> PhaseSnapshot {
    return {};
}

inline auto snapshot_tcp(TcpPhase) noexcept -> PhaseSnapshot {
    return {};
}

inline auto format_report() -> std::string {
    return {};
}

#endif

} // namespace glifistore::hot_path

#if defined(GLIFISTORE_HOT_PATH_PHASES)
#define GS_PHASE_CONCAT_INNER(a, b) a##b
#define GS_PHASE_CONCAT(a, b) GS_PHASE_CONCAT_INNER(a, b)
#define GS_PHASE_GET(phase)                                                                                  \
    const ::glifistore::hot_path::PhaseScope GS_PHASE_CONCAT(gs_phase_get_, __COUNTER__) {                   \
        &::glifistore::hot_path::observe_get_u8,                                                             \
            static_cast<std::uint8_t>(::glifistore::hot_path::GetPhase::phase)                               \
    }
#define GS_PHASE_PUT(phase)                                                                                  \
    const ::glifistore::hot_path::PhaseScope GS_PHASE_CONCAT(gs_phase_put_, __COUNTER__) {                   \
        &::glifistore::hot_path::observe_put_u8,                                                             \
            static_cast<std::uint8_t>(::glifistore::hot_path::PutPhase::phase)                               \
    }
#define GS_PHASE_PUT_NAMED(name, phase)                                                                      \
    ::glifistore::hot_path::PhaseScope name {                                                                \
        &::glifistore::hot_path::observe_put_u8,                                                             \
            static_cast<std::uint8_t>(::glifistore::hot_path::PutPhase::phase)                               \
    }
#define GS_PHASE_TCP(phase)                                                                                  \
    const ::glifistore::hot_path::PhaseScope GS_PHASE_CONCAT(gs_phase_tcp_, __COUNTER__) {                   \
        &::glifistore::hot_path::observe_tcp_u8,                                                             \
            static_cast<std::uint8_t>(::glifistore::hot_path::TcpPhase::phase)                               \
    }
#define GS_PHASE_TCP_NAMED(name, phase)                                                                      \
    ::glifistore::hot_path::PhaseScope name {                                                                \
        &::glifistore::hot_path::observe_tcp_u8,                                                             \
            static_cast<std::uint8_t>(::glifistore::hot_path::TcpPhase::phase)                               \
    }
#define GS_EVENT_TCP(phase) ::glifistore::hot_path::observe_tcp(::glifistore::hot_path::TcpPhase::phase, 0U)
#define GS_PHASE_FINISH(name) name.finish()
#else
#define GS_PHASE_GET(phase) static_cast<void>(0)
#define GS_PHASE_PUT(phase) static_cast<void>(0)
#define GS_PHASE_PUT_NAMED(name, phase) static_cast<void>(0)
#define GS_PHASE_TCP(phase) static_cast<void>(0)
#define GS_PHASE_TCP_NAMED(name, phase) static_cast<void>(0)
#define GS_EVENT_TCP(phase) static_cast<void>(0)
#define GS_PHASE_FINISH(name) static_cast<void>(0)
#endif
