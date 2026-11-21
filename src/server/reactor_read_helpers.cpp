#include "glifistore/server/reactor.hpp"
#include "server/reactor_detail.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace glifistore::server {

void Reactor::observe_read_capacity() {
    if (read_capacity_ == nullptr) {
        return;
    }
    const auto now_ms = read_control_time_ms();
    if (read_window_begin_ms_ == 0) {
        read_window_begin_ms_ = now_ms;
    }
    if (now_ms <= read_window_begin_ms_ || now_ms - read_window_begin_ms_ < 50U) {
        return;
    }
    const auto elapsed_ms = now_ms - read_window_begin_ms_;
    // Sample only lookup/validation CPU, never socket waits or mutation barriers.
    // This observer is deliberately conservative: unmeasured work cannot trigger help.
    const auto estimated_cpu_ns =
        read_window_samples_ == 0 ? 0U
                                  : (read_window_cpu_ns_ / read_window_samples_) * read_window_primary_gets_;
    read_capacity_->observe(
        executor_id_,
        {.begin_ms = read_window_begin_ms_,
         .end_ms = now_ms,
         .demanded_gets_per_second = (read_window_gets_ / elapsed_ms) * 1000U +
                                     ((read_window_gets_ % elapsed_ms) * 1000U) / elapsed_ms,
         .read_cpu_limited = estimated_cpu_ns / elapsed_ms >= 250'000U},
        helper_cpu_ns_);
    read_window_begin_ms_ = now_ms;
    read_window_gets_ = 0;
    read_window_primary_gets_ = 0;
    read_window_cpu_ns_ = 0;
    read_window_samples_ = 0;
}

auto Reactor::process_helper_read_completions() -> Status {
    if (read_capacity_ == nullptr) {
        return {};
    }
    auto& pool = read_capacity_->pool();
    while (auto result = pool.take(executor_id_)) {
        if (helper_reads_outstanding_ == 0) {
            return fail(ErrorCode::corrupted_data, "unexpected read helper completion");
        }
        helper_cpu_ns_ += result->service_cpu_ns;
        Status queued{};
        // No generation adoption or recursive request processing until the borrowed view
        // has been encoded into owning output. Large values use the same scatter owner
        // as the primary path: this is the selected record, never a second GET.
        try {
            auto* current = connection(result->connection);
            if (current != nullptr) {
                ResponseView response{.status = ResponseStatus::ok,
                                      .request_id = result->request_id,
                                      .owner_worker = static_cast<std::uint32_t>(executor_id_),
                                      .worker_count = static_cast<std::uint32_t>(mesh_.size()),
                                      .routing_epoch = kRoutingEpoch};
                current->request_in_flight = false;
                current->in_flight_since = {};
                if (result->error) {
                    response.status = reactor_detail::response_status(Error{.code = *result->error});
                    queued = queue_response(result->connection, response);
                } else if (result->record) {
                    const auto& record = *result->record;
                    if (record.value.size() >= reactor_detail::kMinimumPipelinedScatterValueBytes) {
                        auto value = OwnedValue::from_bytes(record.value, record.sequence.value,
                                                            record.expire_at_ns, record.type);
                        queued = queue_owned_response(result->connection, response, std::move(value), true);
                    } else {
                        response.value = record.value;
                        queued = queue_response(result->connection, response);
                    }
                } else {
                    queued = fail(ErrorCode::corrupted_data, "read helper omitted its result");
                }
            }
        } catch (...) {
            pool.finish(result->helper);
            --helper_reads_outstanding_;
            static_cast<void>(release_cold_read_lease(result->generation_epoch));
            throw;
        }
        pool.finish(result->helper);
        --helper_reads_outstanding_;
        if (!release_cold_read_lease(result->generation_epoch)) {
            return fail(ErrorCode::corrupted_data, "read helper released an unknown epoch");
        }
        if (!queued) {
            close_connection(result->connection);
            continue;
        }
        // Stale ConnectionTokens are dropped after accounting; they never touch a reused socket.
        if (connection(result->connection) != nullptr) {
            if (auto flushed = write_ready(result->connection); !flushed) {
                close_connection(result->connection);
            }
        }
    }
    return {};
}

} // namespace glifistore::server
