#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/hot_path_phases.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/server/reactor.hpp"
#include "server/reactor_detail.hpp"
#include "store/store_internal.hpp"
#include "system_error.hpp"

#include <algorithm>
#include <chrono>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace glifistore::server {

auto Reactor::execute_local(const ConnectionToken token, const RequestView& request,
                            const std::uint64_t key_hash, std::uint64_t& cached_now_ns) -> Status {
    const auto key_string = reactor_detail::key_text(request.key);
    const HashedKey key{.key = key_string, .hash = key_hash};
    ResponseView response{.status = ResponseStatus::ok,
                          .request_id = request.request_id,
                          .owner_worker = static_cast<std::uint32_t>(executor_id_),
                          .worker_count = static_cast<std::uint32_t>(mesh_.size()),
                          .routing_epoch = kRoutingEpoch};
    OwnedValue owned_response;
    bool owns_response_value{};
    // process_frames is entered only from read_ready or write_ready. The read
    // path already owns readable interest and flushes queued output; the write
    // path reconciles interest after process_frames returns. Async submissions
    // therefore leave the next poller transition to their caller—modifying it
    // here would be overwritten before any intervening poller wait.
    GS_PHASE_TCP_NAMED(store_phase, store_op);
    switch (request.opcode) {
    case RequestOpcode::get: {
        auto* current = connection(token);
        if (current == nullptr) {
            return {};
        }
        if (cached_now_ns == 0) {
            cached_now_ns = reactor_detail::current_time_ns();
        }
        // Online backup fence: temporary refusal — OVERLOADED, not INTERNAL_ERROR
        // (OperationGuard fail is bare unavailable without StoreAccess rewrite).
        if (!detail::StoreAccess::admissions_open(store_)) {
            response.status = ResponseStatus::overloaded;
            break;
        }
        if (!local_read_generation_) {
            response.status = ResponseStatus::overloaded;
            break;
        }
        // ADR 0037 Phase C: GET is a visibility barrier for prior mutations on this connection.
        if (!current->mutation_visibility.allows(local_read_generation_->epoch())) {
            pair_writers_.request_read_refresh(executor_id_);
            local_read_generation_ =
                pair_writers_.adopt_read_generation(executor_id_, minimum_cold_read_epoch());
            if (local_read_generation_ == nullptr ||
                !current->mutation_visibility.allows(local_read_generation_->epoch())) {
                response.status = ResponseStatus::overloaded;
                break;
            }
        }
        current->mutation_visibility.clear();
        if (!durable_store_) {
            ready_reads_.fetch_add(1U, std::memory_order_relaxed);
            if (read_capacity_ != nullptr) {
                ++read_window_gets_;
                // The primary already protects this exact generation. Register the borrow
                // before release-publishing the task; every later adoption includes it.
                if (!has_pending_output(*current) && current->cold_reads.empty()) {
                    const auto epoch = local_read_generation_->epoch();
                    if (acquire_cold_read_lease(epoch)) {
                        if (read_capacity_->pool().try_submit(executor_id_, token, request.request_id,
                                                              *local_read_generation_, key_string, key_hash,
                                                              cached_now_ns, wakeup_)) {
                            ++helper_reads_outstanding_;
                            delegated_reads_.fetch_add(1U, std::memory_order_relaxed);
                            current->request_in_flight = true;
                            return {};
                        }
                        if (!release_cold_read_lease(epoch)) {
                            return fail(ErrorCode::corrupted_data, "helper lease rollback underflow");
                        }
                    }
                }
            }
            if (read_capacity_ != nullptr) {
                ++read_window_primary_gets_;
            }
            const bool sample = read_capacity_ != nullptr && (read_window_primary_gets_ % 64U) == 0U;
            const auto cpu_before = sample ? read_thread_cpu_ns() : 0U;
            auto record = local_read_generation_->get_volatile_view(key, cached_now_ns);
            if (sample) {
                read_window_cpu_ns_ += read_thread_cpu_ns() - cpu_before;
                ++read_window_samples_;
            }
            if (!record) {
                response.status = reactor_detail::response_status(record.error());
            } else {
                if (record->value.size() >= reactor_detail::kMinimumPipelinedScatterValueBytes) {
                    owned_response = OwnedValue::from_bytes(record->value, record->sequence.value,
                                                            record->expire_at_ns, record->type);
                    response.value = owned_response.view();
                    owns_response_value = true;
                } else {
                    // The Reader lease protects these bytes until queue_response copies them into
                    // owning output below. No adoption, quiescence or asynchronous handoff intervenes.
                    response.value = record->value;
                }
            }
            break;
        }
        auto published = local_read_generation_->prepare_durable(key);
        if (!published) {
            response.status = reactor_detail::response_status(published.error());
            break;
        }
        // A resident Record that fits the response budget is decoded inline: it never blocks, so
        // it skips the disk-read lane (ADR 0046). Anything else keeps the ADR 0016 path, whose
        // budget check below produces the same outcome as before.
        const auto value_budget = config_.maximum_output_bytes - kResponseHeaderBytes;
        auto record = detail::StoreAccess::prepare_published_durable_get(store_, executor_id_, *published,
                                                                         cached_now_ns, value_budget);
        // ADR 0045 §3: while earlier cold reads of this connection are in flight, this GET
        // takes an ordered slot. Whatever cannot be admitted now (window bytes, disk-read lane,
        // lease) waits for the next completion instead of changing the outcome to OVERLOADED.
        auto& window = current->cold_reads;
        const bool window_open = !window.empty();
        if (!record) {
            response.status = reactor_detail::response_status(record.error());
        } else {
            auto& value = record->value;
            if (value) {
                if (window_open) {
                    const auto headroom = cold_window_headroom(*current);
                    if (headroom < kResponseHeaderBytes ||
                        value->bytes.size() > headroom - kResponseHeaderBytes) {
                        frame_deferred_ = true;
                        return {};
                    }
                }
                owned_response = std::move(value).value();
                response.value = owned_response.view();
                owns_response_value = true;
            } else {
                // Inline Records contain their value, so the Record size bounds the response.
                const auto reserved_value_bytes =
                    std::min<std::size_t>(value_budget, published->reference().size.value);
                const auto reserved_bytes = kResponseHeaderBytes + reserved_value_bytes;
                if (window_open &&
                    (!window.can_append(reserved_bytes) || reserved_bytes > cold_window_headroom(*current))) {
                    frame_deferred_ = true;
                    return {};
                }
                if (disk_reads_outstanding_ >= config_.disk_read_queue_capacity) {
                    if (window_open) {
                        frame_deferred_ = true;
                        return {};
                    }
                    response.status = ResponseStatus::overloaded;
                    break;
                }
                auto& cold = record->cold;
                if (!cold) {
                    response.status = ResponseStatus::internal_error;
                    break;
                }
                if (!window_open && !window.prepare_storage()) {
                    response.status = ResponseStatus::overloaded;
                    break;
                }
                const auto generation_epoch = local_read_generation_->epoch();
                if (!acquire_cold_read_lease(generation_epoch)) {
                    if (window_open) {
                        frame_deferred_ = true;
                        return {};
                    }
                    response.status = ResponseStatus::overloaded;
                    break;
                }
                const auto& cancellation_epoch = read_cancellation_epochs_[token.slot];
                DiskReadTask task{.connection = token,
                                  .request_id = request.request_id,
                                  .generation_epoch = generation_epoch,
                                  .worker_index = executor_id_,
                                  .read = std::move(cold).value(),
                                  .cancellation =
                                      {
                                          .epoch = &cancellation_epoch,
                                          .expected = cancellation_epoch.load(std::memory_order_relaxed),
                                      },
                                  .maximum_value_bytes = reserved_value_bytes,
                                  .completions = &disk_read_completions_,
                                  .wakeup = &wakeup_};
                ++disk_reads_outstanding_;
                if (!disk_reads_.try_submit(std::move(task))) {
                    --disk_reads_outstanding_;
                    if (!release_cold_read_lease(generation_epoch)) {
                        return fail(ErrorCode::corrupted_data, "cold-read lease accounting underflow");
                    }
                    if (window_open) {
                        frame_deferred_ = true;
                        return {};
                    }
                    response.status = ResponseStatus::overloaded;
                    break;
                }
                if (!window.admit_read(request.request_id, reserved_bytes)) {
                    return fail(ErrorCode::corrupted_data, "cold-read window refused a checked admission");
                }
                if (window_open) {
                    cold_reads_pipelined_.fetch_add(1U, std::memory_order_relaxed);
                }
                current->request_in_flight = true;
                return {};
            }
        }
        break;
    }
    case RequestOpcode::put: {
        auto* current = connection(token);
        if (current == nullptr) {
            return {};
        }
        if (detail::StoreAccess::maintenance_mutations_rejected(store_)) {
            pair_writers_.note_rejected(executor_id_);
            response.status = ResponseStatus::overloaded;
            break;
        }
        // Online backup (and close) fence: reject before enqueue. Writer OperationGuard
        // failure is unavailable → wire INTERNAL_ERROR without rewrite on some paths;
        // known-not-committed must be OVERLOADED (mirror maintenance emergency).
        if (!detail::StoreAccess::admissions_open(store_)) {
            pair_writers_.note_rejected(executor_id_);
            response.status = ResponseStatus::overloaded;
            break;
        }
        const auto admission_bytes =
            PairWriterPool::mutation_admission_bytes(request.key.size(), request.value.size());
        if (!admission_bytes || *admission_bytes > config_.durable_mutation_queue_bytes) {
            pair_writers_.note_rejected(executor_id_);
            response.status = ResponseStatus::overloaded;
            break;
        }
        const auto admitted = pair_writers_.try_submit({
            .connection = token,
            .request_id = request.request_id,
            .worker_index = executor_id_,
            .kind = MutationKind::put,
            .key = request.key,
            .key_hash = key_hash,
            .value = request.value,
            .expire_at_ns = request.expire_at_ns,
            .completions = &mutation_completions_,
            .wakeup = &wakeup_,
        });
        if (!admitted) {
            response.status = ResponseStatus::overloaded;
            break;
        }
        ++mutations_outstanding_;
        mutation_bytes_outstanding_ += *admitted;
        ++current->mutations_in_flight;
        ++current->mutation_window_count;
        current->request_in_flight = true;
        return {};
    } break;
    case RequestOpcode::erase: {
        auto* current = connection(token);
        if (current == nullptr) {
            return {};
        }
        if (detail::StoreAccess::maintenance_mutations_rejected(store_)) {
            pair_writers_.note_rejected(executor_id_);
            response.status = ResponseStatus::overloaded;
            break;
        }
        if (!detail::StoreAccess::admissions_open(store_)) {
            pair_writers_.note_rejected(executor_id_);
            response.status = ResponseStatus::overloaded;
            break;
        }
        const auto admission_bytes = PairWriterPool::mutation_admission_bytes(request.key.size(), 0U);
        if (!admission_bytes || *admission_bytes > config_.durable_mutation_queue_bytes) {
            pair_writers_.note_rejected(executor_id_);
            response.status = ResponseStatus::overloaded;
            break;
        }
        const auto admitted = pair_writers_.try_submit({
            .connection = token,
            .request_id = request.request_id,
            .worker_index = executor_id_,
            .kind = MutationKind::erase,
            .key = request.key,
            .key_hash = key_hash,
            .completions = &mutation_completions_,
            .wakeup = &wakeup_,
        });
        if (!admitted) {
            response.status = ResponseStatus::overloaded;
            break;
        }
        ++mutations_outstanding_;
        mutation_bytes_outstanding_ += *admitted;
        ++current->mutations_in_flight;
        ++current->mutation_window_count;
        current->request_in_flight = true;
        return {};
    } break;
    case RequestOpcode::init:
    case RequestOpcode::ping:
    case RequestOpcode::health:
    case RequestOpcode::ready:
    case RequestOpcode::stats:
    case RequestOpcode::backup:
    case RequestOpcode::bind_worker:
        response.status = ResponseStatus::invalid_request;
        break;
    }
    GS_PHASE_FINISH(store_phase);
    if (owns_response_value &&
        owned_response.bytes.size() >= reactor_detail::kMinimumPipelinedScatterValueBytes) {
        return queue_owned_response(token, response, std::move(owned_response), true);
    }
    return queue_response(token, response);
}

