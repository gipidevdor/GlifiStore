#include "glifistore/core/memory_reserve.hpp"
#include "glifistore/server/pair_read_generation.hpp"
#include "test.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] auto bytes(const std::string_view text) noexcept -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

[[nodiscard]] auto text(const glifistore::OwnedValue& value) noexcept -> std::string_view {
    return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

[[nodiscard]] auto append(glifistore::Segment& segment, const glifistore::WorkerRoutingState routing,
                          const std::string_view key, const std::string_view value,
                          const std::uint64_t sequence, const glifistore::Opcode opcode,
                          const std::uint64_t expire_at_ns = 0) -> glifistore::RecordRef {
    auto record = segment.append({.sequence = glifistore::SequenceNumber{sequence},
                                  .opcode = opcode,
                                  .key_hash = glifistore::hash_key_routing(key, routing),
                                  .expire_at_ns = expire_at_ns,
                                  .key = bytes(key),
                                  .value = bytes(value)});
    GLIFI_REQUIRE(record.has_value());
    return *record;
}

} // namespace

GLIFI_TEST("paired read generation overlays delta on base with tombstones and TTL") {
    const glifistore::WorkerRoutingState routing{};
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{7});
    auto generation = glifistore::server::PairReadGeneration::empty(routing);
    GLIFI_REQUIRE(generation.has_value());

    const std::string key{"generation-key"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
    const auto first = append(*segment, routing, key, "base-value", 1, glifistore::Opcode::put);
    const glifistore::server::ReadMutation first_mutation{
        .key = hashed, .record = first, .segment = segment, .opcode = glifistore::Opcode::put};
    generation = glifistore::server::PairReadGeneration::publish(std::move(*generation),
                                                                 std::span{&first_mutation, 1}, 1);
    GLIFI_REQUIRE(generation.has_value());
    GLIFI_REQUIRE((*generation)->base_entries() == 1);
    GLIFI_REQUIRE((*generation)->delta_entries() == 0);
    auto found = (*generation)->get(hashed, 1);
    GLIFI_REQUIRE(found.has_value());
    GLIFI_REQUIRE(text(*found) == "base-value");

    const auto erased = append(*segment, routing, key, {}, 2, glifistore::Opcode::erase);
    const glifistore::server::ReadMutation erase_mutation{
        .key = hashed, .record = erased, .segment = segment, .opcode = glifistore::Opcode::erase};
    generation = glifistore::server::PairReadGeneration::publish(std::move(*generation),
                                                                 std::span{&erase_mutation, 1}, 8);
    GLIFI_REQUIRE(generation.has_value());
    GLIFI_REQUIRE(!(*generation)->get(hashed, 1).has_value());

    const auto expiring = append(*segment, routing, key, "short", 3, glifistore::Opcode::put, 10);
    const glifistore::server::ReadMutation expiring_mutation{
        .key = hashed, .record = expiring, .segment = segment, .opcode = glifistore::Opcode::put};
    generation = glifistore::server::PairReadGeneration::publish(std::move(*generation),
                                                                 std::span{&expiring_mutation, 1}, 8);
    GLIFI_REQUIRE(generation.has_value());
    GLIFI_REQUIRE((*generation)->get(hashed, 9).has_value());
    GLIFI_REQUIRE(!(*generation)->get(hashed, 10).has_value());
    GLIFI_REQUIRE((*generation)->visible_through() == 3);
}

