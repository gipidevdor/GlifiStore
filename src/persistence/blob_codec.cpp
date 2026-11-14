#include "glifistore/core/little_endian.hpp"
#include "glifistore/persistence/blob.hpp"
#include "glifistore/segment/crc32c.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace glifistore {
namespace {
constexpr std::array<std::uint32_t, 64> kRound{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

auto zero(std::span<const std::byte> bytes) -> bool {
    return std::ranges::all_of(bytes, [](std::byte b) { return b == std::byte{}; });
}

auto valid(const BlobRef& ref) -> bool {
    return !zero(ref.id) && ref.logical_size > 0 && ref.logical_size <= kMaximumBlobValueBytes;
}
} // namespace

void BlobHasher::transform(const std::byte* block) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            w[i] = (w[i] << 8U) | std::to_integer<std::uint32_t>(block[4 * i + j]);
        }
    }
    for (std::size_t i = 16; i < w.size(); ++i) {
        const auto a = w[i - 15], b = w[i - 2];
        w[i] = w[i - 16] + (std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3U)) + w[i - 7] +
               (std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10U));
    }
    auto [a, b, c, d, e, f, g, h] = state_;
    for (std::size_t i = 0; i < w.size(); ++i) {
        const auto t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ ((~e) & g)) +
                        kRound[i] + w[i];
        const auto t2 =
            (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void BlobHasher::update(std::span<const std::byte> bytes) noexcept {
    bytes_ += bytes.size();
    while (!bytes.empty()) {
        if (buffered_ == 0 && bytes.size() >= buffer_.size()) {
            transform(bytes.data());
            bytes = bytes.subspan(buffer_.size());
            continue;
        }
        const auto count = std::min(bytes.size(), buffer_.size() - buffered_);
        std::memcpy(buffer_.data() + buffered_, bytes.data(), count);
        buffered_ += count;
        bytes = bytes.subspan(count);
        if (buffered_ == buffer_.size()) {
            transform(buffer_.data());
            buffered_ = 0;
        }
    }
}

auto BlobHasher::finish() const noexcept -> BlobDigest {
    auto copy = *this;
    const auto bits = bytes_ * 8U;
    std::array<std::byte, 128> padding{};
    padding[0] = std::byte{0x80};
    const auto count = buffered_ < 56 ? 56 - buffered_ : 120 - buffered_;
    for (std::size_t i = 0; i < 8; ++i) {
        padding[count + i] = static_cast<std::byte>(bits >> ((7 - i) * 8U));
    }
    copy.update(std::span{padding}.first(count + 8));
    BlobDigest digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            digest[4 * i + j] = static_cast<std::byte>(copy.state_[i] >> ((3 - j) * 8U));
        }
    }
    return digest;
}

auto blob_chunk_count(const std::uint64_t size) noexcept -> std::uint32_t {
    return size == 0 || size > kMaximumBlobValueBytes
               ? 0U
               : static_cast<std::uint32_t>(1U + (size - 1U) / kBlobChunkBytes);
}

auto blob_file_bytes(const std::uint64_t size) noexcept -> std::uint64_t {
    return blob_chunk_count(size) == 0
               ? 0U
               : kBlobHeaderBytes + 16ULL * blob_chunk_count(size) + size + kBlobFooterBytes;
}

auto encode_blob_ref(const BlobRef& ref) -> Result<std::array<std::byte, kBlobRefBytes>> {
    if (!valid(ref)) {
        return fail(ErrorCode::invalid_argument, "invalid BlobRef identity or size");
    }
    std::array<std::byte, kBlobRefBytes> out{};
    std::memcpy(out.data(), "GLBR", 4);
    le::put_u16(out, 4, 1);
    le::put_u16(out, 6, kBlobRefBytes);
    std::memcpy(out.data() + 8, ref.id.data(), 16);
    le::put_u64(out, 24, ref.logical_size);
    le::put_u32(out, 32, kBlobChunkBytes);
    le::put_u32(out, 36, blob_chunk_count(ref.logical_size));
    le::put_u16(out, 40, 1);
    std::memcpy(out.data() + 48, ref.digest.data(), 32);
    return out;
}

auto decode_blob_ref(const std::span<const std::byte> bytes) -> Result<BlobRef> {
    if (bytes.size() != kBlobRefBytes || std::memcmp(bytes.data(), "GLBR", 4) != 0 ||
        le::get_u16(bytes, 4) != 1 || le::get_u16(bytes, 6) != kBlobRefBytes ||
        le::get_u32(bytes, 32) != kBlobChunkBytes || le::get_u16(bytes, 40) != 1 ||
        !zero(bytes.subspan(42, 6))) {
        return fail(ErrorCode::invalid_record, "invalid BlobRef encoding");
    }
    BlobRef ref{.logical_size = le::get_u64(bytes, 24)};
    std::memcpy(ref.id.data(), bytes.data() + 8, 16);
    std::memcpy(ref.digest.data(), bytes.data() + 48, 32);
    if (!valid(ref) || le::get_u32(bytes, 36) != blob_chunk_count(ref.logical_size)) {
        return fail(ErrorCode::invalid_record, "invalid BlobRef extent");
    }
    return ref;
}

