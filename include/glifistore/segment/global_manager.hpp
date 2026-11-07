/**
 * @file
 * @brief Shared registry and allocation of Segment identities across Worker owners.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/segment/segment.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace glifistore {

/// Global Segment Manager: live catalog, deterministic snapshots, and retirement accounting.
class GlobalSegmentManager final {
  public:
    explicit GlobalSegmentManager(SegmentId first_id = SegmentId{1});

    [[nodiscard]] auto allocate_active(WorkerId owner) -> SegmentPtr;
    [[nodiscard]] auto prepare_segment(WorkerId owner) -> Result<SegmentPtr>;
    [[nodiscard]] auto prepare_rotation(const SegmentPtr& active, WorkerId owner) -> Result<SegmentPtr>;
    [[nodiscard]] auto commit_rotation(const SegmentPtr& active, const SegmentPtr& replacement) -> Status;
    [[nodiscard]] auto replace_sealed(std::span<const SegmentId> sources,
                                      std::span<const SegmentPtr> replacements) -> Status;
    [[nodiscard]] auto find(SegmentId id) -> SegmentPtr;
    [[nodiscard]] auto find(SegmentId id) const -> std::shared_ptr<const Segment>;
    [[nodiscard]] auto segments() const -> std::vector<SegmentPtr>;
    [[nodiscard]] auto retired_count() const -> std::size_t;
    [[nodiscard]] auto try_retire(SegmentId id) -> Status;

  private:
    [[nodiscard]] auto register_segment(SegmentPtr segment) -> SegmentPtr;

    SegmentId next_id_;
    std::unordered_map<SegmentId, SegmentPtr> catalog_;
    std::size_t retired_count_{};
    mutable std::mutex mutex_;
};

} // namespace glifistore
