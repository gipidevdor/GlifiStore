#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>

namespace allocation_fault {

struct Observation {
    std::size_t observed{};
    bool fired{};
};

extern std::atomic_bool forbid_all;

void arm(std::size_t fail_at) noexcept;
void arm_process(std::size_t fail_at) noexcept;
void disarm_process() noexcept;
auto disarm() noexcept -> Observation;
void begin_forbid_all() noexcept;
auto end_forbid_all() noexcept -> bool;
// Model caller descheduling during a response allocation, without failing it.
void delay_next_allocation_of_size(std::size_t size, std::chrono::milliseconds delay) noexcept;
[[nodiscard]] auto end_allocation_delay() noexcept -> bool;

} // namespace allocation_fault
