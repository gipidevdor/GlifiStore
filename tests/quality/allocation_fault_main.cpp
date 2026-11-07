#include "allocation_fault_harness.hpp"
#include "allocation_fault_test_support.hpp"
#include "allocation_fault_tests_decl.hpp"

#include <iostream>

namespace allocation_fault_test {
void run_all_tests() {
    run_slot_pool_mirror_without_allocation();
    run_client_mutation_allocation_failures();
    run_client_late_response_allocation();
    const glifistore::DurableRuntimeOptions synchronous{};
    const glifistore::DurableRuntimeOptions strict_group{
        .commit_sync = glifistore::SegmentCommitSync::immediate,
        .sync_interval_ms = 60'000,
        .batch = glifistore::DurableGroupConfig{.max_records = 1, .max_bytes = 65'536, .max_wait_ms = 60'000},
        .strict_ack = true,
    };

    run_exhaustive_allocation_failures(
        {.name = "synchronous new put", .kind = MutationKind::put_new, .options = synchronous});
    run_exhaustive_allocation_failures({.name = "synchronous update",
                                        .kind = MutationKind::put_update,
                                        .seed = true,
                                        .options = synchronous});
    run_exhaustive_allocation_failures(
        {.name = "synchronous erase", .kind = MutationKind::erase, .seed = true, .options = synchronous});
    run_exhaustive_allocation_failures(
        {.name = "strict group put", .kind = MutationKind::put_new, .options = strict_group});
    run_exhaustive_allocation_failures({.name = "Segment rotation put",
                                        .kind = MutationKind::put_new,
                                        .force_rotation = true,
                                        .options = synchronous});
    run_exhaustive_read_failures();
    run_no_post_write_allocation(synchronous);
    run_no_post_write_allocation(strict_group);
    run_paired_volatile_get_inline_zero_heap();
    run_background_allocation_failure_waiters();
    run_exhaustive_compaction_allocation_failures();
    run_compaction_delta_without_allocation();
    run_volatile_rotation_allocation_failures();
    run_volatile_vacuum_publication_allocation_failures();
    run_index_tombstone_rebuild_allocation_failures();
    run_paired_volatile_multichunk_fail_closed();
    run_paired_durable_sync_multichunk_fail_closed();
    run_paired_volatile_sync_midchunk_fail_closed_resource_exhausted();
    run_paired_volatile_sync_midchunk_catch_preserves_resource_exhausted();
    run_paired_sync_durable_group_catch_preserves_resource_exhausted();
    run_paired_async_durable_coalesced_fail_closed();
    run_paired_async_durable_sibling_publish_after_capture_fail();
    run_paired_durable_batch_stops_after_indeterminate_ttl();
    run_paired_sync_durable_sync_drain_after_capture_fail();
    run_paired_sync_durable_sync_ack_after_publish_catch();
    run_paired_sync_durable_sync_erase_ack_after_publish_catch();
    run_paired_sync_durable_sync_ack_after_index_account();
    run_paired_sync_durable_sync_erase_ack_after_index_account();
    run_paired_async_durable_sync_ack_after_index_account();
    run_paired_sync_durable_group_ack_after_index_account();
    run_paired_volatile_sync_ack_after_publish_catch();
    run_paired_volatile_sync_erase_ack_after_publish_catch();
    run_paired_async_volatile_ack_after_publish_catch();
    run_paired_async_durable_sync_ack_after_publish_catch();
    run_paired_async_durable_sync_erase_ack_after_publish_catch();
    run_paired_async_volatile_erase_ack_after_publish_catch();
    run_paired_async_durable_group_ack_after_index_account();
    run_paired_async_durable_group_erase_ack_after_index_account();
}

} // namespace allocation_fault_test

int main() {
    try {
        allocation_fault_test::run_all_tests();
        std::cout << "allocation fault injection passed\n";
        return 0;
    } catch (const std::exception& error) {
        allocation_fault::forbid_all.store(false, std::memory_order_release);
        allocation_fault::disarm_process();
        static_cast<void>(allocation_fault::disarm());
        std::cerr << "allocation fault injection failed: " << error.what() << '\n';
        return 1;
    }
}
