#include "glifistore/store/store.hpp"

#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/hot_path_phases.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/index/index.hpp"
#include "glifistore/persistence/bootstrap.hpp"
#include "glifistore/persistence/resource_limits.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/persistence/store_verify.hpp"
#include "glifistore/segment/global_manager.hpp"
#include "glifistore/store/maintenance.hpp"
#include "glifistore/store/paired/shard_pair_runtime.hpp"
#include "glifistore/store/value.hpp"
#include "glifistore/worker/pool.hpp"
#include "glifistore/worker/topology.hpp"
#include "glifistore/worker/worker.hpp"
#include "store/store_impl.hpp"
#include "store/store_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace glifistore {

[[nodiscard]] auto start_paired_runtime(Store& store, const StoreConfig& config) -> Status {
    return detail::StoreAccess::attach_paired_runtime(store, config);
}

Store::Store(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Store::~Store() {
    try {
        static_cast<void>(close());
    } catch (...) {
        // Destructors cannot surface shutdown errors; explicit return documents
        // the best-effort boundary while close() remains available to callers.
        return;
    }
}

auto Store::open(const StoreConfig& config) -> Result<std::unique_ptr<Store>> try {
    if (config.storage_mode != StorageMode::volatile_memory &&
        config.storage_mode != StorageMode::durable_sync &&
        config.storage_mode != StorageMode::durable_periodic &&
        config.storage_mode != StorageMode::durable_group) {
        return fail(ErrorCode::invalid_argument, "storage mode is unsupported");
    }
    if (config.concurrency != StoreConcurrencyMode::paired &&
        config.concurrency != StoreConcurrencyMode::legacy_mutex) {
        return fail(ErrorCode::invalid_argument, "store concurrency mode is unsupported");
    }
    if (config.durable_open_mode != DurableOpenMode::open_or_create &&
        config.durable_open_mode != DurableOpenMode::open_existing &&
        config.durable_open_mode != DurableOpenMode::create_new) {
        return fail(ErrorCode::invalid_argument, "durable open mode is unsupported");
    }
    if (config.worker_config.maximum_workers == 0 ||
        config.worker_config.maximum_workers > kMaximumWorkerCount) {
        return fail(ErrorCode::invalid_argument, "maximum worker count is outside the supported range");
    }
    if (config.worker_config.explicit_count.has_value() &&
        (*config.worker_config.explicit_count == 0 ||
         *config.worker_config.explicit_count > config.worker_config.maximum_workers)) {
        return fail(ErrorCode::invalid_argument, "explicit worker count is outside the supported range");
    }
    if (config.recovery_now_ns != 0) {
        return fail(ErrorCode::invalid_argument,
                    "recovery_now_ns is no longer supported; inject StoreConfig::clock instead");
    }
    if (auto valid = validate_maintenance_config(config.maintenance); !valid) {
        return unexpected(valid.error());
    }
    const auto topology = detect_worker_topology();
    const auto count = WorkerCountPolicy::choose(topology, config.worker_config);
    if (config.storage_mode == StorageMode::volatile_memory) {
        if (config.data_directory || config.durable_open_mode != DurableOpenMode::open_or_create ||
            config.durable_limits != DurableResourceLimits{}) {
            return fail(ErrorCode::invalid_argument,
                        "volatile storage cannot use durable-only configuration");
        }
        if (auto routing = validate_worker_routing_state(config.worker_routing.state()); !routing) {
            return unexpected(routing.error());
        }
        const auto initial_now_ns = store_detail::sample_clock(config.clock);
        auto impl = std::make_unique<Impl>(
            std::make_unique<VolatileStoreRuntime>(SegmentId{1}, count, config.worker_routing.state()),
            config.worker_routing.state(), config.clock, initial_now_ns);
        auto store = std::unique_ptr<Store>(new Store(std::move(impl)));
        store->impl_->maintenance = std::make_unique<MaintenanceController>(config.maintenance);
        Store* const raw = store.get();
        store->impl_->maintenance->bind_compact(
            [raw](const std::optional<std::size_t> preferred_worker,
                  const std::uint64_t max_copy_bytes) -> Result<CompactionResult> {
                return raw->compact_for_maintenance(preferred_worker, max_copy_bytes);
            });
        store->impl_->maintenance->bind_observe(
            [](MaintenanceObserveRequest) -> Result<MaintenanceObservation> {
                return MaintenanceObservation{.durable = false};
            });
        store->impl_->concurrency = config.concurrency;
        store->impl_->close_drain_deadline_ms = config.close_drain_deadline_ms;
        if (auto paired = start_paired_runtime(*store, config); !paired) {
            static_cast<void>(store->close());
            return unexpected(paired.error());
        }
        // The paired runtime establishes exclusive Worker ownership. Publish
        // that immutable mode before maintenance can inspect the Workers.
        store->impl_->maintenance->start();
        return store;
    }
    if (!config.data_directory || config.data_directory->empty()) {
        return fail(ErrorCode::invalid_argument, "durable storage requires a data directory");
    }
    if (auto valid = validate_durable_resource_limits(config.durable_limits); !valid) {
        return unexpected(valid.error());
    }
    if (config.storage_mode == StorageMode::durable_periodic &&
        config.durable_periodic.sync_interval_ms == 0) {
        return fail(ErrorCode::invalid_argument,
                    "durable_periodic requires sync_interval_ms greater than zero");
    }
    if (config.storage_mode == StorageMode::durable_group) {
        if (auto valid = store_detail::validate_batch_config(config.durable_group); !valid) {
            return unexpected(valid.error());
        }
    }
    if (config.storage_mode == StorageMode::durable_periodic && config.durable_periodic.batch) {
        if (auto valid = store_detail::validate_batch_config(*config.durable_periodic.batch); !valid) {
            return unexpected(valid.error());
        }
    }
    auto directory = DataDirectory::open_and_lock(*config.data_directory,
                                                  store_detail::data_directory_mode(config.durable_open_mode),
                                                  config.filesystem_hooks);
    if (!directory) {
        return unexpected(directory.error());
    }
    if (auto prepared = prepare_durable_store(*directory, config.durable_open_mode, count,
                                              config.worker_config.explicit_count, config.durable_limits,
                                              config.worker_routing);
        !prepared) {
        return unexpected(prepared.error());
    }
    DurableRuntimeOptions runtime_options{};
    runtime_options.limits = config.durable_limits;
    if (config.concurrency == StoreConcurrencyMode::paired) {
        // Generation-only ordinary reads (ADR 0032).
        runtime_options.limits.hot_cache_enabled = false;
        runtime_options.exclusive_writer = true;
    }
    if (config.storage_mode == StorageMode::durable_periodic) {
        runtime_options.commit_sync = SegmentCommitSync::deferred;
        runtime_options.sync_interval_ms = config.durable_periodic.sync_interval_ms;
        runtime_options.batch = config.durable_periodic.batch;
    } else if (config.storage_mode == StorageMode::durable_group) {
        runtime_options.commit_sync = SegmentCommitSync::immediate;
        runtime_options.batch = config.durable_group;
        runtime_options.strict_ack = true;
        runtime_options.sync_interval_ms = config.durable_group.max_wait_ms;
    }
    const auto recovery_now_ns = store_detail::sample_clock(config.clock);
    auto runtime =
        DurableRuntimeCatalog::open_locked(std::move(*directory), recovery_now_ns, runtime_options);
    if (!runtime) {
        return unexpected(runtime.error());
    }
    auto impl = std::make_unique<Impl>(std::move(*runtime), config.clock, recovery_now_ns);
    auto store = std::unique_ptr<Store>(new Store(std::move(impl)));
    store->impl_->maintenance = std::make_unique<MaintenanceController>(config.maintenance);
    Store* const raw = store.get();
    store->impl_->maintenance->bind_compact(
        [raw](const std::optional<std::size_t> preferred_worker,
              const std::uint64_t max_copy_bytes) -> Result<CompactionResult> {
            return raw->compact_for_maintenance(preferred_worker, max_copy_bytes);
        });
    store->impl_->maintenance->bind_observe(
        [raw](MaintenanceObserveRequest req) -> Result<MaintenanceObservation> {
            if (!raw->impl_ || !raw->impl_->durable_runtime) {
                return fail(ErrorCode::unavailable, "durable runtime is unavailable");
            }
            auto observation = raw->impl_->durable_runtime->maintenance_observation(
                raw->impl_->next_compaction_worker.load(std::memory_order_relaxed), raw->impl_->now_ns(),
                req.probe_unread_expired_ttl);
            if (observation && observation->compaction_candidate_worker && !req.probe_unread_expired_ttl) {
                raw->impl_->next_compaction_worker.store((*observation->compaction_candidate_worker + 1U) %
                                                             raw->impl_->worker_count_value,
                                                         std::memory_order_relaxed);
            }
            return observation;
        });
    store->impl_->concurrency = config.concurrency;
    store->impl_->close_drain_deadline_ms = config.close_drain_deadline_ms;
    if (auto paired = start_paired_runtime(*store, config); !paired) {
        static_cast<void>(store->close());
        return unexpected(paired.error());
    }
    store->impl_->maintenance->start();
    return store;
} catch (const std::bad_alloc&) {

    return store_detail::resource_exhausted();
} catch (...) {
    return store_detail::internal_failure();
}

auto Store::worker_count() const noexcept -> std::size_t {
    return impl_->worker_count_value;
}

auto Store::get(const std::string_view key) -> Result<OwnedValue> {
    return get_copy(key);
}

auto Store::get(const std::span<const std::byte> key) -> Result<OwnedValue> {
    return get_copy(key);
}

auto Store::get_copy(const std::string_view key) -> Result<OwnedValue> try {
    PrehashedKey hashed;
    {
        GS_PHASE_GET(hash);
        hashed = PrehashedKey{key, hash_key_routing(key, impl_->routing)};
    }
    std::size_t shard = 0;
    {
        GS_PHASE_GET(route);
        shard = route_worker(hashed.hash, impl_->worker_count_value);
    }
    Impl::OperationGuard operation{*impl_, shard};
    {
        GS_PHASE_GET(admit);
        if (!operation) {
            return store_detail::closed_store();
        }
    }
    const auto now_ns = impl_->now_ns();
    if (impl_->pair_runtime) {
        store::paired::ShardPairRuntime::ReadLease lease{*impl_->pair_runtime, shard};
        {
            GS_PHASE_GET(lease_adopt);
            if (!lease) {
                return fail(ErrorCode::unavailable, "paired read generation is unavailable");
            }
        }
        if (impl_->durable_runtime) {
            auto view = lease.generation()->prepare_durable(hashed);
            if (!view) {
                return unexpected(view.error());
            }
            auto prepared = detail::StoreAccess::prepare_published_durable_get(*this, shard, *view, now_ns);
            if (!prepared) {
                return unexpected(prepared.error());
            }
            auto& value = prepared->value;
            if (value) {
                return std::move(value).value();
            }
            auto& cold = prepared->cold;
            if (cold) {
                return detail::StoreAccess::complete_get_owned(*this, shard, std::move(cold).value());
            }
            return fail(ErrorCode::internal_error, "paired durable GET produced no result");
        }
        // index_lookup / value_copy phases are recorded inside PairReadGeneration::get.
        return lease.generation()->get(hashed, now_ns);
    }
    if (impl_->durable_runtime) {
        GS_PHASE_GET(index_lookup);
        return impl_->durable_runtime->get(hashed, now_ns);
    }
    auto& worker = impl_->volatile_runtime->workers.route(hashed);
    const std::lock_guard lock{worker.mutex_};
    GS_PHASE_GET(index_lookup);
    auto record = worker.get_locked(hashed, now_ns);
    if (!record) {
        return unexpected(record.error());
    }
    GS_PHASE_GET(value_copy);
    return store_detail::copy_value(*record);
} catch (const std::bad_alloc&) {

    return store_detail::resource_exhausted();
} catch (...) {
    impl_->mark_durable_fail_closed();
    return store_detail::internal_failure();
}

auto Store::get_copy(const std::span<const std::byte> key) -> Result<OwnedValue> {
    // Single hash/route/lookup lineage: byte-key overload converges on KeyView.
    return get_copy(store_detail::as_string_view(key));
}

auto Store::put(const std::string_view key, const std::span<const std::byte> value,
                const std::uint64_t expire_at_ns) -> Status try {
    const PrehashedKey hashed{key, hash_key_routing(key, impl_->routing)};
    const auto shard = route_worker(hashed.hash, impl_->worker_count_value);
    Impl::OperationGuard operation{*impl_, shard};
    {
        GS_PHASE_PUT(admit);
        if (!operation) {
            return store_detail::closed_store();
        }
    }
    if (auto rejected = store_detail::reject_if_maintenance_emergency(impl_->maintenance.get()); !rejected) {
        return rejected;
    }
    if (impl_->pair_runtime) {
        return impl_->pair_runtime->mutate(shard, store::paired::MutationKind::put, hashed, value,
                                           expire_at_ns);
    }
    if (impl_->durable_runtime) {
        return store_detail::durable_status(impl_->durable_runtime->put(hashed, value, expire_at_ns));
    }
    return impl_->volatile_runtime->workers.route(hashed).put(hashed, value, expire_at_ns);
} catch (const std::bad_alloc&) {

    return store_detail::resource_exhausted();
} catch (...) {
    impl_->mark_durable_fail_closed();
    return store_detail::internal_failure();
}

auto Store::put(const std::span<const std::byte> key, const std::span<const std::byte> value,
                const std::uint64_t expire_at_ns) -> Status {
    return put(store_detail::as_string_view(key), value, expire_at_ns);
}

auto Store::put_batch(const std::span<const PutItem> items) -> std::vector<Status> try {
    std::vector<Status> statuses(items.size());
    if (items.empty()) {
        return statuses;
    }
    if (auto rejected = store_detail::reject_if_maintenance_emergency(impl_->maintenance.get()); !rejected) {
        for (auto& status : statuses) {
            status = rejected;
        }
        return statuses;
    }

    struct Prepared final {
        HashedKey hashed{};
        std::span<const std::byte> value{};
        std::uint64_t expire_at_ns{};
        std::size_t shard{};
        std::size_t input_index{};
    };

    std::vector<Prepared> prepared;
    prepared.reserve(items.size());
    bool single_shard = true;
    std::size_t first_shard = 0;
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        Prepared entry{.hashed = PrehashedKey{item.key, hash_key_routing(item.key, impl_->routing)},
                       .value = item.value,
                       .expire_at_ns = item.expire_at_ns,
                       .input_index = index};
        entry.shard = route_worker(entry.hashed.hash, impl_->worker_count_value);
        if (index == 0) {
            first_shard = entry.shard;
        } else if (entry.shard != first_shard) {
            single_shard = false;
        }
        prepared.push_back(entry);
    }

    if (!impl_->pair_runtime) {
        for (const auto& entry : prepared) {
#if defined(GLIFISTORE_FAULT_INJECTION)
            // Litmus: arm the real emergency gate after the batch-entry check so
            // later siblings exercise the per-item re-check (mid-batch TOCTOU).
            if (glifistore::fault::consume_fail(glifistore::fault::Site::put_batch_gate)) {
                if (auto* controller = impl_->maintenance.get(); controller != nullptr) {
                    controller->publish_mutations_rejected(true);
                }
            }
#endif
            // Mid-batch / TOCTOU: gate may arm after the batch-entry probe.
            if (auto rejected = store_detail::reject_if_maintenance_emergency(impl_->maintenance.get());
                !rejected) {
                statuses[entry.input_index] = rejected;
                continue;
            }
            Impl::OperationGuard operation{*impl_, entry.shard};
            if (!operation) {
                statuses[entry.input_index] = store_detail::closed_store();
                continue;
            }
            if (impl_->durable_runtime) {
                statuses[entry.input_index] = store_detail::durable_status(
                    impl_->durable_runtime->put(entry.hashed, entry.value, entry.expire_at_ns));
            } else {
                statuses[entry.input_index] = impl_->volatile_runtime->workers.route(entry.hashed)
                                                  .put(entry.hashed, entry.value, entry.expire_at_ns);
            }
        }
        return statuses;
    }

    const auto run_shard_batch = [&](const std::size_t shard, const std::size_t begin,
                                     const std::size_t end) {
        Impl::OperationGuard operation{*impl_, shard};
        if (!operation) {
            for (std::size_t index = begin; index < end; ++index) {
                statuses[prepared[index].input_index] = store_detail::closed_store();
            }
            return;
        }
        const auto count = end - begin;
        std::array<store::paired::ShardPairRuntime::SyncBatchItem, 32> stack_items{};
        std::array<Status, 32> stack_statuses{};
        std::unique_ptr<store::paired::ShardPairRuntime::SyncBatchItem[]> heap_items;
        std::unique_ptr<Status[]> heap_statuses;
        store::paired::ShardPairRuntime::SyncBatchItem* batch_items = stack_items.data();
        Status* batch_statuses = stack_statuses.data();
        if (count > stack_items.size()) {
            heap_items = std::make_unique<store::paired::ShardPairRuntime::SyncBatchItem[]>(count);
            heap_statuses = std::make_unique<Status[]>(count);
            batch_items = heap_items.get();
            batch_statuses = heap_statuses.get();
        }
        for (std::size_t offset = 0; offset < count; ++offset) {
            const auto& entry = prepared[begin + offset];
            batch_items[offset] =
                store::paired::ShardPairRuntime::SyncBatchItem{.kind = store::paired::MutationKind::put,
                                                               .key = &entry.hashed,
                                                               .value = entry.value,
                                                               .expire_at_ns = entry.expire_at_ns};
            batch_statuses[offset] = {};
        }
        const auto batch_status = impl_->pair_runtime->mutate_batch(shard, std::span{batch_items, count},
                                                                    std::span{batch_statuses, count});
        for (std::size_t offset = 0; offset < count; ++offset) {
            if (!batch_status && batch_statuses[offset]) {
                batch_statuses[offset] = batch_status;
            }
            statuses[prepared[begin + offset].input_index] = batch_statuses[offset];
        }
    };

    if (single_shard) {
        run_shard_batch(first_shard, 0, prepared.size());
        return statuses;
    }

    std::sort(prepared.begin(), prepared.end(),
              [](const Prepared& left, const Prepared& right) { return left.shard < right.shard; });
    for (std::size_t begin = 0; begin < prepared.size();) {
        const auto shard = prepared[begin].shard;
        std::size_t end = begin + 1U;
        while (end < prepared.size() && prepared[end].shard == shard) {
            ++end;
        }
        run_shard_batch(shard, begin, end);
        begin = end;
    }
    return statuses;
} catch (const std::bad_alloc&) {

    std::vector<Status> statuses(items.size());
    for (auto& status : statuses) {
        status = store_detail::resource_exhausted();
    }
    return statuses;
} catch (...) {
    impl_->mark_durable_fail_closed();
    std::vector<Status> statuses(items.size());
    for (auto& status : statuses) {
        status = store_detail::internal_failure();
    }
    return statuses;
}

