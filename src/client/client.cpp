#include "glifistore/client/client.hpp"

#include "client_detail.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/tls.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/un.h>
#include <utility>
#include <variant>
#include <vector>

namespace glifistore::client {

using detail::as_bytes;
using detail::Clock;
using detail::connect_socket;
using detail::enrich_error;
using detail::exchange;
using detail::ExchangeFailure;
using detail::ExchangeResult;
using detail::is_mutation;
using detail::is_supported;
using detail::Metadata;
using detail::operation_name;
using detail::OwnedResponse;
using detail::receive_buffered_response;
using detail::response_error;
using detail::send_frame;
using detail::wire_opcode;
using detail::WorkerConnection;

class Client::Impl final {
  public:
    explicit Impl(ClientConfig config) : config_(std::move(config)) {}

    [[nodiscard]] auto initialize() -> Status {
        const bool unix_socket = !config_.unix_socket_path.empty();
        if (unix_socket) {
            if (config_.tls.enable) {
                return fail(ErrorCode::invalid_argument, "TLS is not supported on AF_UNIX clients");
            }
            if (config_.unix_socket_path.size() >= sizeof(sockaddr_un{}.sun_path)) {
                return fail(ErrorCode::invalid_argument, "unix socket path exceeds sockaddr_un capacity");
            }
        } else if (config_.host.empty() || config_.port == 0) {
            return fail(ErrorCode::invalid_argument, "client configuration is outside protocol limits");
        }
        if (config_.connect_timeout_ms == 0 || config_.request_timeout_ms == 0 ||
            config_.maximum_frame_bytes < server::kResponseHeaderBytes ||
            config_.maximum_frame_bytes > server::kMaxFrameBytes || config_.maximum_pipeline_requests == 0 ||
            config_.maximum_pipeline_bytes < server::kRequestHeaderBytes) {
            return fail(ErrorCode::invalid_argument, "client configuration is outside protocol limits");
        }
        if (config_.tls.enable) {
            server::ClientTlsConfig tls_config{
                .enable = true,
                .ca_file = config_.tls.ca_file,
                .certificate_file = config_.tls.cert_file,
                .private_key_file = config_.tls.key_file,
                .server_name = config_.tls.server_name.empty() ? config_.host : config_.tls.server_name,
                .insecure_skip_verify = config_.tls.insecure_skip_verify,
                .handshake_timeout_ms = config_.connect_timeout_ms,
            };
            auto context = server::TlsContext::create_client(tls_config);
            if (!context) {
                return unexpected(context.error());
            }
            tls_context_ = std::move(*context);
            tls_server_name_ = tls_config.server_name;
        }
        auto first = std::make_unique<WorkerConnection>(0);
        auto discovered = bootstrap(*first, std::nullopt);
        if (!discovered) {
            return unexpected(discovered.error());
        }
        worker_count_ = discovered->worker_count;
        routing_epoch_ = discovered->routing_epoch;
        routing_ = discovered->routing;
        connections_.reserve(worker_count_);
        connections_.push_back(std::move(first));
        const Metadata expected{worker_count_, routing_epoch_, routing_};
        for (std::uint32_t worker = 1; worker < worker_count_; ++worker) {
            auto connection = std::make_unique<WorkerConnection>(worker);
            auto connected = bootstrap(*connection, expected);
            if (!connected) {
                close();
                return unexpected(connected.error());
            }
            connections_.push_back(std::move(connection));
        }
        return {};
    }

    [[nodiscard]] auto get(const std::span<const std::byte> key, const RequestOptions options)
        -> Result<std::vector<std::byte>> {
        return read(server::RequestOpcode::get, key, {}, options);
    }

    [[nodiscard]] auto ping(const std::span<const std::byte> payload, const RequestOptions options)
        -> Result<std::vector<std::byte>> {
        return read(server::RequestOpcode::ping, {}, payload, options);
    }

    [[nodiscard]] auto health(const RequestOptions options) -> Result<std::vector<std::byte>> {
        return read(server::RequestOpcode::health, {}, {}, options);
    }

    [[nodiscard]] auto ready(const RequestOptions options) -> Result<std::vector<std::byte>> {
        return read(server::RequestOpcode::ready, {}, {}, options);
    }

    [[nodiscard]] auto stats(const RequestOptions options) -> Result<std::vector<std::byte>> {
        return read(server::RequestOpcode::stats, {}, {}, options);
    }

    [[nodiscard]] auto backup(const std::string_view destination, const RequestOptions options)
        -> Result<std::vector<std::byte>> {
        if (destination.empty()) {
            return fail(ErrorCode::invalid_argument, "backup destination must be non-empty");
        }
        return read(server::RequestOpcode::backup, as_bytes(destination), {}, options);
    }

    [[nodiscard]] auto resolve_deadline(const RequestOptions options) const -> Result<Clock::time_point> {
        const auto timeout_ms = options.timeout.has_value()
                                    ? static_cast<std::uint32_t>(options.timeout->count())
                                    : config_.request_timeout_ms;
        if (timeout_ms == 0 || (options.timeout.has_value() && options.timeout->count() <= 0)) {
            return fail(ErrorCode::invalid_argument, "request timeout must be positive");
        }
        return Clock::now() + std::chrono::milliseconds{timeout_ms};
    }

