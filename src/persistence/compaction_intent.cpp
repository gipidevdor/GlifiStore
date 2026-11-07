#include "glifistore/persistence/compaction_intent.hpp"

#include "glifistore/core/little_endian.hpp"
#include "glifistore/persistence/compaction.hpp"
#include "glifistore/segment/crc32c.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace glifistore {
namespace {

inline constexpr std::size_t kChecksumOffset = 56;
inline constexpr std::size_t kChecksumBytes = 4;

using le::get_u16;
using le::get_u32;
using le::get_u64;
using le::put_u16;
using le::put_u32;
using le::put_u64;

auto all_zero(const std::span<const std::byte> bytes) -> bool {
    return std::ranges::all_of(bytes, [](const std::byte value) { return value == std::byte{0}; });
}

auto checksum_with_zeroed_field(const std::span<const std::byte> bytes) -> std::uint32_t {
    std::vector<std::byte> copy(bytes.begin(), bytes.end());
    std::fill_n(copy.begin() + static_cast<std::ptrdiff_t>(kChecksumOffset), kChecksumBytes, std::byte{0});
    return crc32c(copy);
}

auto validate_intent(const DurableCompactionIntent& intent) -> Status {
    const auto transition =
        validate_durable_compaction_transition(intent.old_manifest, intent.next_manifest, intent.worker_id);
    if (!transition) {
        return unexpected(transition.error());
    }
    return {};
}

} // namespace

auto encoded_compaction_intent_size(const DurableCompactionIntent& intent) -> Result<std::size_t> {
    if (auto valid = validate_intent(intent); !valid) {
        return unexpected(valid.error());
    }
    const auto old_size = encoded_manifest_size(intent.old_manifest);
    const auto next_size = encoded_manifest_size(intent.next_manifest);
    if (!old_size || !next_size) {
        return unexpected((!old_size ? old_size.error() : next_size.error()));
    }
    if (*old_size > std::numeric_limits<std::size_t>::max() - kCompactionIntentHeaderBytes ||
        *next_size > std::numeric_limits<std::size_t>::max() - kCompactionIntentHeaderBytes - *old_size) {
        return fail(ErrorCode::arithmetic_overflow, "compaction intent encoded size overflows size_t");
    }
    const auto total = kCompactionIntentHeaderBytes + *old_size + *next_size;
    static_assert(kMaximumCompactionIntentBytes <= std::numeric_limits<std::uint32_t>::max());
    if (total > kMaximumCompactionIntentBytes) {
        return fail(ErrorCode::arithmetic_overflow, "compaction intent exceeds its format size field");
    }
    return total;
}

auto encode_compaction_intent(const std::span<std::byte> out, const DurableCompactionIntent& intent)
    -> Status {
    const auto total = encoded_compaction_intent_size(intent);
    const auto old_manifest = encode_manifest(intent.old_manifest);
    const auto next_manifest = encode_manifest(intent.next_manifest);
    if (!total || !old_manifest || !next_manifest) {
        if (!total) {
            return unexpected(total.error());
        }
        return unexpected((!old_manifest ? old_manifest.error() : next_manifest.error()));
    }
    if (out.size() < *total) {
        return fail(ErrorCode::invalid_argument, "compaction intent encode buffer is too small");
    }
    std::vector<std::byte> encoded(*total, std::byte{0});
    put_u32(encoded, 0, kCompactionIntentMagic);
    put_u16(encoded, 4, intent.old_manifest.format_version);
    put_u16(encoded, 6, static_cast<std::uint16_t>(kCompactionIntentHeaderBytes));
    put_u32(encoded, 8, static_cast<std::uint32_t>(*total));
    put_u32(encoded, 12, intent.worker_id.value);
    std::memcpy(encoded.data() + 16, intent.old_manifest.store_id.data(),
                intent.old_manifest.store_id.size());
    put_u64(encoded, 32, intent.old_manifest.manifest_generation);
    put_u64(encoded, 40, intent.next_manifest.manifest_generation);
    put_u32(encoded, 48, static_cast<std::uint32_t>(old_manifest->size()));
    put_u32(encoded, 52, static_cast<std::uint32_t>(next_manifest->size()));
    std::memcpy(encoded.data() + kCompactionIntentHeaderBytes, old_manifest->data(), old_manifest->size());
    std::memcpy(encoded.data() + kCompactionIntentHeaderBytes + old_manifest->size(), next_manifest->data(),
                next_manifest->size());
    put_u32(encoded, kChecksumOffset, checksum_with_zeroed_field(encoded));
    std::memcpy(out.data(), encoded.data(), encoded.size());
    return {};
}

