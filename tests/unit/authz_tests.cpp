#include "glifistore/server/authz.hpp"
#include "glifistore/server/protocol.hpp"
#include "test.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] auto key_bytes(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return out;
}

} // namespace

GLIFI_TEST("authz write implies read and admin implies write") {
    using glifistore::server::Capability;
    using glifistore::server::has_capability;
    using glifistore::server::normalize_capabilities;

    const auto write_only = normalize_capabilities(Capability::write);
    GLIFI_REQUIRE(has_capability(write_only, Capability::write));
    GLIFI_REQUIRE(has_capability(write_only, Capability::read));

    const auto admin_only = normalize_capabilities(Capability::admin);
    GLIFI_REQUIRE(has_capability(admin_only, Capability::admin));
    GLIFI_REQUIRE(has_capability(admin_only, Capability::write));
    GLIFI_REQUIRE(has_capability(admin_only, Capability::read));
}

GLIFI_TEST("authz map parses principals and default-denies unknowns") {
    const auto text = R"(
# reader
reader.example read
writer.example write
admin.example admin
)";
    auto policy = glifistore::server::AuthzPolicy::parse(text);
    GLIFI_REQUIRE(policy.has_value());
    GLIFI_REQUIRE(policy->enabled());
    GLIFI_REQUIRE(policy->size() == 3);

    using glifistore::server::authorize_opcode;
    using glifistore::server::Capability;
    using glifistore::server::has_capability;
    using glifistore::server::RequestOpcode;

    const auto reader = policy->capabilities_for("reader.example");
    GLIFI_REQUIRE(has_capability(reader, Capability::read));
    GLIFI_REQUIRE(!has_capability(reader, Capability::write));
    GLIFI_REQUIRE(authorize_opcode(*policy, reader, RequestOpcode::get));
    GLIFI_REQUIRE(authorize_opcode(*policy, reader, RequestOpcode::ping));
    GLIFI_REQUIRE(!authorize_opcode(*policy, reader, RequestOpcode::put));
    GLIFI_REQUIRE(authorize_opcode(*policy, reader, RequestOpcode::init));
    GLIFI_REQUIRE(authorize_opcode(*policy, reader, RequestOpcode::health));

    const auto writer = policy->capabilities_for("writer.example");
    GLIFI_REQUIRE(authorize_opcode(*policy, writer, RequestOpcode::put));
    GLIFI_REQUIRE(authorize_opcode(*policy, writer, RequestOpcode::get));
    GLIFI_REQUIRE(authorize_opcode(*policy, writer, RequestOpcode::erase));

    const auto unknown = policy->capabilities_for("nobody");
    GLIFI_REQUIRE(unknown == Capability::none);
    GLIFI_REQUIRE(!authorize_opcode(*policy, unknown, RequestOpcode::get));
    GLIFI_REQUIRE(authorize_opcode(*policy, unknown, RequestOpcode::ready));
}

GLIFI_TEST("authz map rejects unknown capability and duplicates") {
    const auto bad_cap = glifistore::server::AuthzPolicy::parse("alice mutate");
    GLIFI_REQUIRE(!bad_cap.has_value());

    const auto duplicate = glifistore::server::AuthzPolicy::parse("alice read\nalice write\n");
    GLIFI_REQUIRE(!duplicate.has_value());
}

GLIFI_TEST("authz disabled policy allows all opcodes") {
    glifistore::server::AuthzPolicy policy;
    GLIFI_REQUIRE(!policy.enabled());
    GLIFI_REQUIRE(glifistore::server::authorize_opcode(policy, glifistore::server::Capability::none,
                                                       glifistore::server::RequestOpcode::put));
}

GLIFI_TEST("authz map parses optional key prefix and rejects empty prefix") {
    auto policy = glifistore::server::AuthzPolicy::parse(
        "tenant-a write prefix=a/\ntenant-b read prefix=b/\nshared write\n");
    GLIFI_REQUIRE(policy.has_value());
    GLIFI_REQUIRE(policy->size() == 3);
    GLIFI_REQUIRE(policy->prefix_scoped_count() == 2);
    GLIFI_REQUIRE(policy->key_prefix_for("tenant-a") == "a/");
    GLIFI_REQUIRE(policy->key_prefix_for("tenant-b") == "b/");
    GLIFI_REQUIRE(policy->key_prefix_for("shared").empty());

    const auto empty_prefix = glifistore::server::AuthzPolicy::parse("alice write prefix=");
    GLIFI_REQUIRE(!empty_prefix.has_value());

    const auto junk = glifistore::server::AuthzPolicy::parse("alice write scope=a/");
    GLIFI_REQUIRE(!junk.has_value());
}