    [[nodiscard]] auto mutate(const server::RequestOpcode opcode, const std::span<const std::byte> key,
                              const std::span<const std::byte> value, const std::uint64_t expire_at_ns,
                              const RequestOptions options) -> MutationResult {
        const auto operation = opcode == server::RequestOpcode::put ? "put" : "erase";
        if (!healthy_.load(std::memory_order_acquire)) {
            return rejected(enrich_error(
                {ErrorCode::unavailable, "client is closed or routing metadata changed"}, operation,
                std::nullopt, std::nullopt, std::nullopt, 0, std::nullopt, true, false));
        }
        auto deadline = resolve_deadline(options);
        if (!deadline) {
            return rejected(enrich_error(deadline.error(), operation, std::nullopt, std::nullopt,
                                         std::nullopt, 0, std::nullopt, true, false));
        }
        const auto worker = worker_for(key);
        auto& connection = *connections_[worker];
        const std::lock_guard lock{connection.mutex};
        if (!healthy_.load(std::memory_order_acquire)) {
            return rejected(enrich_error({ErrorCode::unavailable, "client closed before mutation admission"},
                                         operation, std::nullopt, worker, routing_epoch_, 0, std::nullopt,
                                         true, false));
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (auto connected = ensure_connected(connection); !connected) {
                return rejected(enrich_error(connected.error(), operation, std::nullopt, worker,
                                             routing_epoch_, 0, std::nullopt, true, false));
            }
            const auto request_id = next_request_id();
            auto encoded = server::encode_request({.opcode = opcode,
                                                   .request_id = request_id,
                                                   .expire_at_ns = expire_at_ns,
                                                   .key = key,
                                                   .value = value});
            if (!encoded) {
                return rejected(enrich_error(encoded.error(), operation, request_id, worker, routing_epoch_,
                                             0, std::nullopt, true, false));
            }
            if (encoded->size() > config_.maximum_frame_bytes) {
                return rejected(enrich_error(
                    {ErrorCode::record_too_large, "request exceeds the configured frame limit"}, operation,
                    request_id, worker, routing_epoch_, 0, std::nullopt, true, false));
            }
            auto result = exchange(connection, *encoded, config_, *deadline);
            if (auto* failure = std::get_if<ExchangeFailure>(&result)) {
                connection.reset();
                if (failure->request_bytes_sent == 0) {
                    if (attempt == 0) {
                        continue;
                    }
                    return rejected(enrich_error(failure->error, operation, request_id, worker,
                                                 routing_epoch_, 0, std::nullopt, true, false));
                }
                return indeterminate(enrich_error(failure->error, operation, request_id, worker,
                                                  routing_epoch_, failure->request_bytes_sent, std::nullopt,
                                                  true, true));
            }
            auto& response = std::get<OwnedResponse>(result);
            if (auto valid = validate_response(response, request_id, worker); !valid) {
                connection.reset();
                return indeterminate(enrich_error(valid.error(), operation, request_id, worker,
                                                  routing_epoch_, encoded->size(), std::nullopt, true, true));
            }
            if (response.status == server::ResponseStatus::ok) {
                if (!response.value.empty()) {
                    connection.reset();
                    return indeterminate(enrich_error(
                        {ErrorCode::corrupted_data, "mutation response value must be empty"}, operation,
                        request_id, worker, routing_epoch_, encoded->size(), std::nullopt, true, true));
                }
                return {.outcome = MutationOutcome::committed};
            }
            auto error =
                enrich_error(response_error(response.status), operation, request_id, worker, routing_epoch_,
                             encoded->size(), static_cast<std::uint16_t>(response.status), true,
                             response.status == server::ResponseStatus::internal_error);
            if (response.status == server::ResponseStatus::internal_error) {
                return indeterminate(std::move(error));
            }
            if (response.status == server::ResponseStatus::wrong_owner ||
                response.status == server::ResponseStatus::not_bound) {
                healthy_.store(false, std::memory_order_release);
            }
            return rejected(std::move(error));
        }
        return rejected(enrich_error({ErrorCode::unavailable, "could not send mutation"}, operation,
                                     std::nullopt, worker, routing_epoch_, 0, std::nullopt, true, false));
    }

    [[nodiscard]] auto execute_pipeline(const std::span<const PipelineRequest> requests,
                                        const RequestOptions options)
        -> Result<std::vector<PipelineResponse>> {
        auto deadline = resolve_deadline(options);
        if (!deadline) {
            return unexpected(deadline.error());
        }
        return execute_pipeline(requests, *deadline);
    }

