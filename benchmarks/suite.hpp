#pragma once

#include "harness.hpp"

#include <vector>

namespace glifistore::bench {

[[nodiscard]] auto run_benchmark(BenchmarkKind kind, const Config& config, const RunSettings& settings)
    -> std::vector<Result>;
[[nodiscard]] auto quick_configs() -> std::vector<Config>;
[[nodiscard]] auto suite_configs() -> std::vector<Config>;

} // namespace glifistore::bench
