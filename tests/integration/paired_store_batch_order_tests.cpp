// Multi-owner batch ordering (docs/reference/cpp-api.md sections 6 and 7.1; docs/spec/c-abi-v1.md,
// "Mutation outcomes and batches").
//
// put_batch and erase_batch regroup a request by owner, but every owner must still execute its own
// subsequence in caller order: the observable result is the one produced by applying the items one at
// a time in input order. A key always routes to one owner, so repeated keys are the sharpest probe.
//
// std::sort keeps equal elements in input order only for ranges small enough to be insertion-sorted
// (a few dozen elements at most, depending on the library and element type), so grouping a batch by
// owner with an unstable sort must be checked far beyond that range. The batches here use repeated
// keys with distinct values, erase-after-erase, and alternating PUT and ERASE rounds. Input is
// generated from a fixed seed and the keys are chosen through the Store's own routing, so every run
// replays the same batch and always crosses all owners.

#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/store/store.hpp"
#include "test.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using glifistore::ErrorCode;
using glifistore::Status;
using glifistore::Store;
using glifistore::StoreConfig;

constexpr std::size_t kOwnerCount = 4;
constexpr std::size_t kKeysPerOwner = 6;
constexpr std::size_t kMaximumKeyCandidates = 100'000;

enum class RoundKind {
    put_batch,   ///< One put_batch of item_count items.
    erase_batch, ///< One erase_batch of item_count items.
    put_each,    ///< item_count single put() calls: seeds state without using the batch path.
};

struct Round {
    RoundKind kind{};
    std::size_t item_count{};
};