    [[nodiscard]] auto execute_pipeline(const std::span<const PipelineRequest> requests,
                                        const Clock::time_point deadline)
        -> Result<std::vector<PipelineResponse>> {
        if (requests.empty()) {
            return std::vector<PipelineResponse>{};
        }
        if (!healthy_.load(std::memory_order_acquire)) {
            return fail(ErrorCode::unavailable, "client is closed or routing metadata changed");
        }
        if (requests.size() > config_.maximum_pipeline_requests) {
            return fail(ErrorCode::resource_exhausted, "pipeline exceeds the configured request limit");
        }

        struct EncodedRequest {
            std::uint64_t request_id{};
            std::size_t begin{};
            std::size_t size{};
        };

        const auto worker = worker_for(requests.front().key);
        std::vector<PipelineResponse> responses(requests.size());
        std::vector<EncodedRequest> metadata;
        metadata.reserve(requests.size());
        std::size_t output_size{};
        for (const auto& request : requests) {
            if (!is_supported(request.opcode)) {
                return fail(ErrorCode::invalid_argument, "pipeline request contains an invalid opcode");
            }
            if (worker_for(request.key) != worker) {
                return fail(ErrorCode::invalid_argument, "every pipeline key must route to the same Worker");
            }
            if ((request.opcode == PipelineOpcode::get || request.opcode == PipelineOpcode::erase) &&
                (!request.value.empty() || request.put_options.expire_at_ns != 0)) {
                return fail(ErrorCode::invalid_argument,
                            "GET and ERASE pipeline requests cannot carry PUT fields");
            }
            auto frame_size = server::encoded_request_size({.opcode = wire_opcode(request.opcode),
                                                            .expire_at_ns = request.put_options.expire_at_ns,
                                                            .key = request.key,
                                                            .value = request.value});
            if (!frame_size) {
                return unexpected(frame_size.error());
            }
            if (*frame_size > config_.maximum_frame_bytes ||
                *frame_size > config_.maximum_pipeline_bytes - output_size) {
                return fail(ErrorCode::record_too_large,
                            "pipeline exceeds a configured frame or aggregate byte limit");
            }
            metadata.push_back({.begin = output_size, .size = *frame_size});
            output_size += *frame_size;
        }

        std::vector<std::byte> output(output_size);
        for (std::size_t index = 0; index < requests.size(); ++index) {
            const auto& request = requests[index];
            auto& encoded = metadata[index];
            encoded.request_id = next_request_id();
            auto written =
                server::encode_request(std::span<std::byte>{output}.subspan(encoded.begin, encoded.size),
                                       {.opcode = wire_opcode(request.opcode),
                                        .request_id = encoded.request_id,
                                        .expire_at_ns = request.put_options.expire_at_ns,
                                        .key = request.key,
                                        .value = request.value});
            if (!written) {
                return unexpected(written.error());
            }
        }

        auto& connection = *connections_[worker];
        const std::lock_guard lock{connection.mutex};
        if (!healthy_.load(std::memory_order_acquire)) {
            return fail(ErrorCode::unavailable, "client closed before pipeline admission");
        }
        if (auto connected = ensure_connected(connection); !connected) {
            return unexpected(connected.error());
        }

        const auto mark_unresolved = [&](const std::size_t first, const Error& error,
                                         const std::size_t bytes_sent) {
            for (std::size_t index = first; index < requests.size(); ++index) {
                const auto mutation_may_have_arrived =
                    is_mutation(requests[index].opcode) && bytes_sent > metadata[index].begin;
                responses[index].outcome =
                    mutation_may_have_arrived ? PipelineOutcome::indeterminate : PipelineOutcome::failed;
                responses[index].error = enrich_error(
                    error, operation_name(requests[index].opcode), metadata[index].request_id, worker,
                    routing_epoch_,
                    bytes_sent > metadata[index].begin ? bytes_sent - metadata[index].begin : 0,
                    error.wire_status, is_mutation(requests[index].opcode), mutation_may_have_arrived);
            }
        };

        auto sent = send_frame(connection, output, deadline);
        if (const auto* failure = std::get_if<ExchangeFailure>(&sent)) {
            connection.reset();
            mark_unresolved(0, failure->error, failure->request_bytes_sent);
            return responses;
        }
        const auto bytes_sent = std::get<std::size_t>(sent);
        for (std::size_t index = 0; index < requests.size(); ++index) {
            ExchangeResult result = ExchangeFailure{{ErrorCode::resource_exhausted, {}}, bytes_sent};
            try {
                result = receive_buffered_response(connection, config_, deadline, bytes_sent);
            } catch (const std::bad_alloc&) {
                connection.reset();
                mark_unresolved(
                    index,
                    {ErrorCode::resource_exhausted, "allocation failed while receiving pipeline responses"},
                    bytes_sent);
                return responses;
            }
            if (const auto* failure = std::get_if<ExchangeFailure>(&result)) {
                connection.reset();
                mark_unresolved(index, failure->error, bytes_sent);
                return responses;
            }
            auto& response = std::get<OwnedResponse>(result);
            if (auto valid = validate_response(response, metadata[index].request_id, worker); !valid) {
                connection.reset();
                mark_unresolved(index, valid.error(), bytes_sent);
                return responses;
            }
            if (response.status == server::ResponseStatus::ok) {
                if (is_mutation(requests[index].opcode) && !response.value.empty()) {
                    connection.reset();
                    mark_unresolved(index,
                                    {ErrorCode::corrupted_data, "mutation response value must be empty"},
                                    bytes_sent);
                    return responses;
                }
                responses[index].outcome = PipelineOutcome::succeeded;
                responses[index].value = std::move(response.value);
                continue;
            }
            const auto indeterminate = is_mutation(requests[index].opcode) &&
                                       response.status == server::ResponseStatus::internal_error;
            responses[index].outcome =
                indeterminate ? PipelineOutcome::indeterminate : PipelineOutcome::failed;
            responses[index].error =
                enrich_error(response_error(response.status), operation_name(requests[index].opcode),
                             metadata[index].request_id, worker, routing_epoch_,
                             bytes_sent - metadata[index].begin, static_cast<std::uint16_t>(response.status),
                             is_mutation(requests[index].opcode), indeterminate);
            if (response.status == server::ResponseStatus::wrong_owner ||
                response.status == server::ResponseStatus::not_bound) {
                healthy_.store(false, std::memory_order_release);
            }
        }
        return responses;
    }