GLIFI_TEST("paired read generation volatile views borrow exact base and delta bytes") {
    for (const std::size_t merge_entries : {1U, 8U}) {
        const glifistore::WorkerRoutingState routing{};
        auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{19});
        std::weak_ptr<glifistore::Segment> lifetime = segment;
        const std::string key{"borrowed-view"};
        const std::string value(4096, 'v');
        const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
        const auto reference = append(*segment, routing, key, value, 11, glifistore::Opcode::put, 100);
        const auto decoded =
            glifistore::decode_record({segment->base() + reference.offset.value, reference.size.value});
        GLIFI_REQUIRE(decoded.has_value());
        auto initial = glifistore::server::PairReadGeneration::empty(routing);
        GLIFI_REQUIRE(initial.has_value());
        glifistore::server::ReadMutation mutation{
            .key = hashed, .record = reference, .segment = segment, .opcode = glifistore::Opcode::put};
        auto published = glifistore::server::PairReadGeneration::publish(
            std::move(*initial), std::span{&mutation, 1}, merge_entries);
        GLIFI_REQUIRE(published.has_value());
        auto generation = std::move(*published);
        mutation.segment.reset();
        segment.reset();
        GLIFI_REQUIRE(!lifetime.expired());
        const auto borrowed = generation->get_volatile_view(hashed, 99);
        const auto owned = generation->get(hashed, 99);
        GLIFI_REQUIRE(borrowed.has_value());
        GLIFI_REQUIRE(owned.has_value());
        GLIFI_REQUIRE(borrowed->value.data() == decoded->value.data());
        GLIFI_REQUIRE(borrowed->sequence.value == owned->sequence);
        GLIFI_REQUIRE(borrowed->expire_at_ns == owned->expire_at_ns);
        GLIFI_REQUIRE(borrowed->type == owned->storage_type);
        GLIFI_REQUIRE(text(*owned) == value);
        const auto expired = generation->get_volatile_view(hashed, 100);
        GLIFI_REQUIRE(!expired.has_value());
        GLIFI_REQUIRE(expired.error().code == glifistore::ErrorCode::not_found);
        generation.reset();
        GLIFI_REQUIRE(lifetime.expired());
        // Only the independent owning value is accessed after the generation is released.
        GLIFI_REQUIRE(text(*owned) == value);
    }
}

GLIFI_TEST("paired read generation volatile views preserve corruption and tombstone rejection") {
    for (const std::size_t merge_entries : {1U, 8U}) {
        const glifistore::WorkerRoutingState routing{};
        auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{20});
        const std::string key{"borrow-validation"};
        const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
        const auto reference = append(*segment, routing, key, "original", 1, glifistore::Opcode::put);
        auto generation = glifistore::server::PairReadGeneration::empty(routing);
        GLIFI_REQUIRE(generation.has_value());
        const glifistore::server::ReadMutation mutation{
            .key = hashed, .record = reference, .segment = segment, .opcode = glifistore::Opcode::put};
        generation = glifistore::server::PairReadGeneration::publish(std::move(*generation),
                                                                     std::span{&mutation, 1}, merge_entries);
        GLIFI_REQUIRE(generation.has_value());
        // Deliberate memory corruption must still pass through the ordinary CRC validator.
        auto& damaged =
            segment->mutable_base()[reference.offset.value + glifistore::kEncodedRecordHeaderSize];
        damaged ^= std::byte{1};
        const auto borrowed = (*generation)->get_volatile_view(hashed, 0);
        const auto owned = (*generation)->get(hashed, 0);
        GLIFI_REQUIRE(!borrowed.has_value());
        GLIFI_REQUIRE(!owned.has_value());
        GLIFI_REQUIRE(borrowed.error().code == owned.error().code);
        damaged ^= std::byte{1};
        const auto erased = append(*segment, routing, key, {}, 2, glifistore::Opcode::erase);
        const glifistore::server::ReadMutation tombstone{
            .key = hashed, .record = erased, .segment = segment, .opcode = glifistore::Opcode::erase};
        generation = glifistore::server::PairReadGeneration::publish(std::move(*generation),
                                                                     std::span{&tombstone, 1}, merge_entries);
        GLIFI_REQUIRE(generation.has_value());
        const auto absent = (*generation)->get_volatile_view(hashed, 0);
        GLIFI_REQUIRE(!absent.has_value());
        GLIFI_REQUIRE(absent.error().code == glifistore::ErrorCode::not_found);
    }
}

GLIFI_TEST("paired read generation owns the exact Segment generation pin") {
    const glifistore::WorkerRoutingState routing{};
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{9}, glifistore::WorkerId{},
                                                         glifistore::GenerationId{4});
    std::weak_ptr<glifistore::Segment> lifetime = segment;
    auto generation = glifistore::server::PairReadGeneration::empty(routing);
    GLIFI_REQUIRE(generation.has_value());
    const std::string key{"pinned-key"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
    const auto record = append(*segment, routing, key, "pinned-value", 11, glifistore::Opcode::put);
    std::vector<glifistore::server::ReadMutation> mutations;
    mutations.push_back(
        {.key = hashed, .record = record, .segment = segment, .opcode = glifistore::Opcode::put});
    generation = glifistore::server::PairReadGeneration::publish(std::move(*generation), mutations, 8);
    GLIFI_REQUIRE(generation.has_value());
    mutations.clear();
    segment.reset();
    GLIFI_REQUIRE(!lifetime.expired());
    const auto found = (*generation)->get(hashed, 0);
    GLIFI_REQUIRE(found.has_value());
    GLIFI_REQUIRE(text(*found) == "pinned-value");
    generation = glifistore::server::PairReadGeneration::empty(routing);
    GLIFI_REQUIRE(lifetime.expired());
}

