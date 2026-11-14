#pragma once

#include <algorithm>
#include <cstddef>

namespace glifistore::detail {

class AdaptiveBatchSizer final {
  public:
    void reset(const std::size_t minimum, const std::size_t maximum) noexcept {
        minimum_ = minimum;
        maximum_ = maximum;
        target_ = maximum;
    }

    [[nodiscard]] auto target() const noexcept -> std::size_t {
        return target_;
    }

    void observe_deadline(const std::size_t occupancy) noexcept {
        target_ = std::clamp(occupancy, minimum_, maximum_);
    }

    void observe_target_reached(const std::size_t occupancy, const std::size_t admissions) noexcept {
        if (admissions <= occupancy) {
            return;
        }
        target_ = std::min(maximum_, std::max(occupancy + 1U, admissions));
    }

  private:
    std::size_t minimum_{1};
    std::size_t maximum_{1};
    std::size_t target_{1};
};

} // namespace glifistore::detail
