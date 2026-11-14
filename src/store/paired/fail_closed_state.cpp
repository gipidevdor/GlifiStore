#include "glifistore/store/paired/fail_closed_state.hpp"

#include "store/store_internal.hpp"

namespace glifistore::store::paired {

void FailClosedState::arm(const std::span<const FailClosedLaneWake> lanes,
                          const FailClosedScope scope) noexcept {
    healthy_.store(false, std::memory_order_release);
    if (scope == FailClosedScope::pair_and_store) {
        detail::StoreAccess::mark_fail_closed(store_);
    }
    expire_remaining_.store(true, std::memory_order_release);
    for (const auto& lane : lanes) {
        if (lane.signal == nullptr) {
            continue;
        }
        lane.signal->fetch_add(1U, std::memory_order_release);
        lane.signal->notify_one();
    }
}

} // namespace glifistore::store::paired