GLIFI_TEST("authz key prefix denies cross-tenant GET PUT ERASE and allows in-prefix") {
    using glifistore::server::authorize_request;
    using glifistore::server::Capability;
    using glifistore::server::RequestOpcode;

    auto policy = glifistore::server::AuthzPolicy::parse(
        "tenant-a write prefix=tenant-a/\ntenant-b write prefix=tenant-b/\n");
    GLIFI_REQUIRE(policy.has_value());

    const auto grant_a = policy->grant_for("tenant-a");
    const auto in_a = key_bytes("tenant-a/orders/1");
    const auto in_b = key_bytes("tenant-b/orders/1");
    const auto bare = key_bytes("other");

    GLIFI_REQUIRE(
        authorize_request(*policy, grant_a.capabilities, grant_a.key_prefix, RequestOpcode::get, in_a));
    GLIFI_REQUIRE(
        authorize_request(*policy, grant_a.capabilities, grant_a.key_prefix, RequestOpcode::put, in_a));
    GLIFI_REQUIRE(
        authorize_request(*policy, grant_a.capabilities, grant_a.key_prefix, RequestOpcode::erase, in_a));

    GLIFI_REQUIRE(
        !authorize_request(*policy, grant_a.capabilities, grant_a.key_prefix, RequestOpcode::get, in_b));
    GLIFI_REQUIRE(
        !authorize_request(*policy, grant_a.capabilities, grant_a.key_prefix, RequestOpcode::put, in_b));
    GLIFI_REQUIRE(
        !authorize_request(*policy, grant_a.capabilities, grant_a.key_prefix, RequestOpcode::erase, bare));

    // Prefix does not gate lifecycle / ping; STATS requires admin for prefix tenants (ADR 0027).
    GLIFI_REQUIRE(
        authorize_request(*policy, grant_a.capabilities, grant_a.key_prefix, RequestOpcode::ping, in_b));
    GLIFI_REQUIRE(
        !authorize_request(*policy, grant_a.capabilities, grant_a.key_prefix, RequestOpcode::stats, bare));
    GLIFI_REQUIRE(authorize_request(*policy, grant_a.capabilities, grant_a.key_prefix, RequestOpcode::health,
                                    std::span<const std::byte>{}));

    glifistore::server::AuthzPolicy admin_policy;
    admin_policy.bind("tenant-a", Capability::admin, "tenant-a/");
    const auto admin_grant = admin_policy.grant_for("tenant-a");
    GLIFI_REQUIRE(authorize_request(admin_policy, admin_grant.capabilities, admin_grant.key_prefix,
                                    RequestOpcode::stats, bare));

    // Exact-prefix boundary: prefix alone is allowed; shorter key denied.
    const auto exact = key_bytes("tenant-a/");
    const auto short_key = key_bytes("tenant-a");
    GLIFI_REQUIRE(
        authorize_request(*policy, grant_a.capabilities, grant_a.key_prefix, RequestOpcode::get, exact));
    GLIFI_REQUIRE(
        !authorize_request(*policy, grant_a.capabilities, grant_a.key_prefix, RequestOpcode::get, short_key));
}

GLIFI_TEST("authz unrestricted principal keeps whole-keyspace access") {
    using glifistore::server::authorize_request;
    using glifistore::server::Capability;
    using glifistore::server::RequestOpcode;

    glifistore::server::AuthzPolicy policy;
    policy.bind("ops", Capability::write);
    const auto grant = policy.grant_for("ops");
    GLIFI_REQUIRE(grant.key_prefix.empty());
    const auto foreign = key_bytes("anyone/key");
    GLIFI_REQUIRE(
        authorize_request(policy, grant.capabilities, grant.key_prefix, RequestOpcode::get, foreign));
    GLIFI_REQUIRE(
        authorize_request(policy, grant.capabilities, grant.key_prefix, RequestOpcode::stats, foreign));
}

GLIFI_TEST("authz prefix-scoped STATS requires admin capability") {
    using glifistore::server::authorize_opcode;
    using glifistore::server::Capability;
    using glifistore::server::RequestOpcode;
    using glifistore::server::required_capability;

    GLIFI_REQUIRE(required_capability(RequestOpcode::stats, {}) == Capability::read);
    GLIFI_REQUIRE(required_capability(RequestOpcode::stats, "t/") == Capability::admin);

    glifistore::server::AuthzPolicy policy;
    policy.bind("tenant", Capability::write, "tenant/");
    const auto grant = policy.grant_for("tenant");
    GLIFI_REQUIRE(!authorize_opcode(policy, grant.capabilities, RequestOpcode::stats, grant.key_prefix));
    GLIFI_REQUIRE(authorize_opcode(policy, Capability::admin, RequestOpcode::stats, "tenant/"));
}
