#include "glifistore/persistence/runtime_catalog.hpp"
#include "persistence/blob_storage.hpp"

#include <limits>
#include <mutex>

namespace glifistore {
auto DurableRuntimeCatalog::allocate_blob_id() -> Result<BlobId> {
    std::unique_lock lock{catalog_mutex_};
    if (!healthy() || manifest_.format_version != 2 || !options_.blob_storage) {
        return fail(ErrorCode::unavailable, "blob identity allocator unavailable");
    }
    if (manifest_.manifest_generation == std::numeric_limits<std::uint64_t>::max()) {
        return fail(ErrorCode::arithmetic_overflow, "Manifest generation exhausted");
    }
    auto next = manifest_;
    bool carry = true;
    for (auto& byte : next.next_blob_id) {
        if (!carry) {
            break;
        }
        const auto value = std::to_integer<unsigned>(byte) + 1U;
        byte = static_cast<std::byte>(value & 255U);
        carry = value > 255U;
    }
    if (carry) {
        return fail(ErrorCode::arithmetic_overflow, "blob identity space exhausted");
    }
    ++next.manifest_generation;
    const auto id = manifest_.next_blob_id;
    const auto published = directory_.publish_manifest(next, options_.limits.max_manifest_bytes);
    if (!published.durable()) {
        healthy_ = false;
        return unexpected(
            published.error.value_or(Error{ErrorCode::io_error, "blob reservation publication failed"}));
    }
    manifest_ = std::move(next);
    return id;
}

auto DurableRuntimeCatalog::blob_roots() -> Result<std::vector<BlobId>> {
    std::shared_lock lock{catalog_mutex_};
    if (!healthy() || !options_.blob_storage) {
        return fail(ErrorCode::unavailable, "blob catalog unavailable");
    }
    auto roots = detail::verify_blob_closure(directory_, manifest_, *options_.blob_storage);
    if (!roots) {
        healthy_ = false;
    }
    return roots;
}
} // namespace glifistore
