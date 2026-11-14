#pragma once

#include <atomic>
#include <chrono>
#include <thread>

namespace glifistore::test {

// Test-side handshake for the documented bounded-generation contract. A Writer
// that receives reader-quiescence backpressure asks the Reader to remain between
// leases, waits until that state is visible, retries the same mutation, and then
// releases the Reader. This does not weaken GET/value assertions.
class PairedReaderQuiescence final {
  public:
    void reader_checkpoint(const std::atomic_bool& stop) noexcept {
        if (!requested_.load(std::memory_order_acquire)) {
            return;
        }
        quiescent_.store(true, std::memory_order_release);
        while (requested_.load(std::memory_order_acquire) && !stop.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        quiescent_.store(false, std::memory_order_release);
    }

    [[nodiscard]] auto request_until(const std::chrono::steady_clock::time_point deadline) noexcept -> bool {
        requested_.store(true, std::memory_order_release);
        while (!quiescent_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        if (quiescent_.load(std::memory_order_acquire)) {
            return true;
        }
        requested_.store(false, std::memory_order_release);
        return false;
    }

    void release() noexcept {
        requested_.store(false, std::memory_order_release);
    }

  private:
    std::atomic_bool requested_{};
    std::atomic_bool quiescent_{};
};

} // namespace glifistore::test