auto Store::erase(const std::string_view key) -> Status try {
    const HashedKey hashed{key, hash_key_routing(key, impl_->routing)};
    const auto shard = route_worker(hashed.hash, impl_->worker_count_value);
    Impl::OperationGuard operation{*impl_, shard};
    if (!operation) {
        return store_detail::closed_store();
    }
    if (auto rejected = store_detail::reject_if_maintenance_emergency(impl_->maintenance.get()); !rejected) {
        return rejected;
    }
    if (impl_->pair_runtime) {
        return impl_->pair_runtime->mutate(shard, store::paired::MutationKind::erase, hashed, {}, 0);
    }
    if (impl_->durable_runtime) {
        return store_detail::durable_status(impl_->durable_runtime->erase(hashed));
    }
    return impl_->volatile_runtime->workers.route(hashed).erase(hashed);
} catch (const std::bad_alloc&) {

    return store_detail::resource_exhausted();
} catch (...) {
    impl_->mark_durable_fail_closed();
    return store_detail::internal_failure();
}

auto Store::erase(const std::span<const std::byte> key) -> Status {
    return erase(store_detail::as_string_view(key));
}

auto Store::erase_batch(const std::span<const EraseItem> items) -> std::vector<Status> try {
    std::vector<Status> statuses(items.size());
    if (items.empty()) {
        return statuses;
    }
    if (auto rejected = store_detail::reject_if_maintenance_emergency(impl_->maintenance.get()); !rejected) {
        for (auto& status : statuses) {
            status = rejected;
        }
        return statuses;
    }

    struct Prepared final {
        HashedKey hashed{};
        std::size_t shard{};
        std::size_t input_index{};
    };

    std::vector<Prepared> prepared;
    prepared.reserve(items.size());
    bool single_shard = true;
    std::size_t first_shard = 0;
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        Prepared entry{.hashed = PrehashedKey{item.key, hash_key_routing(item.key, impl_->routing)},
                       .input_index = index};
        entry.shard = route_worker(entry.hashed.hash, impl_->worker_count_value);
        if (index == 0) {
            first_shard = entry.shard;
        } else if (entry.shard != first_shard) {
            single_shard = false;
        }
        prepared.push_back(entry);
    }

    if (!impl_->pair_runtime) {
        for (const auto& entry : prepared) {
#if defined(GLIFISTORE_FAULT_INJECTION)
            if (glifistore::fault::consume_fail(glifistore::fault::Site::put_batch_gate)) {
                if (auto* controller = impl_->maintenance.get(); controller != nullptr) {
                    controller->publish_mutations_rejected(true);
                }
            }
#endif
            if (auto rejected = store_detail::reject_if_maintenance_emergency(impl_->maintenance.get());
                !rejected) {
                statuses[entry.input_index] = rejected;
                continue;
            }
            Impl::OperationGuard operation{*impl_, entry.shard};
            if (!operation) {
                statuses[entry.input_index] = store_detail::closed_store();
                continue;
            }
            if (impl_->durable_runtime) {
                statuses[entry.input_index] =
                    store_detail::durable_status(impl_->durable_runtime->erase(entry.hashed));
            } else {
                statuses[entry.input_index] =
                    impl_->volatile_runtime->workers.route(entry.hashed).erase(entry.hashed);
            }
        }
        return statuses;
    }

    const auto run_shard_batch = [&](const std::size_t shard, const std::size_t begin,
                                     const std::size_t end) {
        Impl::OperationGuard operation{*impl_, shard};
        if (!operation) {
            for (std::size_t index = begin; index < end; ++index) {
                statuses[prepared[index].input_index] = store_detail::closed_store();
            }
            return;
        }
        const auto count = end - begin;
        std::array<store::paired::ShardPairRuntime::SyncBatchItem, 32> stack_items{};
        std::array<Status, 32> stack_statuses{};
        std::unique_ptr<store::paired::ShardPairRuntime::SyncBatchItem[]> heap_items;
        std::unique_ptr<Status[]> heap_statuses;
        store::paired::ShardPairRuntime::SyncBatchItem* batch_items = stack_items.data();
        Status* batch_statuses = stack_statuses.data();
        if (count > stack_items.size()) {
            heap_items = std::make_unique<store::paired::ShardPairRuntime::SyncBatchItem[]>(count);
            heap_statuses = std::make_unique<Status[]>(count);
            batch_items = heap_items.get();
            batch_statuses = heap_statuses.get();
        }
        for (std::size_t offset = 0; offset < count; ++offset) {
            const auto& entry = prepared[begin + offset];
            batch_items[offset] =
                store::paired::ShardPairRuntime::SyncBatchItem{.kind = store::paired::MutationKind::erase,
                                                               .key = &entry.hashed,
                                                               .value = {},
                                                               .expire_at_ns = 0};
            batch_statuses[offset] = {};
        }
        const auto batch_status = impl_->pair_runtime->mutate_batch(shard, std::span{batch_items, count},
                                                                    std::span{batch_statuses, count});
        for (std::size_t offset = 0; offset < count; ++offset) {
            if (!batch_status && batch_statuses[offset]) {
                batch_statuses[offset] = batch_status;
            }
            statuses[prepared[begin + offset].input_index] = batch_statuses[offset];
        }
    };

    if (single_shard) {
        run_shard_batch(first_shard, 0, prepared.size());
        return statuses;
    }

    std::sort(prepared.begin(), prepared.end(),
              [](const Prepared& left, const Prepared& right) { return left.shard < right.shard; });
    for (std::size_t begin = 0; begin < prepared.size();) {
        const auto shard = prepared[begin].shard;
        std::size_t end = begin + 1U;
        while (end < prepared.size() && prepared[end].shard == shard) {
            ++end;
        }
        run_shard_batch(shard, begin, end);
        begin = end;
    }
    return statuses;
} catch (const std::bad_alloc&) {

    std::vector<Status> statuses(items.size());
    for (auto& status : statuses) {
        status = store_detail::resource_exhausted();
    }
    return statuses;
} catch (...) {
    impl_->mark_durable_fail_closed();
    std::vector<Status> statuses(items.size());
    for (auto& status : statuses) {
        status = store_detail::internal_failure();
    }
    return statuses;
}

