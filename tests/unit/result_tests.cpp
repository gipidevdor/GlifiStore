#include "glifistore/core/error.hpp"
#include "test.hpp"

#include <optional>

GLIFI_TEST("failed void Result exposes its error") {
    glifistore::Status failure =
        glifistore::fail(glifistore::ErrorCode::invalid_argument, "expected failure");
    GLIFI_REQUIRE(failure.error().code == glifistore::ErrorCode::invalid_argument);
    GLIFI_REQUIRE(failure.error().message == "expected failure");
}

GLIFI_TEST("successful void Result rejects error access without undefined behavior") {
    glifistore::Status success{};
    bool rejected{};
    try {
        static_cast<void>(success.error());
    } catch (const std::bad_optional_access&) {
        rejected = true;
    }
    GLIFI_REQUIRE(rejected);
}
