// put_batch and erase_batch report one status per item. A resource_exhausted status means "known not
// newly committed by this attempt" (docs/spec/error-taxonomy-v1.md, section 5), and a success status
// means the item is visible when the call returns.
//
// An allocation failure in the middle of a multi-owner batch must keep that promise item by item: the
// owners that were already submitted keep their own results, and only the items that were never
// submitted become resource_exhausted. The test enumerates every allocation the calling thread makes
// during a four-owner batch, fails exactly one of them, and checks each reported status against the
// state the Store actually holds. The Writer threads' own allocations are covered by the paired
// runtime tests.

#include "allocation_fault_test_support.hpp"
#include "allocation_fault_tests_decl.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/store/store.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace allocation_fault_test {
namespace {

constexpr std::size_t kOwnerCount = 4;
// More than the 32 items one publication chunk (and the stack scratch of one owner group) holds, so
// every owner group needs a heap allocation of its own.
constexpr std::size_t kKeysPerOwner = 40;
// A single owner takes its whole batch: above the 32-item chunk, and below the per-owner backlog the
// paired runtime accepts before its Reader reaches quiescence (a seed of 64 puts is refused).
constexpr std::size_t kSingleOwnerKeys = 48;
constexpr std::size_t kMaximumKeyCandidates = 100'000;
constexpr std::size_t kMaximumExpectedAllocations = 4096;

enum class BatchKind { put, erase };

struct BatchScenario {
    std::string_view name;
    BatchKind kind{BatchKind::put};
    glifistore::StoreConcurrencyMode concurrency{glifistore::StoreConcurrencyMode::paired};
    /// True when every key holds an old value before the batch. An unseeded PUT batch inserts new keys,
    /// which makes the in-memory Index allocate for each item.
    bool seeded{true};
    /// True when every key belongs to one owner: the batch takes the single-owner path, which submits
    /// all its items at once and so must succeed or fail as a whole.
    bool single_owner{false};
};

[[nodiscard]] auto old_value(const std::size_t index) -> std::string {
    return "old-" + std::to_string(index);
}

[[nodiscard]] auto new_value(const std::size_t index) -> std::string {
    return "new-" + std::to_string(index);
}

/// Keys spread evenly over the owners, interleaved so neighbouring items belong to different owners:
/// the batch always takes the multi-owner path, whatever the routing hash is.
[[nodiscard]] auto keys_for_every_owner(const glifistore::WorkerRoutingState routing)
    -> std::vector<std::string> {
    std::vector<std::vector<std::string>> by_owner(kOwnerCount);
    std::size_t complete_owners = 0;
    for (std::size_t candidate = 0; candidate < kMaximumKeyCandidates && complete_owners < kOwnerCount;
         ++candidate) {
        auto key = "batch-fault-key-" + std::to_string(candidate);
        const auto owner = glifistore::route_worker(glifistore::hash_key_routing(key, routing), kOwnerCount);
        auto& bucket = by_owner[owner];
        if (bucket.size() == kKeysPerOwner) {
            continue;
        }
        bucket.push_back(std::move(key));
        if (bucket.size() == kKeysPerOwner) {
            ++complete_owners;
        }
    }
    require(complete_owners == kOwnerCount, "batch allocation fixture found no key set covering every owner");
    std::vector<std::string> keys;
    keys.reserve(kOwnerCount * kKeysPerOwner);
    for (std::size_t slot = 0; slot < kKeysPerOwner; ++slot) {
        for (auto& bucket : by_owner) {
            keys.push_back(std::move(bucket[slot]));
        }
    }
    return keys;
}

/// Keys that all belong to owner 0, so a batch drawn from them never spans owners.
[[nodiscard]] auto keys_for_one_owner(const glifistore::WorkerRoutingState routing)
    -> std::vector<std::string> {
    std::vector<std::string> keys;
    for (std::size_t candidate = 0; candidate < kMaximumKeyCandidates && keys.size() < kSingleOwnerKeys;
         ++candidate) {
        auto key = "batch-fault-key-" + std::to_string(candidate);
        if (glifistore::route_worker(glifistore::hash_key_routing(key, routing), kOwnerCount) == 0) {
            keys.push_back(std::move(key));
        }
    }
    require(keys.size() == kSingleOwnerKeys, "batch allocation fixture found too few keys");
    return keys;
}

/// The value a key holds, or nullopt when it is absent. Any other read error is a test failure.
[[nodiscard]] auto observe(glifistore::Store& store, const std::string_view key)
    -> std::optional<std::string> {
    const auto got = store.get(key);
    if (got.has_value()) {
        return value_text(*got);
    }
    require(got.error().code == glifistore::ErrorCode::not_found, "batch allocation fixture read failed");
    return std::nullopt;
}

struct BatchOutcome {
    std::size_t applied{};
    std::size_t refused{};
};

/// Verifies every status against the Store's state and reports how many items were applied and how
/// many were refused as resource_exhausted.
auto check_statuses(glifistore::Store& store, const std::vector<std::string>& keys,
                    const std::span<const glifistore::Status> statuses, const BatchScenario& scenario)
    -> BatchOutcome {
    require(statuses.size() == keys.size(), "batch returned the wrong number of statuses");
    BatchOutcome outcome;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const auto state = observe(store, keys[index]);
        const bool holds_old =
            scenario.seeded ? state.has_value() && *state == old_value(index) : !state.has_value();
        const bool holds_new = state.has_value() && *state == new_value(index);
        const bool applied = scenario.kind == BatchKind::put ? holds_new : !state.has_value();
        // An item is either fully applied or untouched: never a third value.
        require(applied || holds_old, "batch item holds neither its old nor its new state");
        if (statuses[index].has_value()) {
            require(applied, "batch item reported success but was not applied");
            ++outcome.applied;
            continue;
        }
        if (statuses[index].error().code == glifistore::ErrorCode::resource_exhausted) {
            require(!applied, "batch item reported resource_exhausted but was applied");
            ++outcome.refused;
        }
    }
    return outcome;
}