auto Store::flush() -> Status try {
    Impl::OperationGuard operation{*impl_, impl_->control_shard()};
    if (!operation) {
        return store_detail::closed_store();
    }
    if (impl_->durable_runtime) {
        return impl_->durable_runtime->flush();
    }
    return {};
} catch (const std::bad_alloc&) {

    return store_detail::resource_exhausted();
} catch (...) {
    impl_->mark_durable_fail_closed();
    return store_detail::internal_failure();
}

auto Store::backup_to(const std::filesystem::path& destination, const bool scan_records)
    -> Result<DurableStoreBackupReport> try {
    if (impl_->lifecycle.load(std::memory_order_acquire) != Impl::LifecycleState::open) {
        return store_detail::closed_store();
    }
    if (!impl_->durable_runtime) {
        return fail(ErrorCode::invalid_argument, "online backup requires a durable Store");
    }

    const auto fence_started = std::chrono::steady_clock::now();
    // Fence admissions before taking compaction_mutex so a compact that already holds the mutex
    // (and an OperationGuard) cannot deadlock against wait_for_active_operations. The fence is
    // counted: a concurrent backup cannot reopen admission while this owner still copies.
    impl_->close_admission();

    struct AdmissionResume final {
        Impl* impl;

        ~AdmissionResume() {
            if (impl != nullptr) {
                impl->resume_admission_if_open();
            }
        }

        void release() noexcept {
            if (impl != nullptr) {
                impl->resume_admission_if_open();
                impl = nullptr;
            }
        }
    } resume{impl_.get()};

    if (!impl_->wait_for_active_operations()) {
        return fail(ErrorCode::unavailable, "backup timed out waiting for active operations");
    }

    if (impl_->lifecycle.load(std::memory_order_acquire) != Impl::LifecycleState::open) {
        return store_detail::closed_store();
    }

    auto copied = [&]() -> Result<DurableStoreBackupReport> {
        std::unique_lock compaction_lock{impl_->compaction_mutex};

        // The counted fence acquired before the wait is still owned here. A
        // preceding backup may release its own fence, but cannot reopen
        // admissions until every concurrent owner has released.
        if (!impl_->wait_for_active_operations()) {
            return fail(ErrorCode::unavailable, "backup timed out waiting for active operations");
        }
        if (impl_->lifecycle.load(std::memory_order_acquire) != Impl::LifecycleState::open) {
            return store_detail::closed_store();
        }
        if (!impl_->durable_runtime || !impl_->durable_runtime->healthy()) {
            return fail(ErrorCode::unavailable, "durable runtime is unavailable");
        }

        return impl_->durable_runtime->backup_to(destination, scan_records);
    }();
    // Resume writers before destination verify: the destination is a private empty directory and
    // does not share catalog locks with the live Store. Source CRC is not re-run on the live catalog
    // after resume (writers may mutate); destination CRC (when scan_records) is the promotion gate
    // (ADR 0034 shorter-fence incremental).
    const auto fence_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - fence_started)
            .count());
    resume.release();

    if (!copied) {
        return unexpected(copied.error());
    }
    copied->admission_fence_ns = fence_ns;

    const auto verify_started = std::chrono::steady_clock::now();
    auto destination_verification = verify_durable_store_path(destination, scan_records);
    if (!destination_verification) {
        return unexpected(destination_verification.error());
    }
    copied->destination_verification = std::move(*destination_verification);
    copied->destination_crc_scanned = scan_records;
    copied->destination_verify_ns =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - verify_started)
                                       .count());
    return copied;
} catch (const std::bad_alloc&) {

    return store_detail::resource_exhausted();
} catch (...) {
    impl_->mark_durable_fail_closed();
    return store_detail::internal_failure();
}

