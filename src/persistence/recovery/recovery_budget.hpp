#pragma once

#include "glifistore/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace glifistore::recovery {

class RecoveryMemoryBudget final {
  public:
    explicit RecoveryMemoryBudget(std::uint64_t maximum);

    [[nodiscard]] auto ensure_peak(std::uint64_t additional) const -> Status;
    [[nodiscard]] auto retain(std::uint64_t additional) -> Status;
    [[nodiscard]] auto retain_repeated(std::uint64_t unit, std::size_t count) -> Status;

  private:
    std::uint64_t maximum_{};
    std::uint64_t retained_{};
};

inline constexpr std::uint64_t kRecoveryBytesPerSegment = 256;
inline constexpr std::uint64_t kRecoveryBytesPerWorker = 1024;
inline constexpr std::uint64_t kRecoveryBytesPerKey = 256;

} // namespace glifistore::recovery