GLIFI_TEST("paired Delta arena retains immutable overwrite versions in fixed record blocks") {
    const glifistore::WorkerRoutingState routing{};
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{15});
    auto generation = glifistore::server::PairReadGeneration::empty(routing);
    GLIFI_REQUIRE(generation.has_value());
    const std::string key{"arena-key"};
    const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
    std::shared_ptr<const glifistore::server::PairReadGeneration> first_generation;
    glifistore::store::paired::ReadGenerationMemoryStats first_memory{};

    for (std::uint64_t sequence = 1; sequence <= 65; ++sequence) {
        const auto value = std::to_string(sequence);
        const glifistore::server::ReadMutation mutation{
            .key = hashed,
            .record = append(*segment, routing, key, value, sequence, glifistore::Opcode::put),
            .segment = segment,
            .opcode = glifistore::Opcode::put,
        };
        generation = glifistore::server::PairReadGeneration::publish_incremental(std::move(*generation),
                                                                                 std::span{&mutation, 1});
        GLIFI_REQUIRE(generation.has_value());
        if (sequence == 1) {
            first_generation = *generation;
            first_memory = first_generation->memory_stats();
        }
    }

    const auto overwrite_memory = (*generation)->memory_stats();
    // COW replaces the same logical page but does not grow the reachable
    // directory topology. The cached O(1) census must count reachable nodes,
    // not every historical clone.
    GLIFI_REQUIRE(overwrite_memory.delta_lookup_storage_bytes == first_memory.delta_lookup_storage_bytes);
    GLIFI_REQUIRE(overwrite_memory.delta_record_versions == 65);
    GLIFI_REQUIRE(overwrite_memory.delta_arena_record_bytes > first_memory.delta_arena_record_bytes);

    const std::string external_key(17, 'x');
    const glifistore::HashedKey external_hashed{external_key,
                                                glifistore::hash_key_routing(external_key, routing)};
    const glifistore::server::ReadMutation external_mutation{
        .key = external_hashed,
        .record = append(*segment, routing, external_key, "external", 66, glifistore::Opcode::put),
        .segment = segment,
        .opcode = glifistore::Opcode::put,
    };
    generation = glifistore::server::PairReadGeneration::publish_incremental(
        std::move(*generation), std::span{&external_mutation, 1});
    GLIFI_REQUIRE(generation.has_value());

    GLIFI_REQUIRE((*generation)->delta_entries() == 2);
    GLIFI_REQUIRE((*generation)->delta_record_versions() == 66);
    GLIFI_REQUIRE((*generation)->delta_arena_record_bytes() == 128U * 64U);
    GLIFI_REQUIRE((*generation)->delta_arena_key_bytes() == external_key.size());
    GLIFI_REQUIRE((*generation)->delta_arena_key_storage_bytes() == 4U * 1024U);
    const auto external_memory = (*generation)->memory_stats();
    GLIFI_REQUIRE(external_memory.delta_lookup_storage_bytes >= overwrite_memory.delta_lookup_storage_bytes);
    GLIFI_REQUIRE(external_memory.delta_allocated_lower_bound_bytes ==
                  external_memory.delta_lookup_storage_bytes + external_memory.delta_arena_record_bytes +
                      external_memory.delta_arena_key_storage_bytes);
    const auto latest = (*generation)->get(hashed, 0);
    const auto external = (*generation)->get(external_hashed, 0);
    GLIFI_REQUIRE(latest.has_value());
    GLIFI_REQUIRE(external.has_value());
    GLIFI_REQUIRE(text(*latest) == "65");
    GLIFI_REQUIRE(text(*external) == "external");
    GLIFI_REQUIRE(first_generation != nullptr);
    GLIFI_REQUIRE(text(*first_generation->get(hashed, 0)) == "1");
}

