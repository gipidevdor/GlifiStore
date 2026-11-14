#include "persistence/adaptive_batch_sizer.hpp"
#include "test.hpp"

GLIFI_TEST("adaptive batch target contracts to deadline occupancy") {
    glifistore::detail::AdaptiveBatchSizer sizer;
    sizer.reset(1, 32);
    GLIFI_REQUIRE(sizer.target() == 32);

    sizer.observe_deadline(4);
    GLIFI_REQUIRE(sizer.target() == 4);
    sizer.observe_target_reached(4, 4);
    GLIFI_REQUIRE(sizer.target() == 4);
}

GLIFI_TEST("adaptive batch target grows to admitted burst within bounds") {
    glifistore::detail::AdaptiveBatchSizer sizer;
    sizer.reset(4, 32);
    sizer.observe_deadline(1);
    GLIFI_REQUIRE(sizer.target() == 4);

    sizer.observe_target_reached(4, 16);
    GLIFI_REQUIRE(sizer.target() == 16);
    sizer.observe_target_reached(16, 64);
    GLIFI_REQUIRE(sizer.target() == 32);
}