auto Store::compact() -> Result<CompactionResult> {
    return compact_for_maintenance(std::nullopt, 0);
}

auto Store::compact_for_maintenance(const std::optional<std::size_t> preferred_worker,
                                    const std::uint64_t max_copy_bytes) -> Result<CompactionResult> try {
    Impl::OperationGuard operation{*impl_, impl_->control_shard()};
    if (!operation) {
        return store_detail::closed_store();
    }
    GS_FAULT_SITE(compact);
    // try_to_lock is the compact-admission progress bound: a second caller fails
    // closed with sequence_conflict instead of waiting unbounded (GS-CONCUR-LIVE-001).
    std::unique_lock maintenance_lock{impl_->compaction_mutex, std::try_to_lock};
    if (!maintenance_lock.owns_lock()) {
        return fail(ErrorCode::sequence_conflict, "another Store compaction is already running");
    }
    if (impl_->volatile_runtime) {
        const auto now_ns = impl_->now_ns();
        for (std::size_t attempted = 0; attempted < impl_->worker_count_value; ++attempted) {
            const auto worker_index = impl_->next_compaction_worker.load(std::memory_order_relaxed);
            impl_->next_compaction_worker.store((worker_index + 1U) % impl_->worker_count_value,
                                                std::memory_order_relaxed);
            auto result = impl_->volatile_runtime->workers.worker(worker_index).compact(now_ns);
            if (!result) {
                return unexpected(result.error());
            }
            auto& compacted = result.value();
            if (compacted) {
                return store_detail::public_compaction_result(worker_index, compacted.value());
            }
        }
        return CompactionResult{};
    }

    if (preferred_worker) {
        if (*preferred_worker >= impl_->worker_count_value) {
            return fail(ErrorCode::invalid_argument, "maintenance compaction Worker is outside the Store");
        }
        impl_->next_compaction_worker.store((*preferred_worker + 1U) % impl_->worker_count_value,
                                            std::memory_order_relaxed);
        const auto copy_rate = impl_->maintenance ? impl_->maintenance->compaction_copy_rate_limit() : 0U;
        auto result = impl_->durable_runtime->compact_worker(*preferred_worker, impl_->now_ns(),
                                                             max_copy_bytes, copy_rate);
        if (result.outcome == DurableCompactionOutcome::not_compacted && result.error.has_value() &&
            result.error->code == ErrorCode::not_found) {
            return CompactionResult{};
        }
        return store_detail::public_compaction_result(*preferred_worker, std::move(result));
    }

    std::optional<std::size_t> first_candidate;
    std::optional<CompactionResult> last_no_gain;
    for (;;) {
        auto candidate = impl_->durable_runtime->next_compaction_worker(
            impl_->next_compaction_worker.load(std::memory_order_relaxed));
        if (!candidate) {
            return unexpected(candidate.error());
        }
        const auto& candidate_worker = candidate.value();
        if (!candidate_worker || (first_candidate && candidate_worker.value() == first_candidate.value())) {
            if (last_no_gain) {
                return last_no_gain.value();
            }
            return CompactionResult{};
        }
        const auto worker_index = candidate_worker.value();
        if (!first_candidate) {
            first_candidate = worker_index;
        }
        impl_->next_compaction_worker.store((worker_index + 1U) % impl_->worker_count_value,
                                            std::memory_order_relaxed);
        auto result = impl_->durable_runtime->compact_worker(worker_index, impl_->now_ns(), 0);
        if (result.outcome == DurableCompactionOutcome::not_beneficial) {
            auto mapped = store_detail::public_compaction_result(worker_index, std::move(result));
            if (!mapped) {
                return unexpected(mapped.error());
            }
            last_no_gain = *mapped;
            continue;
        }
        return store_detail::public_compaction_result(worker_index, std::move(result));
    }
} catch (const std::bad_alloc&) {

    return store_detail::resource_exhausted();
} catch (...) {
    impl_->mark_durable_fail_closed();
    return store_detail::internal_failure();
}