GLIFI_TEST("paired compact base preserves inline boundary and multi-block keys") {
    const glifistore::WorkerRoutingState routing{};
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{10});
    auto generation = glifistore::server::PairReadGeneration::empty(routing);
    GLIFI_REQUIRE(generation.has_value());

    const std::array keys{
        std::string(16, 'i'),
        std::string(17, 'e'),
        std::string((64U * 1024U) + 1U, 'l'),
    };
    std::array<glifistore::server::ReadMutation, keys.size()> mutations{};
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const auto& key = keys[index];
        mutations[index] = {
            .key = {key, glifistore::hash_key_routing(key, routing)},
            .record = append(*segment, routing, key, "value", index + 1U, glifistore::Opcode::put),
            .segment = segment,
            .opcode = glifistore::Opcode::put,
        };
    }
    generation =
        glifistore::server::PairReadGeneration::publish(std::move(*generation), mutations, mutations.size());
    GLIFI_REQUIRE(generation.has_value());
    GLIFI_REQUIRE((*generation)->base_entries() == keys.size());
    GLIFI_REQUIRE((*generation)->delta_entries() == 0);
    const auto memory = (*generation)->memory_stats();
    GLIFI_REQUIRE(memory.base_entries == keys.size());
    GLIFI_REQUIRE(memory.base_capacity == 8);
    GLIFI_REQUIRE(memory.base_record_storage_bytes == keys.size() * 64U);
    GLIFI_REQUIRE(memory.base_record_mapped_storage_bytes == 0);
    GLIFI_REQUIRE(memory.base_lookup_storage_bytes == memory.base_capacity * 5U);
    // Before the compact lookup cell, the immutable base carried one control
    // byte, one full hash and one pointer for every bucket (17 B/bucket).
    GLIFI_REQUIRE(memory.base_lookup_storage_bytes < memory.base_capacity * 17U);
    GLIFI_REQUIRE(memory.base_key_bytes == keys[1].size() + keys[2].size());
    GLIFI_REQUIRE(memory.base_key_storage_bytes >= memory.base_key_bytes);
    GLIFI_REQUIRE(memory.base_pin_storage_bytes >= sizeof(glifistore::SegmentPtr));
    GLIFI_REQUIRE(memory.base_allocated_lower_bound_bytes >=
                  memory.base_record_storage_bytes + memory.base_lookup_storage_bytes +
                      memory.base_key_storage_bytes + memory.base_pin_storage_bytes);
    GLIFI_REQUIRE(memory.delta_entries == 0);
    GLIFI_REQUIRE(memory.delta_lookup_storage_bytes > 0);
    GLIFI_REQUIRE(memory.current_allocated_lower_bound_bytes == memory.generation_shell_bytes +
                                                                    memory.base_allocated_lower_bound_bytes +
                                                                    memory.delta_allocated_lower_bound_bytes);
    for (const auto& key : keys) {
        const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
        const auto found = (*generation)->get(hashed, 0);
        GLIFI_REQUIRE(found.has_value());
        GLIFI_REQUIRE(text(*found) == "value");
    }
}

