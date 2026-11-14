#include "glifistore/store/paired/volatile_sync_chunk.hpp"

#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/hot_path_phases.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/store/paired/mutation_execution.hpp"
#include "store/store_internal.hpp"

#include <exception>
#include <utility>

namespace glifistore::store::paired {
namespace {

[[nodiscard]] auto
node_store_entered(const std::array<VolatileSyncMutationView*, kMaximumPublicationBatch>& published_nodes,
                   const std::size_t publication_count, const VolatileSyncMutationView* target) noexcept
    -> bool {
    for (std::size_t published = 0; published < publication_count; ++published) {
        if (published_nodes[published] == target) {
            return true;
        }
    }
    return false;
}

void invoke_hook(const std::function<void()>* hook) {
    if (hook == nullptr) {
        return;
    }
    (*hook)();
}

void handle_volatile_sync_exception(
    const VolatileSyncChunkMode mode, const bool store_mutated, const bool generation_published,
    std::span<VolatileSyncMutationView> views,
    const std::array<VolatileSyncMutationView*, kMaximumPublicationBatch>& published_nodes,
    const std::size_t publication_count, const std::function<void()>* publish_fail_closed,
    const char* writer_failure_message) {
    // Sticky on any Store-entered catch — including post-publish Site::publish faults
    // (ACK stays success when generation_published; pair must still fail-closed).
    if (store_mutated) {
        invoke_hook(publish_fail_closed);
    }
    for (auto& view : views) {
        if (generation_published && view.status) {
            continue;
        }
        if (mode == VolatileSyncChunkMode::combiner) {
            if (!view.status) {
                continue;
            }
            view.status = Status{fail(store_mutated ? ErrorCode::unavailable : ErrorCode::resource_exhausted,
                                      "paired mutation allocation failed")};
            continue;
        }
        const bool store_entered = node_store_entered(published_nodes, publication_count, &view);
        if (store_entered) {
            view.status = Status{fail(ErrorCode::unavailable, writer_failure_message)};
        } else if (!view.status) {
            continue;
        } else if (store_mutated) {
            view.status = Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
        } else {
            view.status = Status{fail(ErrorCode::resource_exhausted, writer_failure_message)};
        }
    }
}

[[nodiscard]] auto
apply_store_mutations(Store& store, const std::size_t shard, const VolatileSyncChunkMode mode,
                      std::atomic_bool& healthy, std::span<VolatileSyncMutationView> views,
                      std::array<ReadMutation, kMaximumPublicationBatch>& publications,
                      std::array<VolatileSyncMutationView*, kMaximumPublicationBatch>& published_nodes,
                      std::size_t& publication_count, const std::function<void()>* publish_fail_closed)
    -> bool {
    const auto apply_one = [&](VolatileSyncMutationView& view) {
        GS_FAULT_SITE(mutate);
        const auto& key = *view.key;
        auto published =
            view.kind == VolatileSyncMutationView::Kind::put
                ? detail::StoreAccess::put_volatile_published(
                      store, shard, key, view.value, view.expire_at_ns,
                      detail::StoreAccess::PublishedAdmission::caller_holds_guard)
                : detail::StoreAccess::erase_volatile_published(
                      store, shard, key, detail::StoreAccess::PublishedAdmission::caller_holds_guard);
        if (!published) {
            bool sticky = false;
            auto error = classify_volatile_mutation_error(published.error(), sticky);
            if (sticky) {
                invoke_hook(publish_fail_closed);
            }
            view.status = Status{unexpected(std::move(error))};
            return;
        }
        publications[publication_count] = ReadMutation{.key = key,
                                                       .record = published->record,
                                                       .segment = std::move(published->segment),
                                                       .opcode = published->opcode};
        published_nodes[publication_count] = &view;
        ++publication_count;
        if (mode == VolatileSyncChunkMode::dedicated_writer &&
            glifistore::fault::consume_fail(glifistore::fault::Site::mutate)) {
            invoke_hook(publish_fail_closed);
        }
    };

    if (mode == VolatileSyncChunkMode::dedicated_writer) {
        GS_PHASE_PUT(worker_apply);
        for (auto& view : views) {
            if (!healthy.load(std::memory_order_acquire)) {
                for (auto* remaining = &view; remaining < views.data() + views.size(); ++remaining) {
                    remaining->status =
                        Status{fail(ErrorCode::resource_exhausted, "paired runtime is fail-closed")};
                }
                return false;
            }
            apply_one(view);
        }
        return true;
    }

    for (auto& view : views) {
        apply_one(view);
    }
    return true;
}

} // namespace

void apply_volatile_sync_publication_chunk(
    Store& store, const std::size_t shard, LanePublicationContext& publication,
    const std::span<VolatileSyncMutationView> views,
    std::optional<GenerationSlotPool::Reservation>& slot_reservation, const VolatileSyncChunkMode mode,
    std::atomic_bool& healthy, const std::function<void()>* publish_fail_closed,
    const std::function<void()>* reclaim_after_publish,
    const std::function<void(std::size_t publication_count)>* prepare_publish_retry) {
    if (views.empty()) {
        return;
    }

    std::array<ReadMutation, kMaximumPublicationBatch> publications{};
    std::array<VolatileSyncMutationView*, kMaximumPublicationBatch> published_nodes{};
    std::size_t publication_count = 0;
    bool store_mutated = false;
    bool generation_published = false;

    try {
        // dedicated_writer may return false mid-chunk after Site::mutate sticky: earlier
        // siblings are already Store-staged in publications[], later siblings are stamped
        // resource_exhausted. Still publish staged mutations — never leave default success
        // without Index visibility (GET would miss an ACK'd key).
        static_cast<void>(apply_store_mutations(store, shard, mode, healthy, views, publications,
                                                published_nodes, publication_count, publish_fail_closed));
        store_mutated = publication_count != 0;
        if (publication_count == 0) {
            if (slot_reservation) {
                slot_reservation->reset();
            }
            return;
        }
        bool published_ok = false;
        {
            GS_PHASE_PUT(publish);
            const auto* retry =
                mode == VolatileSyncChunkMode::dedicated_writer ? prepare_publish_retry : nullptr;
            published_ok = publish_incremental_read_mutations(
                publication, std::span{publications.data(), publication_count}, slot_reservation, retry);
        }
        if (!published_ok) {
            invoke_hook(publish_fail_closed);
            for (std::size_t index = 0; index < publication_count; ++index) {
                published_nodes[index]->status =
                    Status{fail(ErrorCode::unavailable, "read publication failed")};
            }
            return;
        }
        generation_published = true;
        for (std::size_t index = 0; index < publication_count; ++index) {
            published_nodes[index]->status = Status{};
        }
        // Mid-chunk sticky: published nodes ACK'd above; never-entered siblings keep
        // the resource_exhausted stamp from apply_store_mutations.
        if (glifistore::fault::consume_fail(glifistore::fault::Site::publish)) {
            throw std::bad_alloc{};
        }
        invoke_hook(reclaim_after_publish);
    } catch (const std::bad_alloc&) {
        handle_volatile_sync_exception(mode, store_mutated, generation_published, views, published_nodes,
                                       publication_count, publish_fail_closed,
                                       "paired mutation allocation failed");
    } catch (...) {
        handle_volatile_sync_exception(mode, store_mutated, generation_published, views, published_nodes,
                                       publication_count, publish_fail_closed, "paired Writer failure");
    }
}

} // namespace glifistore::store::paired
