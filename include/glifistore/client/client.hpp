/**
 * @file
 * @brief Synchronous wire-v2 client, bounded pipelines, and explicit mutation outcomes.
 */
#pragma once

#include "glifistore/core/error.hpp"
#include "glifistore/core/worker_routing.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace glifistore::client {

/// Opt-in TLS (ADR 0020). Cleartext remains the default. When enable is true the
/// client fails closed (no cleartext fallback). Hostname/SNI verification is on
/// unless insecure_skip_verify is set (lab escape only).
/**
 * @brief Opt-in TLS settings; handshake/verification failure never falls back to cleartext.
 */
struct TlsOptions {
    bool enable{false};      ///< Explicit TLS opt-in.
    std::string ca_file{};   ///< CA bundle override; empty uses the TLS backend defaults.
    std::string cert_file{}; ///< Optional client certificate for mutual TLS.
    std::string key_file{};  ///< Private key paired with the client certificate.
    /// Empty uses ClientConfig::host for SNI / hostname verification.
    std::string server_name{};        ///< SNI/hostname override; empty uses ClientConfig::host.
    bool insecure_skip_verify{false}; ///< Lab-only escape from peer certificate verification.
};

/**
 * @brief Transport, deadline, frame-size, and pipeline admission bounds.
 */