GLIFI_TEST("paired incremental merge preserves cut and post-cut visibility with two levels") {
    const glifistore::WorkerRoutingState routing{};
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{12});
    auto generation = glifistore::server::PairReadGeneration::empty(routing);
    GLIFI_REQUIRE(generation.has_value());
    const auto hashed = [&](const std::string_view key) {
        return glifistore::HashedKey{key, glifistore::hash_key_routing(key, routing)};
    };

    std::array<glifistore::server::ReadMutation, 3> base_mutations{
        glifistore::server::ReadMutation{.key = hashed("a"),
                                         .record =
                                             append(*segment, routing, "a", "a1", 1, glifistore::Opcode::put),
                                         .segment = segment,
                                         .opcode = glifistore::Opcode::put},
        glifistore::server::ReadMutation{.key = hashed("b"),
                                         .record =
                                             append(*segment, routing, "b", "b1", 2, glifistore::Opcode::put),
                                         .segment = segment,
                                         .opcode = glifistore::Opcode::put},
        glifistore::server::ReadMutation{.key = hashed("c"),
                                         .record =
                                             append(*segment, routing, "c", "c1", 3, glifistore::Opcode::put),
                                         .segment = segment,
                                         .opcode = glifistore::Opcode::put},
    };
    generation = glifistore::server::PairReadGeneration::publish(std::move(*generation), base_mutations, 3);
    GLIFI_REQUIRE(generation.has_value());
    GLIFI_REQUIRE((*generation)->base_entries() == 3);
    GLIFI_REQUIRE((*generation)->delta_entries() == 0);

    std::array<glifistore::server::ReadMutation, 3> cut_mutations{
        glifistore::server::ReadMutation{.key = hashed("a"),
                                         .record =
                                             append(*segment, routing, "a", "a2", 4, glifistore::Opcode::put),
                                         .segment = segment,
                                         .opcode = glifistore::Opcode::put},
        glifistore::server::ReadMutation{.key = hashed("b"),
                                         .record =
                                             append(*segment, routing, "b", {}, 5, glifistore::Opcode::erase),
                                         .segment = segment,
                                         .opcode = glifistore::Opcode::erase},
        glifistore::server::ReadMutation{.key = hashed("d"),
                                         .record =
                                             append(*segment, routing, "d", "d1", 6, glifistore::Opcode::put),
                                         .segment = segment,
                                         .opcode = glifistore::Opcode::put},
    };
    generation =
        glifistore::server::PairReadGeneration::publish_incremental(std::move(*generation), cut_mutations);
    GLIFI_REQUIRE(generation.has_value());
    auto merge = glifistore::server::PairReadGeneration::start_incremental_merge(*generation, 100);
    GLIFI_REQUIRE(merge.has_value());

    auto first_quantum = glifistore::server::PairReadGeneration::advance_incremental_merge(**merge, 4);
    GLIFI_REQUIRE(first_quantum.has_value());
    GLIFI_REQUIRE(*first_quantum <= 4);
    GLIFI_REQUIRE(!glifistore::server::PairReadGeneration::merge_ready(**merge));
    GLIFI_REQUIRE(text(*(*generation)->get(hashed("a"), 0)) == "a2");
    GLIFI_REQUIRE(!(*generation)->get(hashed("b"), 0).has_value());
    GLIFI_REQUIRE(text(*(*generation)->get(hashed("d"), 0)) == "d1");

    std::array<glifistore::server::ReadMutation, 3> post_mutations{
        glifistore::server::ReadMutation{.key = hashed("a"),
                                         .record =
                                             append(*segment, routing, "a", "a3", 7, glifistore::Opcode::put),
                                         .segment = segment,
                                         .opcode = glifistore::Opcode::put},
        glifistore::server::ReadMutation{.key = hashed("d"),
                                         .record =
                                             append(*segment, routing, "d", {}, 8, glifistore::Opcode::erase),
                                         .segment = segment,
                                         .opcode = glifistore::Opcode::erase},
        glifistore::server::ReadMutation{.key = hashed("e"),
                                         .record =
                                             append(*segment, routing, "e", "e1", 9, glifistore::Opcode::put),
                                         .segment = segment,
                                         .opcode = glifistore::Opcode::put},
    };
    generation = glifistore::server::PairReadGeneration::publish_incremental(std::move(*generation),
                                                                             post_mutations, merge->get());
    GLIFI_REQUIRE(generation.has_value());
    GLIFI_REQUIRE(glifistore::server::PairReadGeneration::merge_post_entries(**merge) == 3);

    std::size_t quanta{1};
    while (!glifistore::server::PairReadGeneration::merge_ready(**merge)) {
        auto advanced = glifistore::server::PairReadGeneration::advance_incremental_merge(**merge, 4'096);
        GLIFI_REQUIRE(advanced.has_value());
        GLIFI_REQUIRE(*advanced <= 4'096);
        ++quanta;
    }
    GLIFI_REQUIRE(quanta > 2);
    generation =
        glifistore::server::PairReadGeneration::finish_incremental_merge(std::move(*generation), **merge);
    GLIFI_REQUIRE(generation.has_value());
    GLIFI_REQUIRE((*generation)->base_entries() == 3);
    GLIFI_REQUIRE((*generation)->delta_entries() == 3);
    GLIFI_REQUIRE((*generation)->visible_through() == 9);
    GLIFI_REQUIRE(text(*(*generation)->get(hashed("a"), 0)) == "a3");
    GLIFI_REQUIRE(!(*generation)->get(hashed("b"), 0).has_value());
    GLIFI_REQUIRE(text(*(*generation)->get(hashed("c"), 0)) == "c1");
    GLIFI_REQUIRE(!(*generation)->get(hashed("d"), 0).has_value());
    GLIFI_REQUIRE(text(*(*generation)->get(hashed("e"), 0)) == "e1");
}

GLIFI_TEST("paired incremental merge applies bounded post-cut backpressure before publication") {
    const glifistore::WorkerRoutingState routing{};
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{13});
    auto generation = glifistore::server::PairReadGeneration::empty(routing);
    GLIFI_REQUIRE(generation.has_value());
    const auto mutation = [&](const std::string_view key, const std::uint64_t sequence) {
        return glifistore::server::ReadMutation{
            .key = {key, glifistore::hash_key_routing(key, routing)},
            .record = append(*segment, routing, key, "value", sequence, glifistore::Opcode::put),
            .segment = segment,
            .opcode = glifistore::Opcode::put,
        };
    };
    auto cut = mutation("cut", 1);
    generation = glifistore::server::PairReadGeneration::publish_incremental(std::move(*generation),
                                                                             std::span{&cut, 1});
    GLIFI_REQUIRE(generation.has_value());
    auto merge = glifistore::server::PairReadGeneration::start_incremental_merge(*generation, 2);
    GLIFI_REQUIRE(merge.has_value());
    std::array post{mutation("post-a", 2), mutation("post-b", 3)};
    GLIFI_REQUIRE(glifistore::server::PairReadGeneration::can_publish_incremental(**generation, merge->get(),
                                                                                  post.size()));
    generation = glifistore::server::PairReadGeneration::publish_incremental(std::move(*generation), post,
                                                                             merge->get());
    GLIFI_REQUIRE(generation.has_value());
    GLIFI_REQUIRE(
        !glifistore::server::PairReadGeneration::can_publish_incremental(**generation, merge->get(), 1));
    GLIFI_REQUIRE(glifistore::server::PairReadGeneration::merge_post_entries(**merge) == 2);
}

