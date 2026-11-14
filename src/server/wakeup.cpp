#include "glifistore/server/wakeup.hpp"

#include "system_error.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <unistd.h>

#if defined(__linux__)
#include <sys/eventfd.h>
#endif

namespace glifistore::server {

auto Wakeup::create() -> Result<Wakeup> {
#if defined(__linux__)
    SocketHandle descriptor{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
    if (!descriptor.valid()) {
        return system_error("eventfd");
    }
    return Wakeup{std::move(descriptor), {}};
#else
    std::array<int, 2> descriptors{};
    if (::pipe(descriptors.data()) != 0) {
        return system_error("pipe");
    }
    SocketHandle reader{descriptors[0]};
    SocketHandle writer{descriptors[1]};
    if (auto status = set_nonblocking(reader.descriptor()); !status) {
        return unexpected(status.error());
    }
    if (auto status = set_nonblocking(writer.descriptor()); !status) {
        return unexpected(status.error());
    }
    if (auto status = set_close_on_exec(reader.descriptor()); !status) {
        return unexpected(status.error());
    }
    if (auto status = set_close_on_exec(writer.descriptor()); !status) {
        return unexpected(status.error());
    }
    return Wakeup{std::move(reader), std::move(writer)};
#endif
}

auto Wakeup::notify() const -> Status {
#if defined(__linux__)
    const std::uint64_t increment = 1;
    const auto descriptor = reader_.descriptor();
    const auto* data = &increment;
    constexpr auto size = sizeof(increment);
#else
    const std::byte increment{1};
    const auto descriptor = writer_.descriptor();
    const auto* data = &increment;
    constexpr auto size = sizeof(increment);
#endif
    while (true) {
        if (::write(descriptor, data, size) == static_cast<ssize_t>(size)) {
            return {};
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return {};
        }
        if (errno != EINTR) {
            return system_error("wakeup write");
        }
    }
}

auto Wakeup::drain() const -> Status {
#if defined(__linux__)
    std::uint64_t value{};
    auto* data = &value;
    constexpr auto size = sizeof(value);
#else
    std::array<std::byte, 256> buffer{};
    auto* data = buffer.data();
    const auto size = buffer.size();
#endif
    while (true) {
        const auto received = ::read(reader_.descriptor(), data, size);
        if (received > 0) {
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return {};
        }
        if (received == 0) {
            return {};
        }
        return system_error("wakeup read");
    }
}

} // namespace glifistore::server
