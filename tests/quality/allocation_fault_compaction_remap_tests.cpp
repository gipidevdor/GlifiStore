#include "allocation_fault_harness.hpp"
#include "allocation_fault_test_support.hpp"
#include "allocation_fault_tests_decl.hpp"

#include <string>

namespace allocation_fault_test {

// ADR 0044: the commit-time remap runs after the next Manifest is durable, so
// it must neither allocate nor fail. Exercise heap-key and inline-key slots.
void run_compaction_remap_without_allocation() {
    glifistore::Index index;
    std::string binary_key(1024, 'x');
    binary_key[128] = '\0';
    const auto heap_key = glifistore::HashedKey::compute(binary_key);
    const auto inline_key = glifistore::HashedKey::compute("inline");
    glifistore::RecordRef source{.segment_id = glifistore::SegmentId{3},
                                 .offset = glifistore::RecordOffset{4096},
                                 .size = glifistore::RecordSize{2048},
                                 .sequence = glifistore::SequenceNumber{1},
                                 .generation = glifistore::GenerationId{1}};
    require(index.insert_or_assign(heap_key, source).has_value(), "remap fixture insert failed");
    require(index.insert_or_assign(inline_key, source).has_value(), "remap fixture insert failed");
    auto output = source;
    output.generation = glifistore::GenerationId{2};
    allocation_fault::begin_forbid_all();
    bool remapped{};
    try {
        remapped = index.replace_if_current(heap_key, source, output) &&
                   index.replace_if_current(inline_key, source, output) &&
                   !index.replace_if_current(inline_key, source, output);
    } catch (...) {
        static_cast<void>(allocation_fault::end_forbid_all());
        throw;
    }
    const auto allocated = allocation_fault::end_forbid_all();
    require(remapped && !allocated, "compaction remap attempted an allocation or missed a slot");
    require(index.find(heap_key) == output && index.find(inline_key) == output,
            "allocation-free remap lost its final reference");
}

} // namespace allocation_fault_test
