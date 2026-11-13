#pragma once

#include <atomic>
#include <cstdint>

namespace glifistore::runtime_catalog_detail {

/**
 * @brief Logical ownership handshake for one durable Worker's mutable Index.
 *
 * Under the mutex-elided exclusive Writer, the Worker mutex does not serialize
 * Index publication. Every Index publisher (the Writer) brackets its Index work
 * with enter_writer()/leave_writer() and checks armed() after entering; every
 * exclusive Index owner (compaction snapshot and commit, unread-TTL probe,
 * prepared GETs, flush) calls arm() and then wait_for_writers().
 *
 * Correctness argument (Dekker): the Writer performs `depth += 1; read holders`
 * and an owner performs `holders += 1; read depth`. All four accesses are
 * seq_cst, so they belong to one total order and at least one side observes the
 * other: either the Writer sees a holder and backs out, or the owner sees the
 * Writer and waits for leave_writer(). Acquire/release alone permits the
 * store-buffering outcome in which both proceed.
 *
 * The gate is armed exactly when `holders != 0`. A single counter (rather than
 * a counter plus a separate boolean) is required: with two variables, a
 * concurrent final disarm and first arm can interleave so a holder believes the
 * gate is armed while the boolean reads false.
 */
class IndexQuiesceGate final {
  public:
    /// Writer hot path: announce Index work. Must precede the armed() check.
    void enter_writer() noexcept {
        writer_depth_.fetch_add(1U, std::memory_order_seq_cst);
    }

    /// Writer hot path: end Index work and wake an owner waiting for zero depth.
    void leave_writer() noexcept {
        if (writer_depth_.fetch_sub(1U, std::memory_order_seq_cst) == 1U) {
            writer_depth_.notify_all();
        }
    }

    /// True while at least one exclusive Index owner holds the gate.
    [[nodiscard]] auto armed() const noexcept -> bool {
        return holders_.load(std::memory_order_seq_cst) != 0U;
    }

    /// Owner: take one nested hold. Writers entering afterwards back out.
    void arm() noexcept {
        holders_.fetch_add(1U, std::memory_order_seq_cst);
    }

    /// Owner: release one hold.
    /// @return true when this call released the final hold.
    [[nodiscard]] auto disarm() noexcept -> bool {
        return holders_.fetch_sub(1U, std::memory_order_seq_cst) == 1U;
    }

    /// Owner, after arm(): block until every Writer that entered before the hold left.
    void wait_for_writers() const noexcept {
        for (;;) {
            const auto depth = writer_depth_.load(std::memory_order_seq_cst);
            if (depth == 0U) {
                return;
            }
            writer_depth_.wait(depth, std::memory_order_acquire);
        }
    }

    [[nodiscard]] auto writer_depth() const noexcept -> std::uint32_t {
        return writer_depth_.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto holders() const noexcept -> std::uint32_t {
        return holders_.load(std::memory_order_acquire);
    }

  private:
    std::atomic_uint32_t writer_depth_{};
    std::atomic_uint32_t holders_{};
};

} // namespace glifistore::runtime_catalog_detail
