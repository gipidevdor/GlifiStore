#include "persistence/recovery/recovery_budget.hpp"

namespace glifistore::recovery {

RecoveryMemoryBudget::RecoveryMemoryBudget(const std::uint64_t maximum) : maximum_(maximum) {}

auto RecoveryMemoryBudget::ensure_peak(const std::uint64_t additional) const -> Status {
    if (additional > maximum_ - retained_) {
        return fail(ErrorCode::resource_exhausted, "durable recovery exceeds the configured memory budget");
    }
    return {};
}

auto RecoveryMemoryBudget::retain(const std::uint64_t additional) -> Status {
    if (auto available = ensure_peak(additional); !available) {
        return available;
    }
    retained_ += additional;
    return {};
}

auto RecoveryMemoryBudget::retain_repeated(const std::uint64_t unit, const std::size_t count) -> Status {
    if (unit != 0 && count > std::numeric_limits<std::uint64_t>::max() / unit) {
        return fail(ErrorCode::arithmetic_overflow, "durable recovery memory estimate overflow");
    }
    return retain(unit * static_cast<std::uint64_t>(count));
}

} // namespace glifistore::recovery
