#include "glifistore/store/prepared_read.hpp"
#include "store/store_impl.hpp"

#include <memory>
#include <new>
#include <utility>

namespace glifistore {

detail::PreparedColdRead::PreparedColdRead(State&& state) noexcept {
    static_assert(sizeof(State) <= kStateBytes);
    static_assert(alignof(State) <= alignof(std::max_align_t));
    std::construct_at(reinterpret_cast<State*>(storage_.data()), std::move(state));
    engaged_ = true;
}

auto detail::PreparedColdRead::state() noexcept -> State* {
    return std::launder(reinterpret_cast<State*>(storage_.data()));
}

auto detail::PreparedColdRead::state() const noexcept -> const State* {
    return std::launder(reinterpret_cast<const State*>(storage_.data()));
}

void detail::PreparedColdRead::reset() noexcept {
    if (engaged_) {
        std::destroy_at(state());
        engaged_ = false;
    }
}

detail::PreparedColdRead::PreparedColdRead(PreparedColdRead&& other) noexcept {
    if (other.engaged_) {
        std::construct_at(reinterpret_cast<State*>(storage_.data()), std::move(*other.state()));
        engaged_ = true;
        other.reset();
    }
}

auto detail::PreparedColdRead::operator=(PreparedColdRead&& other) noexcept -> PreparedColdRead& {
    if (this != &other) {
        reset();
        if (other.engaged_) {
            std::construct_at(reinterpret_cast<State*>(storage_.data()), std::move(*other.state()));
            engaged_ = true;
            other.reset();
        }
    }
    return *this;
}

detail::PreparedColdRead::~PreparedColdRead() {
    reset();
}

} // namespace glifistore