GLIFI_TEST("paired incremental merge budget amortizes debt across remaining post capacity") {
    const glifistore::WorkerRoutingState routing{};
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{130});
    auto generation = glifistore::server::PairReadGeneration::empty(routing);
    GLIFI_REQUIRE(generation.has_value());
    const auto mutation = [&](const std::string_view key, const std::uint64_t sequence) {
        return glifistore::server::ReadMutation{
            .key = {key, glifistore::hash_key_routing(key, routing)},
            .record = append(*segment, routing, key, "value", sequence, glifistore::Opcode::put),
            .segment = segment,
            .opcode = glifistore::Opcode::put,
        };
    };
    auto cut = mutation("budget-cut", 1);
    generation = glifistore::server::PairReadGeneration::publish_incremental(std::move(*generation),
                                                                             std::span{&cut, 1});
    GLIFI_REQUIRE(generation.has_value());
    auto merge = glifistore::server::PairReadGeneration::start_incremental_merge(*generation, 2);
    GLIFI_REQUIRE(merge.has_value());

    const auto initial_work = glifistore::server::PairReadGeneration::merge_remaining_slots(**merge);
    GLIFI_REQUIRE(initial_work > 2U);
    GLIFI_REQUIRE(glifistore::server::PairReadGeneration::merge_post_capacity_remaining(**merge) == 2U);
    const auto first_budget = glifistore::server::PairReadGeneration::merge_advance_budget(**merge, 1U, 1U);
    GLIFI_REQUIRE(first_budget >= initial_work / 2U);
    GLIFI_REQUIRE(first_budget < initial_work);
    auto advanced = glifistore::server::PairReadGeneration::advance_incremental_merge(**merge, first_budget);
    GLIFI_REQUIRE(advanced.has_value());
    GLIFI_REQUIRE(*advanced == first_budget);

    auto post = mutation("budget-post", 2);
    generation = glifistore::server::PairReadGeneration::publish_incremental(
        std::move(*generation), std::span{&post, 1}, merge->get());
    GLIFI_REQUIRE(generation.has_value());
    GLIFI_REQUIRE(glifistore::server::PairReadGeneration::merge_post_capacity_remaining(**merge) == 1U);
    const auto remaining_work = glifistore::server::PairReadGeneration::merge_remaining_slots(**merge);
    GLIFI_REQUIRE(glifistore::server::PairReadGeneration::merge_advance_budget(**merge, 1U, 1U) ==
                  remaining_work);
}

