#include "persistence/blob_storage.hpp"
#include "store/store_impl.hpp"

namespace glifistore {
auto Store::begin_blob_put(const std::string_view key, BlobOptions options)
    -> Result<std::unique_ptr<BlobWriter>> try {
    Impl::OperationGuard operation{*impl_, impl_->control_shard()};
    if (!operation || !impl_->blobs) {
        return fail(ErrorCode::unavailable, "blob profile is not open");
    }
    return impl_->blobs->begin(key, options,
                               route_worker(hash_key_routing(key, impl_->routing), worker_count()));
} catch (const std::bad_alloc&) {

    return store_detail::resource_exhausted();
} catch (...) {
    return store_detail::internal_failure();
}

auto Store::open_blob(const std::string_view key) -> Result<std::unique_ptr<BlobReader>> try {
    Impl::OperationGuard operation{*impl_, impl_->control_shard()};
    if (!operation || !impl_->blobs || !impl_->blobs->healthy()) {
        return fail(ErrorCode::unavailable, "blob profile is not open");
    }
    auto snapshot = impl_->blobs->snapshot_lock();
    auto value = get_record(key);
    if (!value) {
        return unexpected(value.error());
    }
    const auto shard = route_worker(hash_key_routing(key, impl_->routing), worker_count());
    return impl_->blobs->reader(std::move(*value), shard);
} catch (const std::bad_alloc&) {

    return store_detail::resource_exhausted();
} catch (...) {
    return store_detail::internal_failure();
}

auto Store::get_copy(const std::string_view key) -> Result<OwnedValue> try {
    if (!impl_->blobs) {
        return get_record(key);
    }
    Impl::OperationGuard operation{*impl_, impl_->control_shard()};
    if (!operation || !impl_->blobs->healthy()) {
        return store_detail::closed_store();
    }
    // Keep reference selection and root pinning inside one shared roots window:
    // collection must never unlink the selected file between these two steps.
    auto snapshot = impl_->blobs->snapshot_lock();
    auto value = get_record(key);
    if (!value || value->storage_type != ValueType::blob_ref) {
        return value;
    }
    const auto sequence = value->sequence, expiry = value->expire_at_ns;
    const auto shard = route_worker(hash_key_routing(key, impl_->routing), worker_count());
    auto reader = impl_->blobs->reader(std::move(*value), shard);
    snapshot.unlock();
    if (!reader) {
        return unexpected(reader.error());
    }
    const auto length = (*reader)->size();
    // Budget simultaneous owning-read materializations before allocating bytes.
    // The subtraction form checks the combined charge without unsigned wrap.
    auto used = impl_->blob_materializing_bytes.load(std::memory_order_relaxed);
    for (;;) {
        if (used > kMaximumBlobValueBytes || length > kMaximumBlobValueBytes - used) {
            return fail(ErrorCode::resource_exhausted, "blob owning-GET materialization budget exhausted");
        }
        if (impl_->blob_materializing_bytes.compare_exchange_weak(used, used + length)) {
            break;
        }
    }

    struct Release {
        std::atomic_uint64_t& used;
        std::uint64_t bytes;

        ~Release() {
            used.fetch_sub(bytes);
        }
    } release{impl_->blob_materializing_bytes, length};

    OwnedValue result;
    result.sequence = sequence;
    result.expire_at_ns = expiry;
    result.bytes.assign(nullptr, static_cast<std::size_t>(length));
    std::size_t offset{};
    while (offset < length) {
        auto read = (*reader)->read(std::span{result.bytes.data() + offset, result.bytes.size() - offset});
        if (!read) {
            return unexpected(read.error());
        }
        if (*read == 0) {
            return fail(ErrorCode::corrupted_data, "premature blob EOF");
        }
        offset += *read;
    }
    std::array<std::byte, 1> eof{};
    auto ended = (*reader)->read(eof);
    if (!ended || *ended != 0) {
        return ended ? fail(ErrorCode::corrupted_data, "invalid blob EOF") : unexpected(ended.error());
    }
    return result;
} catch (const std::bad_alloc&) {

    return store_detail::resource_exhausted();
} catch (...) {
    impl_->mark_durable_fail_closed();
    return store_detail::internal_failure();
}

auto Store::put(const std::string_view key, const std::span<const std::byte> value,
                const std::uint64_t expire_at_ns) -> Status try {
    if (!impl_->blobs) {
        return put_record(key, value, expire_at_ns, ValueType::bytes);
    }
    if (!impl_->blobs->healthy()) {
        return store_detail::closed_store();
    }
    if (key.empty() || key.size() > kMaximumBlobKeyBytes || value.size() > kMaximumBlobValueBytes) {
        return fail(ErrorCode::record_too_large, "blob profile key or value exceeds limit");
    }
    if (value.size() <= kMaxNormalRecordSize - kEncodedRecordHeaderSize - key.size()) {
        return put_record(key, value, expire_at_ns, ValueType::bytes);
    }
    auto writer = begin_blob_put(key, {.expire_at_ns = expire_at_ns, .expected_size = value.size()});
    if (!writer) {
        return unexpected(writer.error());
    }
    for (std::size_t offset = 0; offset < value.size();) {
        const auto count = std::min<std::size_t>(kBlobChunkBytes, value.size() - offset);
        if (auto written = (*writer)->write(value.subspan(offset, count)); !written) {
            return written;
        }
        offset += count;
    }
    return (*writer)->commit();
} catch (const std::bad_alloc&) {

    return store_detail::resource_exhausted();
} catch (...) {
    return store_detail::internal_failure();
}

auto Store::collect_blobs() -> Result<BlobGcResult> try {
    Impl::OperationGuard operation{*impl_, impl_->control_shard()};
    if (!operation || !impl_->blobs || !impl_->blobs->healthy()) {
        return fail(ErrorCode::unavailable, "blob profile is not open");
    }
    auto guard = impl_->blobs->collection_lock();
    if (auto flushed = impl_->durable_runtime->flush(); !flushed) {
        return unexpected(flushed.error());
    }
    auto roots = impl_->durable_runtime->blob_roots();
    if (!roots) {
        return unexpected(roots.error());
    }
    return impl_->blobs->collect(*roots);
} catch (const std::bad_alloc&) {

    return store_detail::resource_exhausted();
} catch (...) {
    return store_detail::internal_failure();
}
} // namespace glifistore