    [[nodiscard]] auto execute_batch(const std::span<const PipelineRequest> requests,
                                     const RequestOptions options) -> Result<std::vector<PipelineResponse>> {
        if (requests.empty()) {
            return std::vector<PipelineResponse>{};
        }
        if (!healthy_.load(std::memory_order_acquire)) {
            return fail(ErrorCode::unavailable, "client is closed or routing metadata changed");
        }
        auto deadline = resolve_deadline(options);
        if (!deadline) {
            return unexpected(deadline.error());
        }

        struct WorkerJob {
            std::vector<PipelineRequest> requests;
            std::vector<std::size_t> original_indices;
        };

        std::vector<WorkerJob> jobs(worker_count_);
        for (std::size_t index = 0; index < requests.size(); ++index) {
            const auto& request = requests[index];
            if (!is_supported(request.opcode)) {
                return fail(ErrorCode::invalid_argument, "batch request contains an invalid opcode");
            }
            if ((request.opcode == PipelineOpcode::get || request.opcode == PipelineOpcode::erase) &&
                (!request.value.empty() || request.put_options.expire_at_ns != 0)) {
                return fail(ErrorCode::invalid_argument,
                            "GET and ERASE batch requests cannot carry PUT fields");
            }
            const auto worker = worker_for(request.key);
            if (worker >= worker_count_) {
                return fail(ErrorCode::internal_error, "batch routing produced an invalid Worker");
            }
            auto& job = jobs[worker];
            if (job.requests.size() >= config_.maximum_pipeline_requests) {
                return fail(ErrorCode::resource_exhausted,
                            "batch exceeds the configured per-Worker request limit");
            }
            job.requests.push_back(request);
            job.original_indices.push_back(index);
        }

        std::vector<PipelineResponse> responses(requests.size());

        struct ActiveJob {
            WorkerJob* job{};
            std::future<Result<std::vector<PipelineResponse>>> future;
        };

        std::vector<ActiveJob> active;
        active.reserve(worker_count_);

        const auto shared_deadline = *deadline;
        const auto run_job = [this,
                              shared_deadline](WorkerJob& job) -> Result<std::vector<PipelineResponse>> {
            return execute_pipeline(job.requests, shared_deadline);
        };

        for (auto& job : jobs) {
            if (job.requests.empty()) {
                continue;
            }
            if (active.empty() && std::none_of(jobs.begin(), jobs.end(), [&](const WorkerJob& other) {
                    return &other != &job && !other.requests.empty();
                })) {
                auto executed = run_job(job);
                if (!executed) {
                    for (std::size_t offset = 0; offset < job.original_indices.size(); ++offset) {
                        const auto index = job.original_indices[offset];
                        const auto& request = job.requests[offset];
                        responses[index].outcome = PipelineOutcome::failed;
                        // Group-level pre-admission failure (connect/encode): bytes_sent=0 →
                        // mutation_outcome=rejected, matching mark_unresolved / Erlang.
                        responses[index].error =
                            enrich_error(executed.error(), operation_name(request.opcode), std::nullopt,
                                         worker_for(request.key), routing_epoch_, 0, std::nullopt,
                                         is_mutation(request.opcode), false);
                    }
                    return responses;
                }
                for (std::size_t offset = 0; offset < job.original_indices.size(); ++offset) {
                    responses[job.original_indices[offset]] = std::move((*executed)[offset]);
                }
                return responses;
            }
            active.push_back({
                .job = &job,
                .future = std::async(std::launch::async, run_job, std::ref(job)),
            });
        }

        for (auto& item : active) {
            auto executed = item.future.get();
            if (!executed) {
                for (std::size_t offset = 0; offset < item.job->original_indices.size(); ++offset) {
                    const auto index = item.job->original_indices[offset];
                    const auto& request = item.job->requests[offset];
                    responses[index].outcome = PipelineOutcome::failed;
                    responses[index].error =
                        enrich_error(executed.error(), operation_name(request.opcode), std::nullopt,
                                     worker_for(request.key), routing_epoch_, 0, std::nullopt,
                                     is_mutation(request.opcode), false);
                }
                continue;
            }
            for (std::size_t offset = 0; offset < item.job->original_indices.size(); ++offset) {
                responses[item.job->original_indices[offset]] = std::move((*executed)[offset]);
            }
        }
        return responses;
    }

