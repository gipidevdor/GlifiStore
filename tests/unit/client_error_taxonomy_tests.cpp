#include "glifistore/client/client.hpp"
#include "test.hpp"

#include <cstdint>
#include <string_view>

namespace {

struct TaxonomyCase {
    std::uint16_t wire_status{};
    std::string_view category{};
    std::string_view read_retryability{};
    std::string_view mutation_outcome{};
    std::string_view mutation_retryability{};
    bool unhealthy{};
};

// Mirrors tests/fixtures/error_taxonomy_v1.json (GS-PROTO-ERROR-001).
constexpr TaxonomyCase kCases[] = {
    {1, "invalid_argument", "never", "rejected", "reconcile_first", false},
    {2, "invalid_argument", "never", "rejected", "reconcile_first", false},
    {3, "internal", "new_attempt", "indeterminate", "reconcile_first", false},
    {4, "not_found", "new_attempt", "rejected", "new_attempt", false},
    {5, "overloaded", "never", "rejected", "never", false},
    {6, "protocol", "new_attempt", "rejected", "reconcile_first", true},
    {7, "unavailable", "never", "rejected", "never", true},
    {8, "permission_denied", "never", "rejected", "never", false},
    {99, "protocol", "new_attempt", "indeterminate", "reconcile_first", false},
};

} // namespace

GLIFI_TEST("error taxonomy maps wire status to category and retryability") {
    for (const auto& expected : kCases) {
        const auto error = glifistore::client::error_from_wire_status(expected.wire_status);
        GLIFI_REQUIRE(error.category == expected.category);
        GLIFI_REQUIRE(error.wire_status.has_value());
        GLIFI_REQUIRE(*error.wire_status == expected.wire_status);
        GLIFI_REQUIRE(error.retryability == expected.read_retryability);
        // Read-path mapping leaves mutation_outcome empty; fixture outcome is via helper.
        GLIFI_REQUIRE(error.mutation_outcome.empty());
        GLIFI_REQUIRE(glifistore::client::portable_mutation_outcome(expected.wire_status) ==
                      expected.mutation_outcome);

        const bool indeterminate = expected.mutation_outcome == "indeterminate";
        const auto mutation_retry = glifistore::client::portable_retryability(
            expected.category, /*mutation_sent=*/true, indeterminate);
        GLIFI_REQUIRE(mutation_retry == expected.mutation_retryability);

        // Unhealthy statuses are WRONG_OWNER / NOT_BOUND (client marks session unusable).
        GLIFI_REQUIRE(expected.unhealthy == (expected.wire_status == 6 || expected.wire_status == 7));
    }
}