auto encode_blob_metadata(const BlobFileMetadata& meta)
    -> Result<std::array<std::byte, kBlobHeaderBytes + kMaximumBlobChunks * 16>> {
    if (!valid(meta.reference) || zero(meta.store_id)) {
        return fail(ErrorCode::invalid_argument, "invalid blob metadata");
    }
    std::array<std::byte, kBlobHeaderBytes + kMaximumBlobChunks * 16> out{};
    std::memcpy(out.data(), "GLYB", 4);
    le::put_u16(out, 4, 1);
    le::put_u16(out, 6, kBlobHeaderBytes);
    std::memcpy(out.data() + 8, meta.reference.id.data(), 16);
    std::memcpy(out.data() + 24, meta.store_id.data(), 16);
    le::put_u32(out, 40, meta.owner.value);
    le::put_u32(out, 44, kBlobChunkBytes);
    const auto size = meta.reference.logical_size;
    const auto count = blob_chunk_count(size);
    le::put_u64(out, 48, size);
    le::put_u32(out, 56, count);
    le::put_u16(out, 60, 16);
    le::put_u16(out, 62, 1);
    le::put_u64(out, 64, kBlobHeaderBytes);
    le::put_u64(out, 72, kBlobHeaderBytes + 16ULL * count);
    le::put_u64(out, 80, blob_file_bytes(size) - kBlobFooterBytes);
    le::put_u64(out, 88, blob_file_bytes(size));
    std::uint64_t offset = kBlobHeaderBytes + 16ULL * count, remaining = size;
    for (std::size_t i = 0; i < count; ++i) {
        const auto length = static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining, kBlobChunkBytes));
        if (meta.chunks[i].offset != offset || meta.chunks[i].length != length) {
            return fail(ErrorCode::invalid_argument, "noncanonical blob chunk extent");
        }
        le::put_u64(out, 128 + i * 16, offset);
        le::put_u32(out, 136 + i * 16, length);
        le::put_u32(out, 140 + i * 16, meta.chunks[i].checksum);
        offset += length;
        remaining -= length;
    }
    le::put_u32(out, 96, crc32c(std::span{out}.first(kBlobHeaderBytes + 16U * count)));
    return out;
}

auto decode_blob_metadata(const std::span<const std::byte> bytes) -> Result<BlobFileMetadata> {
    if (bytes.size() < kBlobHeaderBytes || std::memcmp(bytes.data(), "GLYB", 4) != 0 ||
        le::get_u16(bytes, 4) != 1 || le::get_u16(bytes, 6) != kBlobHeaderBytes ||
        le::get_u32(bytes, 44) != kBlobChunkBytes || le::get_u16(bytes, 60) != 16 ||
        le::get_u16(bytes, 62) != 1 || le::get_u64(bytes, 64) != kBlobHeaderBytes ||
        !zero(bytes.subspan(100, 28))) {
        return fail(ErrorCode::invalid_record, "invalid blob header");
    }
    BlobFileMetadata meta{.reference = {.logical_size = le::get_u64(bytes, 48)},
                          .owner = WorkerId{le::get_u32(bytes, 40)}};
    std::memcpy(meta.reference.id.data(), bytes.data() + 8, 16);
    std::memcpy(meta.store_id.data(), bytes.data() + 24, 16);
    const auto count = blob_chunk_count(meta.reference.logical_size);
    if (!valid(meta.reference) || zero(meta.store_id) || le::get_u32(bytes, 56) != count ||
        bytes.size() != kBlobHeaderBytes + 16U * count || le::get_u64(bytes, 72) != bytes.size() ||
        le::get_u64(bytes, 88) != blob_file_bytes(meta.reference.logical_size) ||
        le::get_u64(bytes, 80) != blob_file_bytes(meta.reference.logical_size) - kBlobFooterBytes) {
        return fail(ErrorCode::invalid_record, "invalid blob extent");
    }
    std::array<std::byte, kBlobHeaderBytes + kMaximumBlobChunks * 16> copy{};
    std::memcpy(copy.data(), bytes.data(), bytes.size());
    le::put_u32(copy, 96, 0);
    if (crc32c(std::span{copy}.first(bytes.size())) != le::get_u32(bytes, 96)) {
        return fail(ErrorCode::checksum_mismatch, "blob metadata checksum mismatch");
    }
    std::uint64_t offset = bytes.size(), remaining = meta.reference.logical_size;
    for (std::size_t i = 0; i < count; ++i) {
        auto& chunk = meta.chunks[i];
        chunk = {.offset = le::get_u64(bytes, 128 + i * 16),
                 .length = le::get_u32(bytes, 136 + i * 16),
                 .checksum = le::get_u32(bytes, 140 + i * 16)};
        const auto length = std::min<std::uint64_t>(remaining, kBlobChunkBytes);
        if (chunk.offset != offset || chunk.length != length) {
            return fail(ErrorCode::invalid_record, "invalid blob chunk range");
        }
        offset += length;
        remaining -= length;
    }
    return meta;
}

auto encode_blob_footer(const BlobRef& ref) -> Result<std::array<std::byte, kBlobFooterBytes>> {
    if (!valid(ref)) {
        return fail(ErrorCode::invalid_argument, "invalid blob footer");
    }
    std::array<std::byte, kBlobFooterBytes> out{};
    std::memcpy(out.data(), "GLBF", 4);
    le::put_u16(out, 4, 1);
    le::put_u16(out, 6, kBlobFooterBytes);
    le::put_u64(out, 8, ref.logical_size);
    std::memcpy(out.data() + 16, ref.digest.data(), 32);
    le::put_u32(out, 48, crc32c(out));
    return out;
}

auto validate_blob_footer(const std::span<const std::byte> bytes, const BlobRef& ref) -> Status {
    const auto expected = encode_blob_footer(ref);
    if (!expected || bytes.size() != kBlobFooterBytes || !std::ranges::equal(bytes, *expected)) {
        return fail(ErrorCode::checksum_mismatch, "blob footer or digest mismatch");
    }
    return {};
}
} // namespace glifistore