    [[nodiscard]] auto execute_worker_pipelines(const std::span<const std::vector<PipelineRequest>> batches,
                                                const RequestOptions options)
        -> Result<std::vector<std::vector<PipelineResponse>>> {
        if (!healthy_.load(std::memory_order_acquire)) {
            return fail(ErrorCode::unavailable, "client is closed or routing metadata changed");
        }
        if (batches.size() != worker_count_) {
            return fail(ErrorCode::invalid_argument, "worker pipeline vector does not match Worker count");
        }
        auto deadline = resolve_deadline(options);
        if (!deadline) {
            return unexpected(deadline.error());
        }

        for (std::size_t worker = 0; worker < batches.size(); ++worker) {
            const auto& requests = batches[worker];
            if (requests.size() > config_.maximum_pipeline_requests) {
                return fail(ErrorCode::resource_exhausted, "pipeline exceeds the configured request limit");
            }
            for (const auto& request : requests) {
                if (!is_supported(request.opcode)) {
                    return fail(ErrorCode::invalid_argument, "pipeline request contains an invalid opcode");
                }
                if (worker_for(request.key) != worker) {
                    return fail(ErrorCode::invalid_argument,
                                "pipeline key does not route to the expected Worker");
                }
                if ((request.opcode == PipelineOpcode::get || request.opcode == PipelineOpcode::erase) &&
                    (!request.value.empty() || request.put_options.expire_at_ns != 0)) {
                    return fail(ErrorCode::invalid_argument,
                                "GET and ERASE pipeline requests cannot carry PUT fields");
                }
            }
        }

        std::vector<std::vector<PipelineResponse>> results(worker_count_);

        struct ActiveJob {
            std::size_t worker{};
            const std::vector<PipelineRequest>* requests{};
        };

        std::vector<ActiveJob> planned;
        planned.reserve(worker_count_);
        for (std::size_t worker = 0; worker < batches.size(); ++worker) {
            if (batches[worker].empty()) {
                results[worker] = {};
                continue;
            }
            planned.push_back({.worker = worker, .requests = &batches[worker]});
        }
        if (planned.empty()) {
            return results;
        }

        const auto shared_deadline = *deadline;
        const auto stamp_failed = [&](const std::size_t worker, const std::vector<PipelineRequest>& requests,
                                      const Error& error) {
            results[worker].resize(requests.size());
            for (std::size_t offset = 0; offset < requests.size(); ++offset) {
                const auto& request = requests[offset];
                results[worker][offset].outcome = PipelineOutcome::failed;
                results[worker][offset].error = enrich_error(
                    error, operation_name(request.opcode), std::nullopt, static_cast<std::uint32_t>(worker),
                    routing_epoch_, 0, std::nullopt, is_mutation(request.opcode), false);
            }
        };
        const auto run_job = [this, shared_deadline](const std::vector<PipelineRequest>& requests)
            -> Result<std::vector<PipelineResponse>> { return execute_pipeline(requests, shared_deadline); };

        if (planned.size() == 1) {
            const auto worker = planned.front().worker;
            const auto& requests = *planned.front().requests;
            auto executed = run_job(requests);
            if (!executed) {
                stamp_failed(worker, requests, executed.error());
            } else {
                results[worker] = std::move(*executed);
            }
            return results;
        }

        struct FutureJob {
            ActiveJob job;
            std::future<Result<std::vector<PipelineResponse>>> future;
        };

        std::vector<FutureJob> active;
        active.reserve(planned.size());
        for (const auto& item : planned) {
            active.push_back({
                .job = item,
                .future = std::async(std::launch::async, run_job, std::cref(*item.requests)),
            });
        }
        for (auto& item : active) {
            auto executed = item.future.get();
            const auto& requests = *item.job.requests;
            if (!executed) {
                stamp_failed(item.job.worker, requests, executed.error());
                continue;
            }
            results[item.job.worker] = std::move(*executed);
        }
        return results;
    }

    [[nodiscard]] auto worker_count() const noexcept -> std::uint32_t {
        return worker_count_;
    }

    [[nodiscard]] auto worker_for_key(const std::span<const std::byte> key) const noexcept -> std::uint32_t {
        return worker_for(key);
    }

    [[nodiscard]] auto routing_epoch() const noexcept -> std::uint64_t {
        return routing_epoch_;
    }

    [[nodiscard]] auto routing() const noexcept -> WorkerRoutingState {
        return routing_;
    }

    [[nodiscard]] auto healthy() const noexcept -> bool {
        return healthy_.load(std::memory_order_acquire);
    }

    void close() noexcept {
        healthy_.store(false, std::memory_order_release);
        for (auto& connection : connections_) {
            const std::lock_guard lock{connection->mutex};
            connection->reset();
        }
    }

