#pragma once

#include "glifistore/core/key_hash.hpp"
#include "glifistore/segment/global_manager.hpp"
#include "glifistore/worker/worker.hpp"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace glifistore {

// Fixed-size worker set chosen at startup. Routing is hash(key) % size().
class WorkerPool final {
  public:
    WorkerPool(GlobalSegmentManager& manager, std::size_t worker_count,
               WorkerRoutingState routing = get_worker_routing());
    WorkerPool(const WorkerPool&) = delete;
    auto operator=(const WorkerPool&) -> WorkerPool& = delete;
    WorkerPool(WorkerPool&&) = delete;
    auto operator=(WorkerPool&&) -> WorkerPool& = delete;

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return workers_.size();
    }

    [[nodiscard]] auto worker(std::size_t index) const noexcept -> const Worker& {
        return *workers_[index];
    }

    [[nodiscard]] auto worker(std::size_t index) noexcept -> Worker& {
        return *workers_[index];
    }

    [[nodiscard]] auto route(const HashedKey& key) const noexcept -> const Worker& {
        return worker(route_worker(key.hash, workers_.size()));
    }

    [[nodiscard]] auto route(const HashedKey& key) noexcept -> Worker& {
        return worker(route_worker(key.hash, workers_.size()));
    }

  private:
    GlobalSegmentManager& manager_;
    std::vector<std::unique_ptr<Worker>> workers_;
};

} // namespace glifistore
