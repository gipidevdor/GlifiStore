#include "glifistore/server/peercred.hpp"

#include "system_error.hpp"

#include <cerrno>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/un.h>
#endif

namespace glifistore::server {
namespace {

[[nodiscard]] auto format_uid_principal(const std::uint32_t uid) -> std::string {
    return std::string{peercred_principal_prefix()} + std::to_string(uid);
}

} // namespace

auto peercred_supported() noexcept -> bool {
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    return true;
#else
    return false;
#endif
}

auto peer_credentials(const int descriptor) -> Result<PeerCredentials> {
    if (descriptor < 0) {
        return fail(ErrorCode::invalid_argument, "peer credentials require a valid socket");
    }

#if defined(__linux__)
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
        return system_error("getsockopt(SO_PEERCRED)");
    }
    if (length < sizeof(credentials)) {
        return fail(ErrorCode::io_error, "getsockopt(SO_PEERCRED) returned truncated credentials");
    }
    return PeerCredentials{
        .uid = static_cast<std::uint32_t>(credentials.uid),
        .gid = static_cast<std::uint32_t>(credentials.gid),
        .pid = static_cast<std::uint32_t>(credentials.pid),
    };
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(descriptor, &uid, &gid) != 0) {
        return system_error("getpeereid");
    }
    PeerCredentials credentials{
        .uid = static_cast<std::uint32_t>(uid),
        .gid = static_cast<std::uint32_t>(gid),
        .pid = 0,
    };
#if defined(LOCAL_PEEREPID)
    pid_t peer_pid = 0;
    socklen_t pid_length = sizeof(peer_pid);
    if (::getsockopt(descriptor, SOL_LOCAL, LOCAL_PEEREPID, &peer_pid, &pid_length) == 0 &&
        pid_length == sizeof(peer_pid) && peer_pid > 0) {
        credentials.pid = static_cast<std::uint32_t>(peer_pid);
    }
#endif
    return credentials;
#else
    static_cast<void>(descriptor);
    return fail(ErrorCode::unavailable, "Unix peer credentials are unsupported on this platform");
#endif
}

auto peercred_principal(const PeerCredentials& credentials) -> std::string {
    return format_uid_principal(credentials.uid);
}

} // namespace glifistore::server