  private:
    [[nodiscard]] auto bootstrap(WorkerConnection& connection, const std::optional<Metadata> expected)
        -> Result<Metadata> {
        auto opened = connect_socket(config_);
        if (!opened) {
            return unexpected(opened.error());
        }
        connection.reset();
        connection.socket = std::move(*opened);
        if (tls_context_) {
            auto session = tls_context_->connect_socket(connection.socket.get(), tls_server_name_);
            if (!session) {
                connection.reset();
                return unexpected(session.error());
            }
            connection.tls = std::move(*session);
        }
        const auto init_id = next_request_id();
        auto init = server::encode_request({.opcode = server::RequestOpcode::init, .request_id = init_id});
        if (!init) {
            return unexpected(init.error());
        }
        auto initialized = exchange(connection, *init, config_);
        if (const auto* failure = std::get_if<ExchangeFailure>(&initialized)) {
            connection.reset();
            return unexpected(failure->error);
        }
        const auto& response = std::get<OwnedResponse>(initialized);
        auto routing = decode_init_identity_value(response.value);
        if (!routing || response.status != server::ResponseStatus::ok || response.request_id != init_id ||
            response.worker_count == 0 || response.worker_count > 256 || response.routing_epoch == 0) {
            connection.reset();
            return fail(ErrorCode::corrupted_data, "server INIT response is not valid protocol v2 metadata");
        }
        const Metadata metadata{response.worker_count, response.routing_epoch, *routing};
        if (expected &&
            (metadata.worker_count != expected->worker_count ||
             metadata.routing_epoch != expected->routing_epoch || metadata.routing != expected->routing)) {
            connection.reset();
            return fail(ErrorCode::unavailable,
                        "server routing metadata changed during connection bootstrap");
        }
        const auto bind_id = next_request_id();
        auto bind = server::encode_request({.opcode = server::RequestOpcode::bind_worker,
                                            .request_id = bind_id,
                                            .target_worker = connection.worker});
        if (!bind) {
            return unexpected(bind.error());
        }
        auto bound = exchange(connection, *bind, config_);
        if (const auto* failure = std::get_if<ExchangeFailure>(&bound)) {
            connection.reset();
            return unexpected(failure->error);
        }
        const auto& bind_response = std::get<OwnedResponse>(bound);
        if (bind_response.status != server::ResponseStatus::ok || bind_response.request_id != bind_id ||
            bind_response.owner_worker != connection.worker ||
            bind_response.worker_count != metadata.worker_count ||
            bind_response.routing_epoch != metadata.routing_epoch) {
            connection.reset();
            return fail(ErrorCode::corrupted_data, "server BIND_WORKER response is inconsistent");
        }
        return metadata;
    }

    [[nodiscard]] auto ensure_connected(WorkerConnection& connection) -> Status {
        if (connection.socket) {
            return {};
        }
        auto connected = bootstrap(connection, Metadata{worker_count_, routing_epoch_, routing_});
        if (!connected) {
            return unexpected(connected.error());
        }
        return {};
    }

    [[nodiscard]] auto read(const server::RequestOpcode opcode, const std::span<const std::byte> key,
                            const std::span<const std::byte> value, const RequestOptions options)
        -> Result<std::vector<std::byte>> {
        const auto operation = operation_name(opcode);
        if (!healthy_.load(std::memory_order_acquire)) {
            return unexpected(
                enrich_error({ErrorCode::unavailable, "client is closed or routing metadata changed"},
                             operation, std::nullopt, std::nullopt, std::nullopt));
        }
        auto deadline = resolve_deadline(options);
        if (!deadline) {
            return unexpected(
                enrich_error(deadline.error(), operation, std::nullopt, std::nullopt, std::nullopt));
        }
        const auto worker =
            opcode == server::RequestOpcode::ping || opcode == server::RequestOpcode::backup ||
                    opcode == server::RequestOpcode::health || opcode == server::RequestOpcode::ready ||
                    opcode == server::RequestOpcode::stats
                ? 0U
                : worker_for(key);
        auto& connection = *connections_[worker];
        const std::lock_guard lock{connection.mutex};
        if (!healthy_.load(std::memory_order_acquire)) {
            return unexpected(enrich_error({ErrorCode::unavailable, "client closed before read admission"},
                                           operation, std::nullopt, worker, routing_epoch_));
        }
        Error last_error = enrich_error({ErrorCode::unavailable, "request was not attempted"}, operation,
                                        std::nullopt, worker, routing_epoch_);
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (auto connected = ensure_connected(connection); !connected) {
                last_error = enrich_error(connected.error(), operation, std::nullopt, worker, routing_epoch_);
                continue;
            }
            const auto request_id = next_request_id();
            auto encoded = server::encode_request(
                {.opcode = opcode, .request_id = request_id, .key = key, .value = value});
            if (!encoded) {
                return unexpected(
                    enrich_error(encoded.error(), operation, request_id, worker, routing_epoch_));
            }
            if (encoded->size() > config_.maximum_frame_bytes) {
                return unexpected(
                    enrich_error({ErrorCode::record_too_large, "request exceeds the configured frame limit"},
                                 operation, request_id, worker, routing_epoch_));
            }
            auto result = exchange(connection, *encoded, config_, *deadline);
            if (auto* failure = std::get_if<ExchangeFailure>(&result)) {
                connection.reset();
                if (opcode == server::RequestOpcode::backup && failure->request_bytes_sent > 0) {
                    // Backup is not idempotent to the same destination. A lost OK after a
                    // completed backup would make a blind retry fail with "destination not
                    // empty" and falsely claim the backup never succeeded — treat like a
                    // mutation with bytes in flight (indeterminate / reconcile_first).
                    return unexpected(enrich_error(failure->error, operation, request_id, worker,
                                                   routing_epoch_, failure->request_bytes_sent, std::nullopt,
                                                   true, true));
                }
                last_error = enrich_error(failure->error, operation, request_id, worker, routing_epoch_,
                                          failure->request_bytes_sent);
                continue;
            }
            auto& response = std::get<OwnedResponse>(result);
            if (auto valid = validate_response(response, request_id, worker); !valid) {
                connection.reset();
                // BACKUP response arrived after send — fenced copy may already exist.
                // Same polarity as mutate validate-fail / BACKUP INTERNAL_ERROR.
                if (opcode == server::RequestOpcode::backup) {
                    return unexpected(enrich_error(valid.error(), operation, request_id, worker,
                                                   routing_epoch_, encoded->size(), std::nullopt, true,
                                                   true));
                }
                return unexpected(enrich_error(valid.error(), operation, request_id, worker, routing_epoch_,
                                               encoded->size()));
            }
            if (response.status != server::ResponseStatus::ok) {
                if (response.status == server::ResponseStatus::wrong_owner ||
                    response.status == server::ResponseStatus::not_bound) {
                    healthy_.store(false, std::memory_order_release);
                }
                // BACKUP INTERNAL_ERROR may mean the fenced copy already committed (e.g. report
                // formatting failed after backup_to). Same-destination retry is not safe —
                // treat like bytes-in-flight indeterminate / reconcile_first.
                if (opcode == server::RequestOpcode::backup &&
                    response.status == server::ResponseStatus::internal_error) {
                    return unexpected(enrich_error(response_error(response.status), operation, request_id,
                                                   worker, routing_epoch_, encoded->size(),
                                                   static_cast<std::uint16_t>(response.status), true, true));
                }
                return unexpected(enrich_error(response_error(response.status), operation, request_id, worker,
                                               routing_epoch_, encoded->size(),
                                               static_cast<std::uint16_t>(response.status)));
            }
            return std::move(response.value);
        }
        return unexpected(std::move(last_error));
    }

