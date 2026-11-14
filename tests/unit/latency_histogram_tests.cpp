#include "glifistore/server/latency_histogram.hpp"
#include "test.hpp"

#include <string>

GLIFI_TEST("latency histogram buckets and approximate percentiles") {
    glifistore::server::LatencyHistogram histogram{};
    histogram.observe(500);        // <= 1 us
    histogram.observe(5'000);      // <= 10 us
    histogram.observe(50'000);     // <= 100 us
    histogram.observe(5'000'000);  // <= 10 ms
    histogram.observe(50'000'000); // <= 100 ms

    GLIFI_REQUIRE(histogram.observations == 5);
    GLIFI_REQUIRE(histogram.sum_ns == 55'055'500);
    const auto cumulative = histogram.cumulative();
    GLIFI_REQUIRE(cumulative[0] == 1);
    GLIFI_REQUIRE(cumulative[1] == 2);
    GLIFI_REQUIRE(cumulative[2] == 3);
    GLIFI_REQUIRE(cumulative[4] == 4);
    GLIFI_REQUIRE(cumulative[5] == 5);
    GLIFI_REQUIRE(cumulative.back() == 5);

    const auto p50 = histogram.approximate_percentile_ns(0.50);
    const auto p99 = histogram.approximate_percentile_ns(0.99);
    GLIFI_REQUIRE(p50 >= 10'000);
    GLIFI_REQUIRE(p50 <= 100'000);
    GLIFI_REQUIRE(p99 >= p50);
    GLIFI_REQUIRE(p99 <= 100'000'000);

    std::string exported;
    glifistore::server::append_latency_histogram(exported, "lane[0].service_ns", histogram);
    GLIFI_REQUIRE(exported.find("lane[0].service_ns.count=5\n") != std::string::npos);
    GLIFI_REQUIRE(exported.find("lane[0].service_ns.le_1000=1\n") != std::string::npos);
    GLIFI_REQUIRE(exported.find("lane[0].service_ns.le_inf=5\n") != std::string::npos);
    GLIFI_REQUIRE(exported.find("lane[0].service_ns.p50=") != std::string::npos);
    GLIFI_REQUIRE(exported.find("lane[0].service_ns.p99=") != std::string::npos);
}

GLIFI_TEST("empty latency histogram exports zeros") {
    glifistore::server::LatencyHistogram histogram{};
    GLIFI_REQUIRE(histogram.approximate_percentile_ns(0.50) == 0);
    GLIFI_REQUIRE(histogram.approximate_percentile_ns(0.99) == 0);
    std::string exported;
    glifistore::server::append_latency_histogram(exported, "empty", histogram);
    GLIFI_REQUIRE(exported.find("empty.count=0\n") != std::string::npos);
    GLIFI_REQUIRE(exported.find("empty.p50=0\n") != std::string::npos);
    GLIFI_REQUIRE(exported.find("empty.p99=0\n") != std::string::npos);
}