void run_scenario(const BatchScenario& scenario) {
    glifistore::StoreConfig config{
        .worker_config = {.explicit_count = kOwnerCount},
        .concurrency = scenario.concurrency,
        .paired = {.async_lane_capacity = 8,
                   .async_lane_payload_bytes = 1U * 1024U * 1024U,
                   .reader_epoch_lease = true},
    };
    const auto keys = scenario.single_owner ? keys_for_one_owner(config.worker_routing.state())
                                            : keys_for_every_owner(config.worker_routing.state());

    std::vector<std::string> replacement_values;
    std::vector<glifistore::Store::PutItem> put_items;
    std::vector<glifistore::Store::EraseItem> erase_items;
    replacement_values.reserve(keys.size());
    put_items.reserve(keys.size());
    erase_items.reserve(keys.size());
    for (std::size_t index = 0; index < keys.size(); ++index) {
        replacement_values.push_back(new_value(index));
        put_items.push_back({.key = keys[index], .value = bytes(replacement_values.back())});
        erase_items.push_back({.key = keys[index]});
    }

    bool completed{};
    std::size_t injected_failures{};
    std::size_t split_batches{};
    std::size_t fail_at = 0;
    try {
        for (; fail_at < kMaximumExpectedAllocations; ++fail_at) {
            auto opened = glifistore::Store::open(config);
            require(opened.has_value(), "failed to open the batch allocation Store");
            auto& store = **opened;
            for (std::size_t index = 0; scenario.seeded && index < keys.size(); ++index) {
                require(store.put(keys[index], bytes(old_value(index))).has_value(), "batch seed put failed");
            }

            std::vector<glifistore::Status> statuses;
            allocation_fault::arm(fail_at);
            try {
                statuses = scenario.kind == BatchKind::put ? store.put_batch(put_items)
                                                           : store.erase_batch(erase_items);
            } catch (...) {
                static_cast<void>(allocation_fault::disarm());
                throw;
            }
            const auto allocation = allocation_fault::disarm();

            const auto* runtime = glifistore::detail::StoreAccess::shard_pair_runtime(store);
            const bool sticky = runtime != nullptr && !runtime->healthy();
            if (!allocation.fired) {
                std::cout << scenario.name << ": " << allocation.observed << " allocations enumerated\n";
                require(fail_at == allocation.observed,
                        "batch allocation enumeration changed before its end");
                for (const auto& status : statuses) {
                    require(status.has_value(), "batch baseline reported a failure");
                }
                check_statuses(store, keys, statuses, scenario);
                static_cast<void>(store.close());
                completed = true;
                break;
            }
            ++injected_failures;
            // A failure that tripped the paired runtime's sticky fail-closed state is the runtime's own
            // contract (reads of an indeterminate item are not meaningful); the Store-level promise is
            // checked whenever the Store stays healthy.
            if (!sticky) {
                const auto outcome = check_statuses(store, keys, statuses, scenario);
                if (outcome.applied > 0 && outcome.refused > 0) {
                    ++split_batches;
                }
            }
            static_cast<void>(store.close());
        }
    } catch (const std::exception& error) {
        throw std::runtime_error{std::string{scenario.name} + ", allocation " + std::to_string(fail_at) +
                                 " failing: " + error.what()};
    }
    require(completed, "batch allocation enumeration exceeded its limit");
    require(injected_failures > 0, "batch allocation enumeration injected no failure");
    if (scenario.single_owner) {
        // One owner is submitted as a whole, so an allocation failure cannot leave it half applied.
        require(split_batches == 0, "a single-owner batch was applied in part");
    } else {
        // The interesting failures land after some owners were already applied; without one the
        // enumeration would only prove the easy case.
        require(split_batches > 0, "batch allocation enumeration never failed in the middle of a batch");
    }
    std::cout << scenario.name << ": " << injected_failures << " injected failures, " << split_batches
              << " in the middle of a batch\n";
}

} // namespace

void run_multi_owner_batch_allocation_failures() {
    const std::array<BatchScenario, 5> scenarios{{
        {.name = "paired put_batch", .kind = BatchKind::put},
        {.name = "paired erase_batch", .kind = BatchKind::erase},
        {.name = "paired single-owner put_batch", .kind = BatchKind::put, .single_owner = true},
        {.name = "paired single-owner erase_batch", .kind = BatchKind::erase, .single_owner = true},
        {.name = "legacy put_batch",
         .kind = BatchKind::put,
         .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex,
         .seeded = false},
    }};
    // Every scenario runs even when an earlier one fails, so one run shows which paths are affected.
    std::string failures;
    for (const auto& scenario : scenarios) {
        try {
            run_scenario(scenario);
        } catch (const std::exception& error) {
            failures += "\n  ";
            failures += error.what();
        }
    }
    require(failures.empty(), "multi-owner batch allocation failures:" + failures);
}

} // namespace allocation_fault_test
