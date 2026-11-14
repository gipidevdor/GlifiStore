#include "glifistore/server/mutation_window.hpp"
#include "test.hpp"

using glifistore::server::kMaximumMutationWindow;
using glifistore::server::MutationVisibilityBarrier;

GLIFI_TEST("mutation window visibility barrier enforces RAW epoch") {
    MutationVisibilityBarrier barrier{};
    GLIFI_REQUIRE(!barrier.armed());
    GLIFI_REQUIRE(barrier.allows(0));
    barrier.raise_to(7);
    GLIFI_REQUIRE(barrier.armed());
    GLIFI_REQUIRE(!barrier.allows(6));
    GLIFI_REQUIRE(barrier.allows(7));
    GLIFI_REQUIRE(barrier.allows(9));
    barrier.clear();
    GLIFI_REQUIRE(!barrier.armed());
    GLIFI_REQUIRE(kMaximumMutationWindow == 32);
}