GLIFI_TEST("paired incremental merge rejects publication from another generation lineage") {
    const glifistore::WorkerRoutingState routing{};
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{14});
    auto cut = glifistore::server::PairReadGeneration::empty(routing);
    GLIFI_REQUIRE(cut.has_value());
    const std::string cut_key{"lineage-cut"};
    const glifistore::server::ReadMutation cut_mutation{
        .key = {cut_key, glifistore::hash_key_routing(cut_key, routing)},
        .record = append(*segment, routing, cut_key, "cut", 1, glifistore::Opcode::put),
        .segment = segment,
        .opcode = glifistore::Opcode::put,
    };
    cut = glifistore::server::PairReadGeneration::publish_incremental(std::move(*cut),
                                                                      std::span{&cut_mutation, 1});
    GLIFI_REQUIRE(cut.has_value());
    auto merge = glifistore::server::PairReadGeneration::start_incremental_merge(*cut, 8);
    GLIFI_REQUIRE(merge.has_value());

    auto unrelated = glifistore::server::PairReadGeneration::empty(routing);
    GLIFI_REQUIRE(unrelated.has_value());
    const std::string post_key{"wrong-lineage"};
    const glifistore::server::ReadMutation post_mutation{
        .key = {post_key, glifistore::hash_key_routing(post_key, routing)},
        .record = append(*segment, routing, post_key, "post", 2, glifistore::Opcode::put),
        .segment = segment,
        .opcode = glifistore::Opcode::put,
    };
    auto rejected = glifistore::server::PairReadGeneration::publish_incremental(
        std::move(*unrelated), std::span{&post_mutation, 1}, merge->get());
    GLIFI_REQUIRE(!rejected.has_value());
    GLIFI_REQUIRE(rejected.error().code == glifistore::ErrorCode::invalid_argument);
    GLIFI_REQUIRE(glifistore::server::PairReadGeneration::merge_post_entries(**merge) == 0);
}

GLIFI_TEST("paired read base lineage charges the memory reserve and releases it on retirement") {
    const glifistore::WorkerRoutingState routing{};
    auto reserve =
        std::make_shared<glifistore::MemoryReserve>(std::uint64_t{1} << 30U, std::filesystem::path{});
    auto segment = std::make_shared<glifistore::Segment>(glifistore::SegmentId{21});
    {
        auto generation = glifistore::server::PairReadGeneration::empty(routing, reserve);
        GLIFI_REQUIRE(generation.has_value());
        const auto empty_charge = reserve->stats().index_charged_bytes;
        GLIFI_REQUIRE(empty_charge > 0U);

        // Each publication merges its Delta into a new base; merged bases inherit the reserve.
        constexpr std::size_t kKeys = 2'000;
        for (std::size_t index = 0; index < kKeys; ++index) {
            // Keys longer than the inline capacity exercise the charged key blocks as well.
            const auto key = "reserve-lineage-key-with-external-bytes-" + std::to_string(index);
            const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key, routing)};
            const auto record = append(*segment, routing, key, "v", index + 1U, glifistore::Opcode::put);
            const glifistore::server::ReadMutation mutation{
                .key = hashed, .record = record, .segment = segment, .opcode = glifistore::Opcode::put};
            generation = glifistore::server::PairReadGeneration::publish(std::move(*generation),
                                                                         std::span{&mutation, 1}, 1);
            GLIFI_REQUIRE(generation.has_value());
        }
        GLIFI_REQUIRE((*generation)->base_entries() == kKeys);
        GLIFI_REQUIRE(reserve->stats().index_charged_bytes > empty_charge);
        const std::string probe{"reserve-lineage-key-with-external-bytes-1999"};
        const auto found =
            (*generation)->get(glifistore::HashedKey{probe, glifistore::hash_key_routing(probe, routing)}, 0);
        GLIFI_REQUIRE(found.has_value());
    }
    GLIFI_REQUIRE(reserve->stats().index_charged_bytes == 0U);
}