// Every batch size is well beyond the small-range sort thresholds, and the PUT rounds span several
// publication chunks (at most 32 items) per owner.
constexpr std::array<Round, 3> kPutBatchPlan{{
    {.kind = RoundKind::put_batch, .item_count = 257},
    {.kind = RoundKind::put_batch, .item_count = 96},
    {.kind = RoundKind::put_batch, .item_count = 1'031},
}};

// State is seeded with single puts so a failure here is attributable to erase_batch alone. Repeated keys
// make the first erase of a key succeed and every later one report not_found.
constexpr std::array<Round, 4> kEraseBatchPlan{{
    {.kind = RoundKind::put_each, .item_count = 96},
    {.kind = RoundKind::erase_batch, .item_count = 257},
    {.kind = RoundKind::put_each, .item_count = 96},
    {.kind = RoundKind::erase_batch, .item_count = 129},
}};

constexpr std::array<Round, 6> kAlternatingPlan{{
    {.kind = RoundKind::put_batch, .item_count = 257},
    {.kind = RoundKind::erase_batch, .item_count = 129},
    {.kind = RoundKind::put_batch, .item_count = 96},
    {.kind = RoundKind::erase_batch, .item_count = 65},
    {.kind = RoundKind::put_batch, .item_count = 1'031},
    {.kind = RoundKind::erase_batch, .item_count = 520},
}};

// Per-item fsync makes embedded durable_sync far slower per item, so it replays a shorter plan.
constexpr std::array<Round, 4> kDurableSyncAlternatingPlan{{
    {.kind = RoundKind::put_batch, .item_count = 129},
    {.kind = RoundKind::erase_batch, .item_count = 65},
    {.kind = RoundKind::put_batch, .item_count = 97},
    {.kind = RoundKind::erase_batch, .item_count = 70},
}};

using Model = std::map<std::string, std::string, std::less<>>;

auto bytes(const std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

auto text_of(const glifistore::OwnedValue& value) -> std::string_view {
    return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

[[noreturn]] void order_violation(const std::string_view label, const std::string& detail) {
    throw std::runtime_error{"batch order violated [" + std::string{label} + "]: " + detail};
}

auto describe(const Status& status) -> std::string {
    if (status.has_value()) {
        return "success";
    }
    return "error code " + std::to_string(static_cast<int>(status.error().code)) + " '" +
           status.error().message + "'";
}

/// SplitMix64: a fixed seed replays the same batch on every platform and standard library.
class DeterministicStream final {
  public:
    explicit DeterministicStream(const std::uint64_t seed) noexcept : state_(seed) {}

    auto below(const std::size_t bound) noexcept -> std::size_t {
        state_ += 0x9E3779B97F4A7C15ULL;
        auto mixed = state_;
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
        mixed ^= mixed >> 31U;
        return static_cast<std::size_t>(mixed % bound);
    }

  private:
    std::uint64_t state_;
};

class ScopedDirectory final {
  public:
    ScopedDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-batch-order-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error{"batch order temporary directory creation failed"};
        }
        root_ = created;
    }

    ~ScopedDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    ScopedDirectory(const ScopedDirectory&) = delete;
    auto operator=(const ScopedDirectory&) -> ScopedDirectory& = delete;

    [[nodiscard]] auto store_path() const -> std::filesystem::path {
        return root_ / "store";
    }

  private:
    std::filesystem::path root_;
};

/// Keys spread evenly over the owners. A batch drawn from them therefore takes the multi-owner path
/// (never the single-owner shortcut) whatever the routing hash is.
auto keys_spread_over_owners(const StoreConfig& config) -> std::vector<std::string> {
    const auto routing = config.worker_routing.state();
    std::vector<std::vector<std::string>> by_owner(kOwnerCount);
    std::size_t complete_owners = 0;
    for (std::size_t candidate = 0; candidate < kMaximumKeyCandidates && complete_owners < kOwnerCount;
         ++candidate) {
        auto key = "order-key-" + std::to_string(candidate);
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
    if (complete_owners != kOwnerCount) {
        throw std::runtime_error{"batch order fixture found no key set covering every owner"};
    }
    // Interleave owners so neighbouring pool entries belong to different owners.
    std::vector<std::string> keys;
    keys.reserve(kOwnerCount * kKeysPerOwner);
    for (std::size_t slot = 0; slot < kKeysPerOwner; ++slot) {
        for (auto& bucket : by_owner) {
            keys.push_back(std::move(bucket[slot]));
        }
    }
    return keys;
}

void expect_state(Store& store, const std::vector<std::string>& keys, const Model& model,
                  const std::string_view label) {
    for (const auto& key : keys) {
        const auto got = store.get(key);
        const auto expected = model.find(key);
        if (expected == model.end()) {
            if (got.has_value() || got.error().code != ErrorCode::not_found) {
                order_violation(label, "key " + key + " should be absent");
            }
        } else if (!got.has_value()) {
            order_violation(label, "key " + key + " should hold '" + expected->second + "' but is absent");
        } else if (text_of(*got) != expected->second) {
            order_violation(label, "key " + key + " should hold '" + expected->second + "' but holds '" +
                                       std::string{text_of(*got)} + "'");
        }
    }
}

struct PutDraw {
    std::vector<std::string> keys;
    std::vector<std::string> values;
};

/// Draws PUT items over repeated keys. Every item's value is unique, so any reordering of one key's
/// items changes the final value.
auto draw_puts(const std::vector<std::string>& keys, DeterministicStream& stream,
               const std::size_t item_count, const std::string_view label) -> PutDraw {
    PutDraw draw;
    draw.keys.reserve(item_count);
    draw.values.reserve(item_count);
    for (std::size_t index = 0; index < item_count; ++index) {
        draw.keys.push_back(keys[stream.below(keys.size())]);
        draw.values.push_back(std::string{label} + "/item-" + std::to_string(index));
    }
    return draw;
}

/// Applies one put_batch and checks every positional status against the in-order oracle.
void run_put_batch_round(Store& store, const std::vector<std::string>& keys, DeterministicStream& stream,
                         Model& model, const std::size_t item_count, const std::string_view label) {
    const auto draw = draw_puts(keys, stream, item_count, label);
    std::vector<Store::PutItem> items;
    items.reserve(item_count);
    for (std::size_t index = 0; index < item_count; ++index) {
        items.push_back({.key = draw.keys[index], .value = bytes(draw.values[index])});
    }

    const auto statuses = store.put_batch(items);
    if (statuses.size() != item_count) {
        order_violation(label, "put_batch returned " + std::to_string(statuses.size()) + " statuses for " +
                                   std::to_string(item_count) + " items");
    }
    for (std::size_t index = 0; index < item_count; ++index) {
        model.insert_or_assign(draw.keys[index], draw.values[index]);
        if (!statuses[index].has_value()) {
            order_violation(label, "put_batch item " + std::to_string(index) + " (" + draw.keys[index] +
                                       ") returned " + describe(statuses[index]));
        }
    }
}

/// Applies the same draw as individual put() calls, which never use the batch path.
void run_put_each_round(Store& store, const std::vector<std::string>& keys, DeterministicStream& stream,
                        Model& model, const std::size_t item_count, const std::string_view label) {
    const auto draw = draw_puts(keys, stream, item_count, label);
    for (std::size_t index = 0; index < item_count; ++index) {
        const auto status = store.put(draw.keys[index], bytes(draw.values[index]));
        if (!status.has_value()) {
            order_violation(label, "put item " + std::to_string(index) + " (" + draw.keys[index] +
                                       ") returned " + describe(status));
        }
        model.insert_or_assign(draw.keys[index], draw.values[index]);
    }
}

/// Applies one erase_batch. The statuses reveal which item of a repeated key ran first: only that one
/// finds the key present.
void run_erase_batch_round(Store& store, const std::vector<std::string>& keys, DeterministicStream& stream,
                           Model& model, const std::size_t item_count, const std::string_view label) {
    std::vector<std::string> item_keys;
    item_keys.reserve(item_count);
    for (std::size_t index = 0; index < item_count; ++index) {
        item_keys.push_back(keys[stream.below(keys.size())]);
    }
    std::vector<Store::EraseItem> items;
    items.reserve(item_count);
    for (const auto& key : item_keys) {
        items.push_back({.key = key});
    }

    const auto statuses = store.erase_batch(items);
    if (statuses.size() != item_count) {
        order_violation(label, "erase_batch returned " + std::to_string(statuses.size()) + " statuses for " +
                                   std::to_string(item_count) + " items");
    }
    for (std::size_t index = 0; index < item_count; ++index) {
        const bool present_at_this_item = model.erase(item_keys[index]) != 0U;
        const auto& status = statuses[index];
        const auto where = "erase_batch item " + std::to_string(index) + " (" + item_keys[index] + ") ";
        if (present_at_this_item && !status.has_value()) {
            order_violation(label, where + "should succeed but returned " + describe(status));
        }
        if (!present_at_this_item && (status.has_value() || status.error().code != ErrorCode::not_found)) {
            order_violation(label, where + "should be not_found but returned " + describe(status));
        }
    }
}

void run_plan(Store& store, const std::vector<std::string>& keys, Model& model,
              const std::span<const Round> plan, const std::uint64_t seed, const std::string_view label) {
    DeterministicStream stream{seed};
    for (std::size_t round_index = 0; round_index < plan.size(); ++round_index) {
        const auto& round = plan[round_index];
        auto round_label = std::string{label} + " round " + std::to_string(round_index);
        switch (round.kind) {
        case RoundKind::put_batch:
            round_label += " put_batch " + std::to_string(round.item_count);
            run_put_batch_round(store, keys, stream, model, round.item_count, round_label);
            break;
        case RoundKind::erase_batch:
            round_label += " erase_batch " + std::to_string(round.item_count);
            run_erase_batch_round(store, keys, stream, model, round.item_count, round_label);
            break;
        case RoundKind::put_each:
            round_label += " put_each " + std::to_string(round.item_count);
            run_put_each_round(store, keys, stream, model, round.item_count, round_label);
            break;
        }
        expect_state(store, keys, model, round_label + " final state");
    }
}

void prove_volatile_input_order(StoreConfig config, const std::span<const Round> plan,
                                const std::string_view label) {
    config.worker_config.explicit_count = kOwnerCount;
    const auto keys = keys_spread_over_owners(config);
    auto opened = Store::open(config);
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(store.worker_count() == kOwnerCount);
    Model model;
    run_plan(store, keys, model, plan, 0x5EEDB47C0DEULL, label);
    GLIFI_REQUIRE(store.close().has_value());
}

/// The durable variant also proves the persisted write order: recovery must rebuild the same state.
void prove_durable_input_order(StoreConfig config, const std::span<const Round> plan,
                               const std::string_view label) {
    const ScopedDirectory directory;
    config.worker_config.explicit_count = kOwnerCount;
    config.data_directory = directory.store_path();
    config.durable_open_mode = glifistore::DurableOpenMode::create_new;
    const auto keys = keys_spread_over_owners(config);
    Model model;
    {
        auto opened = Store::open(config);
        GLIFI_REQUIRE(opened.has_value());
        auto& store = **opened;
        GLIFI_REQUIRE(store.worker_count() == kOwnerCount);
        run_plan(store, keys, model, plan, 0xD07AB1E5EEDULL, label);
        GLIFI_REQUIRE(store.close().has_value());
    }
    {
        config.durable_open_mode = glifistore::DurableOpenMode::open_existing;
        auto reopened = Store::open(config);
        GLIFI_REQUIRE(reopened.has_value());
        auto& store = **reopened;
        expect_state(store, keys, model, std::string{label} + " after reopen");
        GLIFI_REQUIRE(store.close().has_value());
    }
}

auto volatile_writer_path_config() -> StoreConfig {
    StoreConfig config;
    config.paired.async_lane_capacity = 8;
    config.paired.async_lane_payload_bytes = 1U * 1024U * 1024U;
    return config;
}

} // namespace

GLIFI_TEST("paired volatile Store put_batch keeps input order across owners") {
    prove_volatile_input_order({}, kPutBatchPlan, "volatile put_batch");
}

GLIFI_TEST("paired volatile Store erase_batch keeps input order across owners") {
    prove_volatile_input_order({}, kEraseBatchPlan, "volatile erase_batch");
}

GLIFI_TEST("paired volatile Store alternating batches keep input order across owners") {
    prove_volatile_input_order({}, kAlternatingPlan, "volatile alternating");
}

GLIFI_TEST("paired volatile Writer-path Store alternating batches keep input order across owners") {
    prove_volatile_input_order(volatile_writer_path_config(), kAlternatingPlan, "volatile Writer");
}

GLIFI_TEST("paired durable_sync Store alternating batches keep input order across owners and reopen") {
    StoreConfig config;
    config.storage_mode = glifistore::StorageMode::durable_sync;
    prove_durable_input_order(config, kDurableSyncAlternatingPlan, "durable_sync alternating");
}

// The erase rounds reject many absent keys behind unflushed siblings of the same Writer sub-batch
// (ADR 0047), so this also proves that such rejections neither reorder nor poison the group commit.
GLIFI_TEST(
    "paired durable_group Writer-path Store alternating batches keep input order across owners and reopen") {
    StoreConfig config;
    config.paired.async_lane_capacity = 8;
    config.paired.async_lane_payload_bytes = 1U * 1024U * 1024U;
    config.storage_mode = glifistore::StorageMode::durable_group;
    config.durable_group = {.max_records = 32, .max_bytes = 65'536, .max_wait_ms = 10, .min_records = 1};
    config.maintenance.mode = glifistore::MaintenanceMode::disabled;
    prove_durable_input_order(config, kAlternatingPlan, "durable_group Writer alternating");
}
