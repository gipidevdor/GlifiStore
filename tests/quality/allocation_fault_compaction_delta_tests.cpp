#include "allocation_fault_harness.hpp"
#include "allocation_fault_test_support.hpp"
#include "allocation_fault_tests_decl.hpp"
#include "persistence/compaction_delta.hpp"

namespace allocation_fault_test {

void run_compaction_delta_without_allocation() {
    glifistore::Index index;
    glifistore::runtime_catalog_detail::CompactionDelta delta{200, 256 * 1024};
    std::string binary_key(1024, 'x');
    binary_key[128] = '\0';
    const auto key = glifistore::HashedKey::compute(binary_key);
    glifistore::RecordRef reference{.segment_id = glifistore::SegmentId{3},
                                    .offset = glifistore::RecordOffset{4096},
                                    .size = glifistore::RecordSize{2048},
                                    .sequence = glifistore::SequenceNumber{1},
                                    .generation = glifistore::GenerationId{1}};
    require(delta.prepare(index).has_value(), "delta reservation failed");
    std::uint64_t active{};
    std::uint64_t sealed{};
    allocation_fault::begin_forbid_all();
    bool replayed{};
    try {
        for (std::uint64_t sequence = 1; sequence <= 100; ++sequence) {
            delta.record(key, std::nullopt);
            reference.sequence = glifistore::SequenceNumber{sequence};
            delta.record(key, reference);
        }
        replayed = delta.replay(index, reference.segment_id, active, sealed).has_value();
    } catch (...) {
        static_cast<void>(allocation_fault::end_forbid_all());
        throw;
    }
    const auto allocated = allocation_fault::end_forbid_all();
    require(replayed && !allocated, "compaction delta record/replay attempted an allocation");
    require(index.find(key) == reference && active == reference.size.value && sealed == 0,
            "allocation-free delta replay lost its final value or accounting");
}

} // namespace allocation_fault_test
