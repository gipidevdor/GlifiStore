#include "glifistore/server/openbsd_sandbox.hpp"

#include <cerrno>
#include <string>
#include <system_error>

#if defined(__OpenBSD__)
#include <unistd.h>
#endif

namespace glifistore::server {
namespace {

[[nodiscard]] auto absolute_existing(const std::filesystem::path& path) -> Result<std::filesystem::path> {
    if (path.empty()) {
        return fail(ErrorCode::invalid_argument, "openbsd sandbox path is empty");
    }
    std::error_code ec;
    auto absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        return fail(ErrorCode::io_error, "openbsd sandbox cannot absolutize path: " + ec.message());
    }
    auto canonical = std::filesystem::weakly_canonical(absolute, ec);
    if (!ec) {
        absolute = std::move(canonical);
    }
    if (!std::filesystem::exists(absolute)) {
        return fail(ErrorCode::not_found, "openbsd sandbox path does not exist: " + absolute.string());
    }
    return absolute;
}

[[nodiscard]] auto push_unveil(OpenbsdSandboxPlan& plan, const std::filesystem::path& path,
                               std::string_view permissions) -> Status {
    if (path.empty()) {
        return {};
    }
    auto resolved = absolute_existing(path);
    if (!resolved) {
        return unexpected(std::move(resolved.error()));
    }
    for (const auto& existing : plan.unveils) {
        if (existing.path == *resolved) {
            return {};
        }
    }
    plan.unveils.push_back(
        OpenbsdUnveilEntry{.path = std::move(*resolved), .permissions = std::string{permissions}});
    return {};
}

} // namespace

auto openbsd_sandbox_supported() noexcept -> bool {
#if defined(__OpenBSD__)
    return true;
#else
    return false;
#endif
}

auto plan_openbsd_sandbox(const DaemonOptions& options) -> Result<OpenbsdSandboxPlan> {
    OpenbsdSandboxPlan plan{};
    plan.available = openbsd_sandbox_supported();
    plan.promises = std::string{openbsd_sandbox_promises()};
    if (!plan.available) {
        return plan;
    }

    if (options.store.data_directory.has_value()) {
        if (auto status = push_unveil(plan, *options.store.data_directory, "rwc"); !status) {
            return unexpected(std::move(status.error()));
        }
    }
    if (auto status = push_unveil(plan, options.server.tls.certificate_file, "r"); !status) {
        return unexpected(std::move(status.error()));
    }
    if (auto status = push_unveil(plan, options.server.tls.private_key_file, "r"); !status) {
        return unexpected(std::move(status.error()));
    }
    if (auto status = push_unveil(plan, options.server.tls.client_ca_file, "r"); !status) {
        return unexpected(std::move(status.error()));
    }
    if (auto status = push_unveil(plan, options.server.tls.crl_file, "r"); !status) {
        return unexpected(std::move(status.error()));
    }
    if (auto status = push_unveil(plan, options.authz_map_path, "r"); !status) {
        return unexpected(std::move(status.error()));
    }
    if (!options.server.unix_socket_path.empty()) {
        // Parent directory must be unveiled for unlink on shutdown; the socket
        // file itself may not exist yet at plan time (bound during Server::create).
        const auto parent = options.server.unix_socket_path.parent_path().empty()
                                ? std::filesystem::path{"."}
                                : options.server.unix_socket_path.parent_path();
        if (auto status = push_unveil(plan, parent, "rwc"); !status) {
            return unexpected(std::move(status.error()));
        }
    }
    return plan;
}

auto apply_openbsd_sandbox(const DaemonOptions& options) -> Status {
    auto planned = plan_openbsd_sandbox(options);
    if (!planned) {
        return unexpected(std::move(planned.error()));
    }
    if (!planned->available) {
        return {};
    }

#if defined(__OpenBSD__)
    for (const auto& entry : planned->unveils) {
        if (::unveil(entry.path.c_str(), entry.permissions.c_str()) != 0) {
            const auto error_number = errno;
            return fail(ErrorCode::io_error,
                        "unveil(" + entry.path.string() + ", " + entry.permissions +
                            ") failed: " + std::error_code{error_number, std::system_category()}.message());
        }
    }
    if (::unveil(nullptr, nullptr) != 0) {
        const auto error_number = errno;
        return fail(ErrorCode::io_error, std::string{"unveil lock failed: "} +
                                             std::error_code{error_number, std::system_category()}.message());
    }
    if (::pledge(planned->promises.c_str(), nullptr) != 0) {
        const auto error_number = errno;
        return fail(ErrorCode::io_error, std::string{"pledge("} + planned->promises + ") failed: " +
                                             std::error_code{error_number, std::system_category()}.message());
    }
#else
    (void)options;
#endif
    return {};
}

} // namespace glifistore::server
