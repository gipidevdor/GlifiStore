#include "glifistore/server/openbsd_sandbox.hpp"
#include "test.hpp"

#include <filesystem>
#include <fstream>
#include <string_view>

GLIFI_TEST("openbsd sandbox promises are stable") {
    const auto promises = glifistore::server::openbsd_sandbox_promises();
    GLIFI_REQUIRE(promises.find("stdio") != std::string_view::npos);
    GLIFI_REQUIRE(promises.find("inet") != std::string_view::npos);
    GLIFI_REQUIRE(promises.find("unix") != std::string_view::npos);
    GLIFI_REQUIRE(promises.find("rpath") != std::string_view::npos);
    GLIFI_REQUIRE(promises.find("dns") == std::string_view::npos);
    GLIFI_REQUIRE(promises.find("exec") == std::string_view::npos);
    GLIFI_REQUIRE(promises.find("proc") == std::string_view::npos);
}

GLIFI_TEST("openbsd sandbox support matches host") {
#if defined(__OpenBSD__)
    GLIFI_REQUIRE(glifistore::server::openbsd_sandbox_supported());
#else
    GLIFI_REQUIRE(!glifistore::server::openbsd_sandbox_supported());
#endif
}

GLIFI_TEST("openbsd sandbox plan is no-op off OpenBSD") {
    glifistore::server::DaemonOptions options{};
    auto planned = glifistore::server::plan_openbsd_sandbox(options);
    GLIFI_REQUIRE(planned.has_value());
#if defined(__OpenBSD__)
    GLIFI_REQUIRE(planned->available);
#else
    GLIFI_REQUIRE(!planned->available);
    GLIFI_REQUIRE(planned->unveils.empty());
#endif
    GLIFI_REQUIRE(planned->promises == glifistore::server::openbsd_sandbox_promises());
}

#if defined(__OpenBSD__)
namespace {

auto write_temp_file(const std::filesystem::path& path, std::string_view contents) -> void {
    std::ofstream out{path};
    out << contents;
    GLIFI_REQUIRE(out.good());
}

} // namespace

GLIFI_TEST("openbsd sandbox plan unveils data dir and tls paths when present") {
    const auto root = std::filesystem::temp_directory_path() / "glifistore-openbsd-sandbox-plan";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto data = root / "data";
    const auto cert = root / "server.crt";
    const auto key = root / "server.key";
    const auto ca = root / "client-ca.crt";
    const auto authz = root / "authz.map";
    std::filesystem::create_directories(data);
    write_temp_file(cert, "cert");
    write_temp_file(key, "key");
    write_temp_file(ca, "ca");
    write_temp_file(authz, "principal=* read,write\n");

    glifistore::server::DaemonOptions options{};
    options.store.data_directory = data;
    options.server.tls.certificate_file = cert;
    options.server.tls.private_key_file = key;
    options.server.tls.client_ca_file = ca;
    options.authz_map_path = authz;

    auto planned = glifistore::server::plan_openbsd_sandbox(options);
    GLIFI_REQUIRE(planned.has_value());
    GLIFI_REQUIRE(planned->available);
    GLIFI_REQUIRE(planned->unveils.size() == 5);
    bool saw_data = false;
    bool saw_cert = false;
    for (const auto& entry : planned->unveils) {
        if (entry.path == std::filesystem::weakly_canonical(std::filesystem::absolute(data))) {
            GLIFI_REQUIRE(entry.permissions == "rwc");
            saw_data = true;
        }
        if (entry.path == std::filesystem::weakly_canonical(std::filesystem::absolute(cert))) {
            GLIFI_REQUIRE(entry.permissions == "r");
            saw_cert = true;
        }
    }
    GLIFI_REQUIRE(saw_data);
    GLIFI_REQUIRE(saw_cert);

    std::filesystem::remove_all(root);
}
#endif

GLIFI_TEST("apply openbsd sandbox is success no-op off OpenBSD") {
#if defined(__OpenBSD__)
    // Real unveil/pledge is exercised by scripts/ci-openbsd-libressl.sh after
    // Server::create opens the data directory; unit tests stay path-only here.
#else
    const auto status = glifistore::server::apply_openbsd_sandbox({});
    GLIFI_REQUIRE(status.has_value());
#endif
}