auto Store::maintenance_snapshot() const -> MaintenanceSnapshot {
    if (!impl_ || !impl_->maintenance) {
        return MaintenanceSnapshot{};
    }
    auto snapshot = impl_->maintenance->snapshot();
    if (impl_->durable_runtime) {
        snapshot.rotation = impl_->durable_runtime->rotation_stats();
    }
    return snapshot;
}

auto Store::close() -> Status try { return impl_->close(); } catch (const std::bad_alloc&) {

    return store_detail::resource_exhausted();
} catch (...) {
    return store_detail::internal_failure();
}

auto Store::verify_index() const -> Status try {
    Impl::OperationGuard operation{*impl_, impl_->control_shard()};
    if (!operation) {
        return store_detail::closed_store();
    }
    if (impl_->durable_runtime) {
        return impl_->durable_runtime->verify_index();
    }
    auto& runtime = *impl_->volatile_runtime;
    // Exclusive Writer: drain Index hot path before locking — mutex alone does not
    // serialize with put/erase_locked_published Index publish.
    std::vector<std::unique_ptr<Worker::ExclusiveIndexQuiesce>> index_quiesces;
    index_quiesces.reserve(runtime.workers.size());
    for (std::size_t index = 0; index < runtime.workers.size(); ++index) {
        auto& worker = runtime.workers.worker(index);
        if (worker.exclusive_writer()) {
            index_quiesces.push_back(std::make_unique<Worker::ExclusiveIndexQuiesce>(worker, true));
        }
    }
    std::vector<std::unique_lock<std::mutex>> worker_locks;
    worker_locks.reserve(runtime.workers.size());
    for (std::size_t index = 0; index < runtime.workers.size(); ++index) {
        worker_locks.emplace_back(runtime.workers.worker(index).mutex_);
    }

    const auto segments = runtime.segment_manager.segments();
    const auto rebuilt = rebuild_index_from_segments(segments, 0, impl_->routing);
    if (!rebuilt) {
        return unexpected(rebuilt.error());
    }
    if (rebuilt->index.stats().size != rebuilt->stats.records_visible) {
        return fail(ErrorCode::corrupted_data, "rebuilt index size does not match visible record count");
    }
    std::size_t worker_entry_count{};
    for (std::size_t index = 0; index < runtime.workers.size(); ++index) {
        const auto& worker = runtime.workers.worker(index);
        if (worker.index().stats().size > rebuilt->index.stats().size) {
            return fail(ErrorCode::corrupted_data, "worker index exceeds rebuilt index size");
        }
        for (const auto& entry : worker.index().entries()) {
            ++worker_entry_count;
            if (route_worker(hash_key_routing(entry.key, impl_->routing), runtime.workers.size()) != index) {
                return fail(ErrorCode::corrupted_data, "worker index entry is in the wrong partition");
            }
            const auto ref = rebuilt->index.find(entry.key);
            if (!ref || *ref != entry.record) {
                return fail(ErrorCode::corrupted_data, "worker index entry does not match rebuilt index");
            }
        }
    }
    if (worker_entry_count != rebuilt->index.stats().size) {
        return fail(ErrorCode::corrupted_data, "worker indexes do not contain every rebuilt entry");
    }
    for (const auto& entry : rebuilt->index.entries()) {
        const auto worker_index =
            route_worker(hash_key_routing(entry.key, impl_->routing), runtime.workers.size());
        const auto ref = runtime.workers.worker(worker_index).index().find(entry.key);
        if (!ref || *ref != entry.record) {
            return fail(ErrorCode::corrupted_data, "rebuilt entry is missing from its worker index");
        }
    }
    return {};
} catch (const std::bad_alloc&) {

    return store_detail::resource_exhausted();
} catch (...) {
    impl_->mark_durable_fail_closed();
    return store_detail::internal_failure();
}

} // namespace glifistore
