#include "glifistore/index/key_arena.hpp"

#include "glifistore/core/checked_math.hpp"

#include <cstring>

namespace glifistore {

auto KeyArena::allocate(const std::size_t size) -> Result<std::uint32_t> {
    if (size == 0) {
        return std::uint32_t{0};
    }
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        return fail(ErrorCode::record_too_large, "index key exceeds arena offset limits");
    }
    const auto needed = checked_add(bump_, size);
    if (!needed) {
        return unexpected(needed.error());
    }
    if (*needed > std::numeric_limits<std::uint32_t>::max()) {
        return fail(ErrorCode::arithmetic_overflow, "index key arena offset overflow");
    }
    if (*needed > storage_.size()) {
        storage_.resize(*needed);
    }
    const auto offset = static_cast<std::uint32_t>(bump_);
    bump_ = *needed;
    return offset;
}

auto KeyArena::data(const std::uint32_t offset, const std::size_t size) const -> std::span<const std::byte> {
    if (size == 0) {
        return {};
    }
    const auto end = checked_add<std::size_t>(offset, size);
    if (!end || *end > storage_.size()) {
        return {};
    }
    return {storage_.data() + offset, size};
}

auto KeyArena::allocated_bytes() const noexcept -> std::size_t {
    return bump_;
}

void KeyArena::clear() noexcept {
    storage_.clear();
    bump_ = 0;
}

auto KeyArena::reserve(const std::size_t bytes) -> Status {
    if (bytes > std::numeric_limits<std::uint32_t>::max()) {
        return fail(ErrorCode::arithmetic_overflow, "index key arena reserve exceeds offset limits");
    }
    storage_.reserve(bytes);
    return {};
}

auto KeyArena::prepare_allocate(const std::size_t size) -> Status {
    const auto needed = checked_add(bump_, size);
    if (!needed) {
        return unexpected(needed.error());
    }
    return reserve(*needed);
}

} // namespace glifistore