auto encode_compaction_intent(const DurableCompactionIntent& intent) -> Result<std::vector<std::byte>> {
    const auto size = encoded_compaction_intent_size(intent);
    if (!size) {
        return unexpected(size.error());
    }
    std::vector<std::byte> encoded(*size, std::byte{0});
    if (auto status = encode_compaction_intent(encoded, intent); !status) {
        return unexpected(status.error());
    }
    return encoded;
}

auto decode_compaction_intent(const std::span<const std::byte> bytes) -> Result<DurableCompactionIntent> {
    if (bytes.size() < kCompactionIntentHeaderBytes) {
        return fail(ErrorCode::invalid_record, "compaction intent is shorter than its v1 header");
    }
    if (get_u32(bytes, 0) != kCompactionIntentMagic || (get_u16(bytes, 4) != 1 && get_u16(bytes, 4) != 2) ||
        get_u16(bytes, 6) != kCompactionIntentHeaderBytes || !all_zero(bytes.subspan(60, 68))) {
        return fail(ErrorCode::invalid_record,
                    "compaction intent magic, version, header size, or reserved fields are invalid");
    }
    const auto old_size = static_cast<std::size_t>(get_u32(bytes, 48));
    const auto next_size = static_cast<std::size_t>(get_u32(bytes, 52));
    if (old_size < kManifestHeaderBytes || next_size < kManifestHeaderBytes ||
        old_size > kMaximumManifestBytes || next_size > kMaximumManifestBytes ||
        old_size > std::numeric_limits<std::size_t>::max() - kCompactionIntentHeaderBytes ||
        next_size > std::numeric_limits<std::size_t>::max() - kCompactionIntentHeaderBytes - old_size) {
        return fail(ErrorCode::invalid_record, "compaction intent manifest extents are invalid");
    }
    const auto expected = kCompactionIntentHeaderBytes + old_size + next_size;
    if (bytes.size() != expected || get_u32(bytes, 8) != expected) {
        return fail(ErrorCode::invalid_record, "compaction intent encoded extent is inconsistent");
    }
    if (get_u32(bytes, kChecksumOffset) != checksum_with_zeroed_field(bytes)) {
        return fail(ErrorCode::checksum_mismatch, "compaction intent checksum mismatch");
    }
    auto old_manifest = decode_manifest(bytes.subspan(kCompactionIntentHeaderBytes, old_size));
    auto next_manifest = decode_manifest(bytes.subspan(kCompactionIntentHeaderBytes + old_size, next_size));
    if (!old_manifest || !next_manifest) {
        return unexpected((!old_manifest ? old_manifest.error() : next_manifest.error()));
    }
    DurableCompactionIntent intent{.worker_id = WorkerId{get_u32(bytes, 12)},
                                   .old_manifest = std::move(*old_manifest),
                                   .next_manifest = std::move(*next_manifest)};
    if (intent.old_manifest.format_version != get_u16(bytes, 4) ||
        intent.next_manifest.format_version != get_u16(bytes, 4)) {
        return fail(ErrorCode::invalid_record, "compaction intent profile mismatch");
    }
    StoreId encoded_store{};
    std::memcpy(encoded_store.data(), bytes.data() + 16, encoded_store.size());
    if (encoded_store != intent.old_manifest.store_id ||
        get_u64(bytes, 32) != intent.old_manifest.manifest_generation ||
        get_u64(bytes, 40) != intent.next_manifest.manifest_generation) {
        return fail(ErrorCode::corrupted_data,
                    "compaction intent header disagrees with its embedded manifests");
    }
    if (auto valid = validate_intent(intent); !valid) {
        return fail(ErrorCode::corrupted_data, valid.error().message);
    }
    return intent;
}

} // namespace glifistore
