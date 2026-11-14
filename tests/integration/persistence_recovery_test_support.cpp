#include "persistence_recovery_test_support.hpp"

namespace persistence_recovery_test_support {

auto recovery_store_id(std::byte first) -> glifistore::StoreId {
    return {first,           std::byte{0x21}, std::byte{0x22}, std::byte{0x23},
            std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27},
            std::byte{0x28}, std::byte{0x29}, std::byte{0x2A}, std::byte{0x2B},
            std::byte{0x2C}, std::byte{0x2D}, std::byte{0x2E}, std::byte{0x2F}};
}

auto key_for_worker(std::size_t worker, std::size_t worker_count, std::string_view prefix) -> std::string {
    for (std::size_t suffix = 0; suffix < 10'000; ++suffix) {
        auto candidate = std::string{prefix} + std::to_string(suffix);
        if (glifistore::route_worker(candidate, worker_count) == worker) {
            return candidate;
        }
    }
    throw std::runtime_error("failed to construct a routed test key");
}

auto segment_identity(const glifistore::StoreId& store_id, const glifistore::ManifestSegmentEntry& entry)
    -> glifistore::SegmentHeaderIdentity {
    return {
        .store_id = store_id,
        .segment_id = entry.segment_id,
        .generation = entry.generation,
        .owner_worker = entry.owner_worker,
    };
}

auto create_segment(glifistore::DataDirectory& directory, const glifistore::StoreId& store_id,
                    const glifistore::ManifestSegmentEntry& entry) -> glifistore::DurableSegmentFile {
    auto created = glifistore::DurableSegmentFile::create(directory, segment_identity(store_id, entry));
    GLIFI_REQUIRE(created.durable());
    GLIFI_REQUIRE(created.file.has_value());
    return std::move(*created.file);
}

void append_record(glifistore::DurableSegmentFile& file, std::uint64_t sequence, std::string_view key,
                   std::string_view value, glifistore::Opcode opcode, std::uint64_t expire_at_ns,
                   std::optional<std::uint64_t> stored_hash) {
    const auto key_bytes = std::as_bytes(std::span{key});
    const auto value_bytes = std::as_bytes(std::span{value});
    const auto encoded = glifistore::encode_record({
        .sequence = glifistore::SequenceNumber{sequence},
        .opcode = opcode,
        .type = glifistore::ValueType::bytes,
        .flags = 0,
        .key_hash = stored_hash.value_or(glifistore::hash_key(key)),
        .expire_at_ns = expire_at_ns,
        .key = key_bytes,
        .value = value_bytes,
    });
    GLIFI_REQUIRE(encoded.has_value());
    GLIFI_REQUIRE(file.append(*encoded).committed());
}

void create_private_file(const std::filesystem::path& path) {
    const auto descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    GLIFI_REQUIRE(descriptor >= 0);
    GLIFI_REQUIRE(::close(descriptor) == 0);
}

auto recovery_manifest(const glifistore::StoreId& store_id, std::uint32_t workers,
                       std::vector<glifistore::ManifestSegmentEntry> segments) -> glifistore::Manifest {
    const auto next_id = segments.empty() ? 1 : segments.back().segment_id.value + 1;
    return {
        .store_id = store_id,
        .manifest_generation = 1,
        .routing_algorithm = glifistore::RoutingAlgorithm::fnv1a64_v1,
        .worker_count = workers,
        .routing_epoch = 1,
        .next_segment_id = glifistore::SegmentId{next_id},
        .next_segment_generation = glifistore::GenerationId{1},
        .segments = std::move(segments),
    };
}

auto owned_text(const glifistore::OwnedValue& value) -> std::string {
    return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

} // namespace persistence_recovery_test_support