auto Reactor::process_messages() -> Status {
    if (auto drained = wakeup_.drain(); !drained) {
        return drained;
    }
    if (auto completed = process_helper_read_completions(); !completed) {
        return completed;
    }
    if (auto completed = process_disk_read_completions(); !completed) {
        return completed;
    }
    if (auto completed = process_mutation_completions(); !completed) {
        return completed;
    }
    return process_handoffs();
}

auto Reactor::process_disk_read_completions() -> Status {
    while (auto completion = disk_read_completions_.try_pop()) {
        if (disk_reads_outstanding_ == 0) {
            return fail(ErrorCode::corrupted_data, "disk-read completion accounting underflow");
        }
        --disk_reads_outstanding_;
        if (!release_cold_read_lease(completion->generation_epoch)) {
            return fail(ErrorCode::corrupted_data, "cold-read completion released an unknown epoch");
        }
        local_read_generation_ = pair_writers_.adopt_read_generation(executor_id_, minimum_cold_read_epoch());
        if (!local_read_generation_) {
            return fail(ErrorCode::unavailable, "paired read generation is unavailable");
        }
        auto* current = connection(completion->connection);
        if (current == nullptr) {
            continue;
        }
        if (!current->request_in_flight || current->cold_reads.pending_reads() == 0) {
            return fail(ErrorCode::corrupted_data, "unexpected disk-read completion");
        }
        HeldResponse decided{.status = ResponseStatus::ok,
                             .request_id = completion->request_id,
                             .owner_worker = static_cast<std::uint32_t>(executor_id_),
                             .worker_count = static_cast<std::uint32_t>(mesh_.size()),
                             .routing_epoch = kRoutingEpoch};
        if (completion->error) {
            decided.status = reactor_detail::response_status(*completion->error);
        } else if (completion->value) {
            decided.value = std::move(*completion->value);
        } else {
            decided.status = ResponseStatus::internal_error;
        }
        // One FIFO lane per Reactor completes a connection's reads in submission order.
        if (!current->cold_reads.complete_read(completion->request_id, std::move(decided))) {
            return fail(ErrorCode::corrupted_data,
                        "disk-read completion does not match the oldest cold read");
        }
        const bool output_was_pending = has_pending_output(*current);
        if (auto released = release_cold_read_responses(completion->connection); !released) {
            close_connection(completion->connection);
            continue;
        }
        current = connection(completion->connection);
        if (current == nullptr) {
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if (current->cold_reads.empty()) {
            current->request_in_flight = false;
            current->in_flight_since = {};
        } else {
            current->in_flight_since = now;
        }
        touch_activity(*current, now);
        // As for mutation completions: resume buffered GETs only while the client keeps up,
        // and admit at most one new cold read per completion.
        if (!current->cold_reads.empty() && !output_was_pending && !current->output_lease &&
            current->input_offset < current->input.size()) {
            if (auto resumed = process_frames(completion->connection, 1U); !resumed) {
                current = connection(completion->connection);
                if (current == nullptr || (!has_pending_output(*current) && current->cold_reads.empty())) {
                    close_connection(completion->connection);
                    continue;
                }
                // A later malformed frame must not discard responses already decided.
                current->close_after_flush = true;
                current->input.clear();
                current->input_offset = 0;
            }
        }
        // Flush the decided responses; write_ready processes pipelined input only
        // after output fully drains. process_frames-on-EAGAIN discarded ACKs.
        if (auto flushed = write_ready(completion->connection); !flushed) {
            close_connection(completion->connection);
        }
    }
    return {};
}

auto Reactor::process_mutation_completions() -> Status {
    while (auto completion = mutation_completions_.try_pop()) {
        // Writer publishes with release before enqueueing the completion. Load
        // with acquire before allowing the same connection to parse a following
        // GET, establishing acknowledged PUT -> visible GET.
        {
            GS_PHASE_PUT(completion_adopt);
            local_read_generation_ =
                pair_writers_.adopt_read_generation(executor_id_, minimum_cold_read_epoch());
        }
        if (!local_read_generation_) {
            return fail(ErrorCode::unavailable, "paired read generation is unavailable");
        }
        GS_PHASE_PUT_NAMED(completion_accounting_phase, completion_accounting);
        if (mutations_outstanding_ == 0) {
            return fail(ErrorCode::corrupted_data, "mutation completion accounting underflow");
        }
        --mutations_outstanding_;
        if (completion->admission_bytes > mutation_bytes_outstanding_) {
            return fail(ErrorCode::corrupted_data, "mutation byte accounting underflow");
        }
        mutation_bytes_outstanding_ -= completion->admission_bytes;
        if (!pair_writers_.release_payload(executor_id_, completion->payload_slot)) {
            // Out-of-order only while drain expiry is armed: abandoned queued work
            // may complete ahead of an earlier in-flight Store mutation.
            if (!pair_writers_.expire_remaining_armed()) {
                return fail(ErrorCode::corrupted_data, "mutation payload completion violated FIFO ownership");
            }
            deferred_mutation_payloads_.push_back(completion->payload_slot);
        } else {
            flush_deferred_mutation_payloads();
        }
        auto* current = connection(completion->connection);
        if (current == nullptr) {
            continue;
        }
        if (!current->request_in_flight || !current->cold_reads.empty() ||
            current->mutations_in_flight == 0) {
            return fail(ErrorCode::corrupted_data, "unexpected mutation completion");
        }
        const bool output_was_pending = has_pending_output(*current);
        --current->mutations_in_flight;
        if (current->mutations_in_flight == 0) {
            current->request_in_flight = false;
            current->in_flight_since = {};
            current->mutation_window_count = 0;
        }
        if (!completion->error && completion->writer_epoch != 0) {
            current->mutation_visibility.raise_to(completion->writer_epoch);
        }
        touch_activity(*current, std::chrono::steady_clock::now());
        GS_PHASE_FINISH(completion_accounting_phase);
        ResponseView response{.status = ResponseStatus::ok,
                              .request_id = completion->request_id,
                              .owner_worker = static_cast<std::uint32_t>(executor_id_),
                              .worker_count = static_cast<std::uint32_t>(mesh_.size()),
                              .routing_epoch = kRoutingEpoch};
        if (completion->error) {
            response.status = reactor_detail::response_status(*completion->error);
        }
        {
            GS_PHASE_PUT(completion_response);
            if (auto queued = queue_response(completion->connection, response); !queued) {
                close_connection(completion->connection);
                continue;
            }
        }
        // Resume already-buffered frames after the completed mutation, before
        // flushing its decided response. At most one new asynchronous request
        // can be admitted; its Writer work may overlap this socket drain while
        // response bytes remain ordered in the connection output buffer.
        if (!output_was_pending && current->input_offset < current->input.size()) {
            GS_PHASE_PUT(completion_pipeline_resume);
            if (auto resumed = process_frames(completion->connection, 1U); !resumed) {
                current = connection(completion->connection);
                if (current == nullptr || !has_pending_output(*current)) {
                    close_connection(completion->connection);
                    continue;
                }
                // A later malformed frame must not discard responses already
                // decided earlier in the pipeline.
                current->close_after_flush = true;
                current->input.clear();
                current->input_offset = 0;
            }
        }
        {
            GS_PHASE_PUT(completion_socket_flush);
            if (auto flushed = write_ready(completion->connection); !flushed) {
                close_connection(completion->connection);
            }
        }
    }
    return {};
}

void Reactor::flush_deferred_mutation_payloads() noexcept {
    while (!deferred_mutation_payloads_.empty()) {
        if (!pair_writers_.release_payload(executor_id_, deferred_mutation_payloads_.front())) {
            return;
        }
        deferred_mutation_payloads_.erase(deferred_mutation_payloads_.begin());
    }
}

} // namespace glifistore::server
