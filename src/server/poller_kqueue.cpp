#include "glifistore/server/poller.hpp"

#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__OpenBSD__)
#error "kqueue backend requires macOS, FreeBSD, or OpenBSD"
#endif

#include "glifistore/core/fault_injection.hpp"
#include "glifistore/server/socket.hpp"
#include "system_error.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

namespace glifistore::server {
namespace {

[[nodiscard]] auto token_pointer(const std::uint64_t token) noexcept -> void* {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(token));
}

[[nodiscard]] auto token_value(void* token) noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(token));
}

void set_change(struct kevent& change, const int descriptor, const int16_t filter, const bool enabled,
                const std::uint64_t token) noexcept {
    const auto flags = static_cast<std::uint16_t>(EV_ADD | EV_CLEAR | (enabled ? EV_ENABLE : EV_DISABLE));
    EV_SET(&change, static_cast<std::uintptr_t>(descriptor), filter, flags, 0, 0, token_pointer(token));
}

[[nodiscard]] auto apply_interests(const int poller, const int descriptor, const std::uint64_t token,
                                   const IoInterest interest) -> Status {
    std::array<struct kevent, 2> changes{};
    set_change(changes[0], descriptor, EVFILT_READ, has_interest(interest, IoInterest::read), token);
    set_change(changes[1], descriptor, EVFILT_WRITE, has_interest(interest, IoInterest::write), token);
    if (::kevent(poller, changes.data(), static_cast<int>(changes.size()), nullptr, 0, nullptr) != 0) {
        return system_error("kevent(update)");
    }
    return {};
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
    const auto descriptor = ::kqueue();
    if (descriptor < 0) {
        return system_error("kqueue");
    }
    if (auto close_on_exec = set_close_on_exec(descriptor); !close_on_exec) {
        static_cast<void>(::close(descriptor));
        return unexpected(close_on_exec.error());
    }
    return Poller{descriptor};
}

auto Poller::add(const int descriptor, const std::uint64_t token, const IoInterest interest) -> Status {
    return apply_interests(descriptor_, descriptor, token, interest);
}

auto Poller::modify(const int descriptor, const std::uint64_t token, const IoInterest interest) -> Status {
    return apply_interests(descriptor_, descriptor, token, interest);
}

auto Poller::remove(const int descriptor) -> Status {
    if (glifistore::fault::consume_fail(glifistore::fault::Site::poller_remove)) {
        return fail(ErrorCode::io_error, "injected poller remove failure");
    }
    std::array<struct kevent, 2> changes{};
    EV_SET(&changes[0], static_cast<std::uintptr_t>(descriptor), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], static_cast<std::uintptr_t>(descriptor), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    int result = 0;
    do {
        result = ::kevent(descriptor_, changes.data(), static_cast<int>(changes.size()), nullptr, 0, nullptr);
    } while (result != 0 && errno == EINTR);
    if (result != 0 && errno != ENOENT) {
        return system_error("kevent(delete)");
    }
    return {};
}

auto Poller::wait(const std::span<IoEvent> events, const int timeout_ms) -> Result<std::size_t> {
    if (events.empty()) {
        return std::size_t{0};
    }
    std::array<struct kevent, 1024> native_events{};
    const auto capacity = std::min(events.size(), native_events.size());
    timespec timeout{.tv_sec = timeout_ms / 1000,
                     .tv_nsec = static_cast<long>(timeout_ms % 1000) * 1'000'000L};
    const timespec* timeout_pointer = timeout_ms < 0 ? nullptr : &timeout;
    int ready = 0;
    do {
        ready = ::kevent(descriptor_, nullptr, 0, native_events.data(), static_cast<int>(capacity),
                         timeout_pointer);
    } while (ready < 0 && errno == EINTR);
    if (ready < 0) {
        return system_error("kevent(wait)");
    }
    for (int index = 0; index < ready; ++index) {
        const auto& native = native_events[static_cast<std::size_t>(index)];
        auto flags = native.filter == EVFILT_READ ? IoFlags::readable : IoFlags::writable;
        if ((native.flags & EV_ERROR) != 0U) {
            flags = flags | IoFlags::error;
        }
        if ((native.flags & EV_EOF) != 0U) {
            flags = flags | IoFlags::hangup;
        }
        events[static_cast<std::size_t>(index)] = {.token = token_value(native.udata), .flags = flags};
    }
    return static_cast<std::size_t>(ready);
}

} // namespace glifistore::server
