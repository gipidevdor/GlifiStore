/**
 * @file
 * @brief Deterministic failure seams for allocation and exception campaigns.
 */
#pragma once

// Debug-only adverse scheduling hooks for concurrency stress (Phase B1).
//
// Production / default builds: every GS_FAULT_SITE expands to a no-op.
// Enable only with -DGLIFISTORE_FAULT_INJECTION=ON (CMake) which defines the
// compile macro GLIFISTORE_FAULT_INJECTION. Never enable in release packaging.

#include <chrono>
#include <cstdint>

namespace glifistore::fault {

/**
 * @brief Named deterministic failure sites; instrumentation is enabled only in the corresponding build
 * profile.
 */
enum class Site : std::uint8_t {
    enqueue = 1,
    mutate = 2,
    publish = 3,
    adopt = 4,
    rotate = 5,
    compact = 6,
    close = 7,
    capture = 8,
    deferred_ttl = 9,
    index_account = 10,
    response_queue = 11,
    input_buffer = 12,
    init_identity = 13,
    poller_remove = 14,
    tls_write_want_read = 15,
    backup_report = 16,
    // After mutate_durable_batch returns, before result classification finishes.
    post_mutate = 17,
    // Writer try_drain_durable_snapshot returns false before publishing (litmus).
    drain_snapshot = 18,
    // Start of StoreAccess::mutate_durable_batch before any durable mutate (litmus).
    durable_batch_pre = 19,
    // DurableSegmentFile::open before any Record write (litmus: known not committed).
    segment_open = 20,
    // Before each mutate_durable_batch sibling mutate (litmus: mid-batch gate TOCTOU).
    durable_batch_gate = 21,
    // Before each Store::put_batch non-paired sibling put (litmus: mid-batch gate TOCTOU).
    put_batch_gate = 22,
    // Dedicated Writer after detaching one synchronous admission snapshot.
    sync_lane_snapshot = 23,
    // SSL_write path returns WANT_WRITE once (edge-triggered flush litmus).
    tls_write_want_write = 24,
};

#if defined(GLIFISTORE_FAULT_INJECTION)

void at(Site site) noexcept;
void configure(std::uint64_t seed, std::uint32_t yield_percent, std::uint32_t sleep_us_max) noexcept;
void reset() noexcept;
// Deterministic one-shot failure for litmus tests (does not throw from at()).
// Distinct sites may be armed concurrently (independent pending slots).
void fail_once(Site site) noexcept;
// Fail the Nth consume_fail for site (1 = first). Zero clears that site only.
void fail_nth(Site site, std::uint32_t n) noexcept;
[[nodiscard]] auto consume_fail(Site site) noexcept -> bool;
// Deterministic one-shot block for litmus: maybe_block waits until release_block.
void arm_block(Site site) noexcept;
void release_block(Site site) noexcept;
[[nodiscard]] auto wait_until_blocked(Site site, std::chrono::milliseconds timeout = std::chrono::seconds{2})
    -> bool;
void maybe_block(Site site) noexcept;

#else

inline void at(Site) noexcept {}

inline void configure(std::uint64_t, std::uint32_t, std::uint32_t) noexcept {}

inline void reset() noexcept {}

inline void fail_once(Site) noexcept {}

inline void fail_nth(Site, std::uint32_t) noexcept {}

inline auto consume_fail(Site) noexcept -> bool {
    return false;
}

inline void arm_block(Site) noexcept {}

inline void release_block(Site) noexcept {}

inline auto wait_until_blocked(Site, std::chrono::milliseconds = std::chrono::seconds{2}) -> bool {
    return false;
}

inline void maybe_block(Site) noexcept {}

#endif

} // namespace glifistore::fault

#if defined(GLIFISTORE_FAULT_INJECTION)
#define GS_FAULT_SITE(site) ::glifistore::fault::at(::glifistore::fault::Site::site)
#define GS_FAULT_BLOCK(site) ::glifistore::fault::maybe_block(::glifistore::fault::Site::site)
#else
#define GS_FAULT_SITE(site) ((void)0)
#define GS_FAULT_BLOCK(site) ((void)0)
#endif