    [[nodiscard]] auto validate_response(const OwnedResponse& response, const std::uint64_t request_id,
                                         const std::uint32_t worker) -> Status {
        if (response.request_id != request_id) {
            return fail(ErrorCode::corrupted_data, "server response request ID does not match");
        }
        if (response.worker_count != worker_count_ || response.routing_epoch != routing_epoch_) {
            healthy_.store(false, std::memory_order_release);
            return fail(ErrorCode::unavailable, "server routing metadata changed");
        }
        if (response.owner_worker != worker && response.status != server::ResponseStatus::wrong_owner) {
            healthy_.store(false, std::memory_order_release);
            return fail(ErrorCode::corrupted_data, "server response came from the wrong worker");
        }
        return {};
    }

    [[nodiscard]] auto worker_for(const std::span<const std::byte> key) const noexcept -> std::uint32_t {
        return static_cast<std::uint32_t>(hash_key_routing(key, routing_) % worker_count_);
    }

    [[nodiscard]] auto next_request_id() noexcept -> std::uint64_t {
        auto id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
        if (id == 0) {
            id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
        }
        return id;
    }

    [[nodiscard]] static auto rejected(Error error) -> MutationResult {
        if (error.mutation_outcome.empty()) {
            error.mutation_outcome = "rejected";
        }
        return {.outcome = MutationOutcome::rejected, .error = std::move(error)};
    }

    [[nodiscard]] static auto indeterminate(Error error) -> MutationResult {
        if (error.mutation_outcome.empty()) {
            error.mutation_outcome = "indeterminate";
        }
        return {.outcome = MutationOutcome::indeterminate, .error = std::move(error)};
    }

    ClientConfig config_;
    std::shared_ptr<server::TlsContext> tls_context_{};
    std::string tls_server_name_{};
    std::vector<std::unique_ptr<WorkerConnection>> connections_;
    std::uint32_t worker_count_{};
    std::uint64_t routing_epoch_{};
    WorkerRoutingState routing_{};
    std::atomic<std::uint64_t> next_request_id_{1};
    std::atomic<bool> healthy_{true};
};

Client::Client(std::unique_ptr<Impl> implementation) noexcept : implementation_(std::move(implementation)) {}

auto Client::connect(ClientConfig config) -> Result<Client> {
    try {
        auto implementation = std::make_unique<Impl>(std::move(config));
        if (auto initialized = implementation->initialize(); !initialized) {
            return unexpected(initialized.error());
        }
        return Client{std::move(implementation)};
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "client allocation failed");
    } catch (const std::exception& exception) {
        return fail(ErrorCode::internal_error,
                    std::string{"client initialization failed: "} + exception.what());
    }
}

Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
auto Client::operator=(Client&&) noexcept -> Client& = default;

auto Client::get(const std::span<const std::byte> key, const RequestOptions options)
    -> Result<std::vector<std::byte>> {
    try {
        if (!implementation_) {
            return fail(ErrorCode::unavailable, "client was moved from");
        }
        return implementation_->get(key, options);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "client allocation failed");
    }
}

auto Client::get(const std::string_view key, const RequestOptions options) -> Result<std::vector<std::byte>> {
    return get(as_bytes(key), options);
}

