#include "glifistore/server/poller.hpp"
#include "system_error.hpp"

#if !defined(__linux__)
#error "epoll backend requires Linux"
#endif

#include "glifistore/core/fault_injection.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <sys/epoll.h>
#include <unistd.h>

namespace glifistore::server {
namespace {

[[nodiscard]] auto epoll_flags(const IoInterest interest) noexcept -> std::uint32_t {
    std::uint32_t flags = EPOLLET | EPOLLRDHUP;
    if (has_interest(interest, IoInterest::read)) {
        flags |= EPOLLIN;
    }
    if (has_interest(interest, IoInterest::write)) {
        flags |= EPOLLOUT;
    }
    return flags;
}

[[nodiscard]] auto io_flags(const std::uint32_t events) noexcept -> IoFlags {
    auto flags = IoFlags::none;
    if ((events & EPOLLIN) != 0U) {
        flags = flags | IoFlags::readable;
    }
    if ((events & EPOLLOUT) != 0U) {
        flags = flags | IoFlags::writable;
    }
    if ((events & EPOLLERR) != 0U) {
        flags = flags | IoFlags::error;
    }
    if ((events & (EPOLLHUP | EPOLLRDHUP)) != 0U) {
        flags = flags | IoFlags::hangup;
    }
    return flags;
}

} // namespace

Poller::~Poller() {
    reset();
}

Poller::Poller(Poller&& other) noexcept : descriptor_(other.release()) {}

auto Poller::operator=(Poller&& other) noexcept -> Poller& {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

auto Poller::release() noexcept -> int {
    const auto descriptor = descriptor_;
    descriptor_ = -1;
    return descriptor;
}

void Poller::reset(const int descriptor) noexcept {
    if (descriptor_ >= 0) {
        static_cast<void>(::close(descriptor_));
    }
    descriptor_ = descriptor;
}

auto Poller::create() -> Result<Poller> {
    const auto descriptor = ::epoll_create1(EPOLL_CLOEXEC);
    if (descriptor < 0) {
        return system_error("epoll_create1");
    }
    return Poller{descriptor};
}

auto Poller::add(const int descriptor, const std::uint64_t token, const IoInterest interest) -> Status {
    epoll_event event{};
    event.events = epoll_flags(interest);
    event.data.u64 = token;
    if (::epoll_ctl(descriptor_, EPOLL_CTL_ADD, descriptor, &event) != 0) {
        return system_error("epoll_ctl(ADD)");
    }
    return {};
}

auto Poller::modify(const int descriptor, const std::uint64_t token, const IoInterest interest) -> Status {
    epoll_event event{};
    event.events = epoll_flags(interest);
    event.data.u64 = token;
    if (::epoll_ctl(descriptor_, EPOLL_CTL_MOD, descriptor, &event) != 0) {
        return system_error("epoll_ctl(MOD)");
    }
    return {};
}

auto Poller::remove(const int descriptor) -> Status {
    if (glifistore::fault::consume_fail(glifistore::fault::Site::poller_remove)) {
        return fail(ErrorCode::io_error, "injected poller remove failure");
    }
    int result = 0;
    do {
        result = ::epoll_ctl(descriptor_, EPOLL_CTL_DEL, descriptor, nullptr);
    } while (result != 0 && errno == EINTR);
    if (result != 0 && errno != ENOENT) {
        return system_error("epoll_ctl(DEL)");
    }
    return {};
}

auto Poller::wait(const std::span<IoEvent> events, const int timeout_ms) -> Result<std::size_t> {
    if (events.empty()) {
        return std::size_t{0};
    }
    std::array<epoll_event, 1024> native_events{};
    const auto capacity = std::min(events.size(), native_events.size());
    int ready = 0;
    do {
        ready = ::epoll_wait(descriptor_, native_events.data(), static_cast<int>(capacity), timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready < 0) {
        return system_error("epoll_wait");
    }
    for (int index = 0; index < ready; ++index) {
        events[static_cast<std::size_t>(index)] = {
            .token = native_events[static_cast<std::size_t>(index)].data.u64,
            .flags = io_flags(native_events[static_cast<std::size_t>(index)].events)};
    }
    return static_cast<std::size_t>(ready);
}

} // namespace glifistore::server
