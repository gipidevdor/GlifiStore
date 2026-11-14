#include "glifistore/segment/global_manager.hpp"

#include <algorithm>
#include <limits>
#include <mutex>

namespace glifistore {

GlobalSegmentManager::GlobalSegmentManager(const SegmentId first_id) : next_id_(first_id) {}

auto GlobalSegmentManager::register_segment(SegmentPtr segment) -> SegmentPtr {
    catalog_.insert_or_assign(segment->id(), segment);
    return segment;
}

auto GlobalSegmentManager::allocate_active(const WorkerId owner) -> SegmentPtr {
    const std::lock_guard lock{mutex_};
    auto segment = register_segment(std::make_shared<Segment>(next_id_, owner));
    ++next_id_.value;
    return segment;
}

auto GlobalSegmentManager::prepare_segment(const WorkerId owner) -> Result<SegmentPtr> {
    const std::lock_guard lock{mutex_};
    if (next_id_.value == std::numeric_limits<std::uint64_t>::max()) {
        return fail(ErrorCode::arithmetic_overflow, "Segment identity space is exhausted");
    }
    auto segment = std::make_shared<Segment>(next_id_, owner);
    ++next_id_.value;
    return segment;
}

auto GlobalSegmentManager::prepare_rotation(const SegmentPtr& active, const WorkerId owner)
    -> Result<SegmentPtr> {
    const std::lock_guard lock{mutex_};
    if (!active || active->state() != SegmentState::active) {
        return fail(ErrorCode::invalid_argument, "rotation preparation requires an active segment");
    }
    const auto found = catalog_.find(active->id());
    if (found == catalog_.end() || found->second != active || active->owner() != owner) {
        return fail(ErrorCode::invalid_reference, "rotation preparation requires the catalog active owner");
    }
    if (next_id_.value == std::numeric_limits<std::uint64_t>::max()) {
        return fail(ErrorCode::arithmetic_overflow, "Segment identity space is exhausted");
    }
    auto segment = std::make_shared<Segment>(next_id_, owner);
    ++next_id_.value;
    return segment;
}

auto GlobalSegmentManager::commit_rotation(const SegmentPtr& active, const SegmentPtr& replacement)
    -> Status {
    const std::lock_guard lock{mutex_};
    if (!active || !replacement || active->state() != SegmentState::active ||
        replacement->state() != SegmentState::active || active->owner() != replacement->owner()) {
        return fail(ErrorCode::invalid_argument, "rotation commit requires matching active segments");
    }
    const auto found = catalog_.find(active->id());
    if (found == catalog_.end() || found->second != active || catalog_.contains(replacement->id())) {
        return fail(ErrorCode::invalid_reference, "rotation commit disagrees with the live catalog");
    }
    static_cast<void>(register_segment(replacement));
    if (auto sealed = active->seal(); !sealed) {
        catalog_.erase(replacement->id());
        return sealed;
    }
    return {};
}

auto GlobalSegmentManager::replace_sealed(const std::span<const SegmentId> sources,
                                          const std::span<const SegmentPtr> replacements) -> Status {
    const std::lock_guard lock{mutex_};
    if (sources.empty()) {
        return fail(ErrorCode::invalid_argument, "vacuum replacement requires at least one source Segment");
    }
    WorkerId owner{};
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto found = catalog_.find(sources[index]);
        if (found == catalog_.end() || found->second->state() != SegmentState::sealed ||
            found->second->stats().live_records != 0) {
            return fail(ErrorCode::invalid_argument,
                        "vacuum replacement requires zero-live sealed source Segments");
        }
        if (index == 0) {
            owner = found->second->owner();
        } else if (found->second->owner() != owner) {
            return fail(ErrorCode::invalid_argument, "vacuum replacement sources must belong to one Worker");
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (sources[previous] == sources[index]) {
                return fail(ErrorCode::invalid_argument,
                            "vacuum replacement contains a duplicate source Segment identity");
            }
        }
    }
    for (std::size_t index = 0; index < replacements.size(); ++index) {
        const auto& replacement = replacements[index];
        if (!replacement || replacement->state() != SegmentState::sealed || replacement->owner() != owner ||
            catalog_.contains(replacement->id())) {
            return fail(ErrorCode::invalid_argument,
                        "vacuum publication requires detached sealed replacement Segments");
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (replacements[previous]->id() == replacement->id()) {
                return fail(ErrorCode::invalid_argument,
                            "vacuum publication contains a duplicate Segment identity");
            }
        }
    }

    std::size_t published{};
    try {
        for (; published < replacements.size(); ++published) {
            static_cast<void>(register_segment(replacements[published]));
        }
    } catch (...) {
        for (std::size_t rollback = 0; rollback < published; ++rollback) {
            catalog_.erase(replacements[rollback]->id());
        }
        throw;
    }
    for (const auto id : sources) {
        const auto found = catalog_.find(id);
        static_cast<void>(found->second->retire());
        catalog_.erase(found);
        ++retired_count_;
    }
    return {};
}

auto GlobalSegmentManager::find(const SegmentId id) const -> std::shared_ptr<const Segment> {
    const std::lock_guard lock{mutex_};
    const auto found = catalog_.find(id);
    return found == catalog_.end() ? nullptr : found->second;
}

auto GlobalSegmentManager::find(const SegmentId id) -> SegmentPtr {
    const std::lock_guard lock{mutex_};
    const auto found = catalog_.find(id);
    return found == catalog_.end() ? nullptr : found->second;
}

auto GlobalSegmentManager::segments() const -> std::vector<SegmentPtr> {
    const std::lock_guard lock{mutex_};
    std::vector<SegmentPtr> segments;
    segments.reserve(catalog_.size());
    for (const auto& [id, segment] : catalog_) {
        static_cast<void>(id);
        segments.push_back(segment);
    }
    std::ranges::sort(segments, {}, [](const SegmentPtr& segment) { return segment->id().value; });
    return segments;
}

auto GlobalSegmentManager::retired_count() const -> std::size_t {
    const std::lock_guard lock{mutex_};
    return retired_count_;
}

auto GlobalSegmentManager::try_retire(const SegmentId id) -> Status {
    const std::lock_guard lock{mutex_};
    const auto found = catalog_.find(id);
    if (found == catalog_.end()) {
        return fail(ErrorCode::invalid_reference, "segment is not in the catalog");
    }
    if (auto retired = found->second->retire(); !retired) {
        return retired;
    }
    catalog_.erase(found);
    ++retired_count_;
    return {};
}

} // namespace glifistore
