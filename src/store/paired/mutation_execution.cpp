#include "glifistore/store/paired/mutation_execution.hpp"

#include "store/store_internal.hpp"

namespace glifistore::store::paired {

void rewrite_known_not_committed_wire_error(Error& error) noexcept {
    switch (error.code) {
    case ErrorCode::not_found:
    case ErrorCode::invalid_argument:
    case ErrorCode::record_too_large:
    case ErrorCode::resource_exhausted:
    case ErrorCode::storage_exhausted:
    case ErrorCode::file_too_large:
    case ErrorCode::descriptor_exhausted:
    case ErrorCode::read_only_filesystem:
    case ErrorCode::sequence_conflict:
    case ErrorCode::segment_full:
    case ErrorCode::segment_sealed:
    case ErrorCode::arithmetic_overflow:
        return;
    default:
        error.code = ErrorCode::resource_exhausted;
        return;
    }
}

auto classify_volatile_mutation_error(Error error, bool& sticky_indeterminate) noexcept -> Error {
    if (error.code == ErrorCode::unavailable) {
        sticky_indeterminate = true;
        return error;
    }
    rewrite_known_not_committed_wire_error(error);
    return error;
}

auto execute_durable_single(Store& store, const std::size_t shard, const MutationKind kind,
                            const HashedKey& key, const std::span<const std::byte> value,
                            const std::uint64_t expire_at_ns, const ValueType type) -> DurableMutationResult {
    DurableMutationResult result;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        result = kind == MutationKind::put
                     ? detail::StoreAccess::put_durable(store, shard, key, value, expire_at_ns, type)
                     : detail::StoreAccess::erase_durable(store, shard, key);
        if (!detail::StoreAccess::should_retry_durable_mutation(result, attempt)) {
            break;
        }
    }
    return result;
}

} // namespace glifistore::store::paired