struct ClientConfig {
    std::string host{"127.0.0.1"}; ///< TCP host used unless a Unix-socket path is configured.
    std::uint16_t port{7379};      ///< TCP service port.
    /// When non-empty, dial AF_UNIX at this path instead of TCP host:port (ADR 0029).
    /// Mutually exclusive with TLS.
    std::string unix_socket_path{};          ///< Unix-socket path; mutually exclusive with TLS.
    std::uint32_t connect_timeout_ms{3'000}; ///< Connection/bootstrap budget in milliseconds.
    std::uint32_t request_timeout_ms{5'000}; ///< Default request budget in milliseconds.
    std::size_t maximum_frame_bytes{std::size_t{2} * 1024U *
                                    1024U};     ///< Maximum accepted frame size in bytes.
    std::size_t maximum_pipeline_requests{256}; ///< Maximum requests admitted in one owner pipeline.
    std::size_t maximum_pipeline_bytes{std::size_t{1024} *
                                       1024U}; ///< Maximum encoded request bytes in one owner pipeline.
    TlsOptions tls{};
};

/**
 * @brief Expiration of one wire PUT, expressed as an absolute timestamp.
 */
struct PutOptions {
    /// Absolute Unix time in nanoseconds. Zero means no expiry.
    std::uint64_t expire_at_ns{}; ///< Absolute Unix nanoseconds; zero means no expiration.
};

/// Optional per-call request budget. When timeout is unset, ClientConfig::request_timeout_ms
/// applies. Connect and bootstrap INIT/BIND always use the configured defaults.
/**
 * @brief Optional per-call deadline budget; bootstrap uses configured defaults.
 */
struct RequestOptions {
    /// Override for this call; unset uses request_timeout_ms.
    std::optional<std::chrono::milliseconds> timeout{};
};

/// Observable outcome of a client mutation; an indeterminate operation may have committed.
enum class MutationOutcome {
    committed,
    rejected,
    indeterminate,
};

/**
 * @brief Explicit mutation knowledge plus an optional owning error.
 */
struct MutationResult {
    MutationOutcome outcome{MutationOutcome::rejected}; ///< Known commit/rejection/ambiguity of the mutation.
    std::optional<Error> error{};                       ///< Owning failure context when available.

    [[nodiscard]] auto committed() const noexcept -> bool {
        return outcome == MutationOutcome::committed;
    }
};

/**
 * @brief Wire operations admitted by the bounded client pipeline API.
 */
enum class PipelineOpcode {
    get,
    put,
    erase,
};

/**
 * @brief Borrowed request data consumed before the enclosing client call returns.
 */
struct PipelineRequest {
    PipelineOpcode opcode{PipelineOpcode::get}; ///< Operation to encode.
    std::span<const std::byte> key{};           ///< Binary key borrowed for the client call.
    std::span<const std::byte> value{};         ///< PUT payload borrowed for the client call.
    PutOptions put_options{};
};

/**
 * @brief Per-position pipeline result.
 * @note indeterminate means a sent mutation may have committed; callers must not
 * infer rejection or automatically retry solely from a transport error.
 */
enum class PipelineOutcome {
    succeeded,
    failed,
    indeterminate,
};

/**
 * @brief Owning positional response, including partial transport-failure outcomes.
 */
struct PipelineResponse {
    PipelineOutcome outcome{PipelineOutcome::failed}; ///< Outcome at this request position.
    std::vector<std::byte> value{};                   ///< Owning response payload, if one was returned.
    std::optional<Error> error{};                     ///< Owning failure context when available.

    [[nodiscard]] auto succeeded() const noexcept -> bool {
        return outcome == PipelineOutcome::succeeded;
    }
};

/// Portable error-taxonomy-v1 helpers (docs/spec/error-taxonomy-v1.md). Used by Client
/// and cross-SDK conformance tests; not a general application API beyond category mapping.
[[nodiscard]] auto error_from_wire_status(std::uint16_t wire_status) -> Error;
[[nodiscard]] auto portable_retryability(std::string_view category, bool mutation_sent, bool indeterminate)
    -> std::string;
/// Standalone PUT/ERASE outcome implied by a decoded wire status (fixture matrix).
[[nodiscard]] auto portable_mutation_outcome(std::uint16_t wire_status) -> std::string;

/// A synchronous, thread-safe TCP client for wire protocol v2. It keeps one
/// connection bound to each server worker, preserving per-key routing while
/// allowing independent workers to make progress concurrently.
/// @see ClientConfig, MutationResult
class Client final {
  public:
    /// Connect and bootstrap owner-bound connections using the configured transport.
    [[nodiscard]] static auto connect(ClientConfig config = {}) -> Result<Client>;
    ~Client();

    Client(Client&&) noexcept;
    auto operator=(Client&&) noexcept -> Client&;
    Client(const Client&) = delete;
    auto operator=(const Client&) -> Client& = delete;

    /**
     * @brief Read an owning copy of the latest visible value on its owner connection.
     * @param key Binary key borrowed until return.
     * @param options Request deadline override.
     * @return Owned bytes, not_found for absence/expiration, or a transport/server error.
     */
    [[nodiscard]] auto get(std::span<const std::byte> key, RequestOptions options = {})
        -> Result<std::vector<std::byte>>;
    /**
     * @brief Binary-safe string-view overload of get(); the result owns its bytes.
     */
    [[nodiscard]] auto get(std::string_view key, RequestOptions options = {})
        -> Result<std::vector<std::byte>>;
    /**
     * @brief Send PING on Worker 0 and return its owning echo payload.
     * @param payload Bytes borrowed until return.
     * @param options Request deadline override.
     * @return Echoed bytes, or a transport/server error.
     */
    [[nodiscard]] auto ping(std::span<const std::byte> payload = {}, RequestOptions options = {})
        -> Result<std::vector<std::byte>>;
    /// Lifecycle probes (Worker 0). On OK, health/ready return wire ASCII
    /// GlifiStore/live and GlifiStore/ready; stats returns the STATS text body.
    /// Distinct from healthy(), which is local connection fail-closed state.
    /**
     * @brief Query the remote liveness probe on Worker 0; success returns GlifiStore/live.
     * @note This sends a request; healthy() observes local connection state only.
     */
    [[nodiscard]] auto health(RequestOptions options = {}) -> Result<std::vector<std::byte>>;
    /**
     * @brief Query remote readiness on Worker 0; success returns GlifiStore/ready.
     * @note This sends a request; healthy() observes local connection state only.
     */
    [[nodiscard]] auto ready(RequestOptions options = {}) -> Result<std::vector<std::byte>>;
    /**
     * @brief Query the remote STATS text body on Worker 0.
     * @note This sends a request; healthy() observes local connection state only.
     */
    [[nodiscard]] auto stats(RequestOptions options = {}) -> Result<std::vector<std::byte>>;
    /// Online fenced durable backup (wire BACKUP). Destination path is UTF-8; must be empty.
    /// Not zero-impact hot I/O — briefly fences Store admissions. Admin-gated under secure authz.
    /**
     * @brief Request the admin-gated, admission-fenced durable backup operation.
     * @param destination UTF-8 path naming an empty destination directory.
     * @param options Deadline for this client call.
     * @return Owning server report text, or a transport/authorization/storage error.
     * @note A client timeout does not undo server-side file work already started.
     */
    [[nodiscard]] auto backup(std::string_view destination, RequestOptions options = {})
        -> Result<std::vector<std::byte>>;

    /**
     * @brief Send a PUT and classify what is known about its commit.
     * @param key Binary key borrowed until return.
     * @param value Value bytes borrowed until return.
     * @param put_options Absolute expiration; zero disables expiration.
     * @param options Deadline for the whole operation.
     * @return Committed, rejected, or indeterminate knowledge with optional error context.
     * @note A sent mutation that loses its response can be indeterminate. There is no automatic retry.
     */
    [[nodiscard]] auto put(std::span<const std::byte> key, std::span<const std::byte> value,
                           PutOptions put_options = {}, RequestOptions options = {}) -> MutationResult;
    /**
     * @brief Binary-safe string-view overload of put() with the same outcome classification.
     */
    [[nodiscard]] auto put(std::string_view key, std::string_view value, PutOptions put_options = {},
                           RequestOptions options = {}) -> MutationResult;
    /**
     * @brief Send ERASE and preserve commit ambiguity on transport failure.
     * @return Explicit mutation knowledge; an error does not by itself prove rollback.
     * @note key is borrowed until return; options applies to the whole call.
     */
    [[nodiscard]] auto erase(std::span<const std::byte> key, RequestOptions options = {}) -> MutationResult;
    /**
     * @brief Binary-safe string-view overload of erase() with the same outcome classification.
     */
    [[nodiscard]] auto erase(std::string_view key, RequestOptions options = {}) -> MutationResult;

    /// Executes an ordered pipeline on one Worker-bound connection. Every key
    /// must route to the same Worker. Responses always correspond positionally
    /// to requests, including partial transport failures.
    /**
     * @brief Execute a bounded ordered sequence on a single owner connection.
     * @param requests Borrowed keys/values; every key must route to the same Worker.
     * @param options Shared deadline for this pipeline.
     * @return Positional owning responses, or an error preventing pipeline execution.
     * @note Per-position ambiguity survives partial failure. No transaction or rollback is provided.
     */
    [[nodiscard]] auto execute_pipeline(std::span<const PipelineRequest> requests,
                                        RequestOptions options = {}) -> Result<std::vector<PipelineResponse>>;

    /// Groups requests by Worker, runs one pipeline per Worker (concurrently when
    /// more than one Worker is involved), and restores caller order. Not atomic:
    /// Workers succeed or fail independently after admission. One shared deadline
    /// covers the whole batch call.
    /**
     * @brief Partition requests by owner and restore their original response order.
     * @param requests Borrowed data for the duration of this call.
     * @param options One shared deadline across all owner pipelines.
     * @return Positional owning responses, or a batch-level admission/execution error.
     * @note Owners progress and fail independently; there is no cross-owner atomicity.
     */
    [[nodiscard]] auto execute_batch(std::span<const PipelineRequest> requests, RequestOptions options = {})
        -> Result<std::vector<PipelineResponse>>;

    /// Pre-sharded Worker vectors: `batches.size()` must equal `worker_count()`.
    /// Index `i` is the pipeline for Worker `i` (empty vectors are skipped and yield
    /// an empty response vector). Every key in `batches[i]` must route to Worker `i`.
    /// Same non-atomic fan-out and shared deadline as `execute_batch`.
    /**
     * @brief Execute prepartitioned owner pipelines with a shared call deadline.
     * @param batches Exactly worker_count() vectors; keys in vector i must route to i.
     * @param options One deadline shared by all nonempty vectors.
     * @return One positional response vector per Worker; empty inputs yield empty vectors.
     */
    [[nodiscard]] auto execute_worker_pipelines(std::span<const std::vector<PipelineRequest>> batches,
                                                RequestOptions options = {})
        -> Result<std::vector<std::vector<PipelineResponse>>>;

    /**
     * @brief Compute the owner from this connection set's immutable routing metadata.
     */
    [[nodiscard]] auto worker_for(std::span<const std::byte> key) const noexcept -> std::uint32_t;
    /**
     * @brief Binary-safe string overload of the owner-routing query.
     */
    [[nodiscard]] auto worker_for(std::string_view key) const noexcept -> std::uint32_t;
    /**
     * @brief Return the Worker count learned during bootstrap.
     */
    [[nodiscard]] auto worker_count() const noexcept -> std::uint32_t;
    /**
     * @brief Return the routing epoch learned during bootstrap.
     */
    [[nodiscard]] auto routing_epoch() const noexcept -> std::uint64_t;
    /**
     * @brief Copy the routing algorithm/seed learned during bootstrap.
     */
    [[nodiscard]] auto routing() const noexcept -> WorkerRoutingState;
    /**
     * @brief Observe local fail-closed connection state without probing the server.
     */
    [[nodiscard]] auto healthy() const noexcept -> bool;
    /**
     * @brief Close local owner connections; does not issue a remote server shutdown.
     */
    void close() noexcept;

  private:
    class Impl;
    explicit Client(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace glifistore::client