auto Client::ping(const std::span<const std::byte> payload, const RequestOptions options)
    -> Result<std::vector<std::byte>> {
    try {
        if (!implementation_) {
            return fail(ErrorCode::unavailable, "client was moved from");
        }
        return implementation_->ping(payload, options);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "client allocation failed");
    }
}

auto Client::health(const RequestOptions options) -> Result<std::vector<std::byte>> {
    try {
        if (!implementation_) {
            return fail(ErrorCode::unavailable, "client was moved from");
        }
        return implementation_->health(options);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "client allocation failed");
    }
}

auto Client::ready(const RequestOptions options) -> Result<std::vector<std::byte>> {
    try {
        if (!implementation_) {
            return fail(ErrorCode::unavailable, "client was moved from");
        }
        return implementation_->ready(options);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "client allocation failed");
    }
}

auto Client::stats(const RequestOptions options) -> Result<std::vector<std::byte>> {
    try {
        if (!implementation_) {
            return fail(ErrorCode::unavailable, "client was moved from");
        }
        return implementation_->stats(options);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "client allocation failed");
    }
}

auto Client::backup(const std::string_view destination, const RequestOptions options)
    -> Result<std::vector<std::byte>> {
    try {
        if (!implementation_) {
            return fail(ErrorCode::unavailable, "client was moved from");
        }
        return implementation_->backup(destination, options);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "client allocation failed");
    }
}

auto Client::put(const std::span<const std::byte> key, const std::span<const std::byte> value,
                 const PutOptions put_options, const RequestOptions options) -> MutationResult {
    try {
        if (!implementation_) {
            return {.outcome = MutationOutcome::rejected,
                    .error = Error{.code = ErrorCode::unavailable,
                                   .message = "client was moved from",
                                   .mutation_outcome = "rejected"}};
        }
        return implementation_->mutate(server::RequestOpcode::put, key, value, put_options.expire_at_ns,
                                       options);
    } catch (const std::bad_alloc&) {
        return {.outcome = MutationOutcome::rejected,
                .error = Error{.code = ErrorCode::resource_exhausted,
                               .message = "client allocation failed",
                               .mutation_outcome = "rejected"}};
    }
}

auto Client::put(const std::string_view key, const std::string_view value, const PutOptions put_options,
                 const RequestOptions options) -> MutationResult {
    return put(as_bytes(key), as_bytes(value), put_options, options);
}

auto Client::erase(const std::span<const std::byte> key, const RequestOptions options) -> MutationResult {
    try {
        if (!implementation_) {
            return {.outcome = MutationOutcome::rejected,
                    .error = Error{.code = ErrorCode::unavailable,
                                   .message = "client was moved from",
                                   .mutation_outcome = "rejected"}};
        }
        return implementation_->mutate(server::RequestOpcode::erase, key, {}, 0, options);
    } catch (const std::bad_alloc&) {
        return {.outcome = MutationOutcome::rejected,
                .error = Error{.code = ErrorCode::resource_exhausted,
                               .message = "client allocation failed",
                               .mutation_outcome = "rejected"}};
    }
}

auto Client::erase(const std::string_view key, const RequestOptions options) -> MutationResult {
    return erase(as_bytes(key), options);
}

auto Client::execute_pipeline(const std::span<const PipelineRequest> requests, const RequestOptions options)
    -> Result<std::vector<PipelineResponse>> {
    try {
        if (!implementation_) {
            return fail(ErrorCode::unavailable, "client was moved from");
        }
        return implementation_->execute_pipeline(requests, options);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "pipeline allocation failed before completion");
    }
}

auto Client::execute_batch(const std::span<const PipelineRequest> requests, const RequestOptions options)
    -> Result<std::vector<PipelineResponse>> {
    try {
        if (!implementation_) {
            return fail(ErrorCode::unavailable, "client was moved from");
        }
        return implementation_->execute_batch(requests, options);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "batch allocation failed before completion");
    }
}

auto Client::execute_worker_pipelines(const std::span<const std::vector<PipelineRequest>> batches,
                                      const RequestOptions options)
    -> Result<std::vector<std::vector<PipelineResponse>>> {
    try {
        if (!implementation_) {
            return fail(ErrorCode::unavailable, "client was moved from");
        }
        return implementation_->execute_worker_pipelines(batches, options);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "worker pipeline allocation failed before completion");
    }
}

auto Client::worker_for(const std::span<const std::byte> key) const noexcept -> std::uint32_t {
    return implementation_ ? implementation_->worker_for_key(key) : 0;
}

auto Client::worker_for(const std::string_view key) const noexcept -> std::uint32_t {
    return worker_for(as_bytes(key));
}

auto Client::worker_count() const noexcept -> std::uint32_t {
    return implementation_ ? implementation_->worker_count() : 0;
}

auto Client::routing_epoch() const noexcept -> std::uint64_t {
    return implementation_ ? implementation_->routing_epoch() : 0;
}

auto Client::routing() const noexcept -> WorkerRoutingState {
    return implementation_ ? implementation_->routing() : WorkerRoutingState{};
}

auto Client::healthy() const noexcept -> bool {
    return implementation_ && implementation_->healthy();
}

void Client::close() noexcept {
    if (implementation_) {
        implementation_->close();
    }
}

} // namespace glifistore::client
