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

// Opt-in TLS (ADR 0020). Cleartext remains the default. When enable is true the
// client fails closed (no cleartext fallback). Hostname/SNI verification is on
// unless insecure_skip_verify is set (lab escape only).
struct TlsOptions {
    bool enable{false};
    std::string ca_file{};
    std::string cert_file{};
    std::string key_file{};
    // Empty uses ClientConfig::host for SNI / hostname verification.
    std::string server_name{};
    bool insecure_skip_verify{false};
};

struct ClientConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{7379};
    // When non-empty, dial AF_UNIX at this path instead of TCP host:port (ADR 0029).
    // Mutually exclusive with TLS.
    std::string unix_socket_path{};
    std::uint32_t connect_timeout_ms{3'000};
    std::uint32_t request_timeout_ms{5'000};
    std::size_t maximum_frame_bytes{std::size_t{2} * 1024U * 1024U};
    std::size_t maximum_pipeline_requests{256};
    std::size_t maximum_pipeline_bytes{std::size_t{1024} * 1024U};
    TlsOptions tls{};
};

struct PutOptions {
    // Absolute Unix time in nanoseconds. Zero means no expiry.
    std::uint64_t expire_at_ns{};
};

// Optional per-call request budget. When timeout is unset, ClientConfig::request_timeout_ms
// applies. Connect and bootstrap INIT/BIND always use the configured defaults.
struct RequestOptions {
    std::optional<std::chrono::milliseconds> timeout{};
};

/// Observable outcome of a client mutation; an indeterminate operation may have committed.
enum class MutationOutcome {
    committed,
    rejected,
    indeterminate,
};

struct MutationResult {
    MutationOutcome outcome{MutationOutcome::rejected};
    std::optional<Error> error{};

    [[nodiscard]] auto committed() const noexcept -> bool {
        return outcome == MutationOutcome::committed;
    }
};

enum class PipelineOpcode {
    get,
    put,
    erase,
};

struct PipelineRequest {
    PipelineOpcode opcode{PipelineOpcode::get};
    std::span<const std::byte> key{};
    std::span<const std::byte> value{};
    PutOptions put_options{};
};

enum class PipelineOutcome {
    succeeded,
    failed,
    indeterminate,
};

struct PipelineResponse {
    PipelineOutcome outcome{PipelineOutcome::failed};
    std::vector<std::byte> value{};
    std::optional<Error> error{};

    [[nodiscard]] auto succeeded() const noexcept -> bool {
        return outcome == PipelineOutcome::succeeded;
    }
};

// Portable error-taxonomy-v1 helpers (docs/spec/error-taxonomy-v1.md). Used by Client
// and cross-SDK conformance tests; not a general application API beyond category mapping.
[[nodiscard]] auto error_from_wire_status(std::uint16_t wire_status) -> Error;
[[nodiscard]] auto portable_retryability(std::string_view category, bool mutation_sent, bool indeterminate)
    -> std::string;
// Standalone PUT/ERASE outcome implied by a decoded wire status (fixture matrix).
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

    [[nodiscard]] auto get(std::span<const std::byte> key, RequestOptions options = {})
        -> Result<std::vector<std::byte>>;
    [[nodiscard]] auto get(std::string_view key, RequestOptions options = {})
        -> Result<std::vector<std::byte>>;
    [[nodiscard]] auto ping(std::span<const std::byte> payload = {}, RequestOptions options = {})
        -> Result<std::vector<std::byte>>;
    // Lifecycle probes (Worker 0). On OK, health/ready return wire ASCII
    // GlifiStore/live and GlifiStore/ready; stats returns the STATS text body.
    // Distinct from healthy(), which is local connection fail-closed state.
    [[nodiscard]] auto health(RequestOptions options = {}) -> Result<std::vector<std::byte>>;
    [[nodiscard]] auto ready(RequestOptions options = {}) -> Result<std::vector<std::byte>>;
    [[nodiscard]] auto stats(RequestOptions options = {}) -> Result<std::vector<std::byte>>;
    // Online fenced durable backup (wire BACKUP). Destination path is UTF-8; must be empty.
    // Not zero-impact hot I/O — briefly fences Store admissions. Admin-gated under secure authz.
    [[nodiscard]] auto backup(std::string_view destination, RequestOptions options = {})
        -> Result<std::vector<std::byte>>;

    [[nodiscard]] auto put(std::span<const std::byte> key, std::span<const std::byte> value,
                           PutOptions put_options = {}, RequestOptions options = {}) -> MutationResult;
    [[nodiscard]] auto put(std::string_view key, std::string_view value, PutOptions put_options = {},
                           RequestOptions options = {}) -> MutationResult;
    [[nodiscard]] auto erase(std::span<const std::byte> key, RequestOptions options = {}) -> MutationResult;
    [[nodiscard]] auto erase(std::string_view key, RequestOptions options = {}) -> MutationResult;

    // Executes an ordered pipeline on one Worker-bound connection. Every key
    // must route to the same Worker. Responses always correspond positionally
    // to requests, including partial transport failures.
    [[nodiscard]] auto execute_pipeline(std::span<const PipelineRequest> requests,
                                        RequestOptions options = {}) -> Result<std::vector<PipelineResponse>>;

    // Groups requests by Worker, runs one pipeline per Worker (concurrently when
    // more than one Worker is involved), and restores caller order. Not atomic:
    // Workers succeed or fail independently after admission. One shared deadline
    // covers the whole batch call.
    [[nodiscard]] auto execute_batch(std::span<const PipelineRequest> requests, RequestOptions options = {})
        -> Result<std::vector<PipelineResponse>>;

    // Pre-sharded Worker vectors: `batches.size()` must equal `worker_count()`.
    // Index `i` is the pipeline for Worker `i` (empty vectors are skipped and yield
    // an empty response vector). Every key in `batches[i]` must route to Worker `i`.
    // Same non-atomic fan-out and shared deadline as `execute_batch`.
    [[nodiscard]] auto execute_worker_pipelines(std::span<const std::vector<PipelineRequest>> batches,
                                                RequestOptions options = {})
        -> Result<std::vector<std::vector<PipelineResponse>>>;

    [[nodiscard]] auto worker_for(std::span<const std::byte> key) const noexcept -> std::uint32_t;
    [[nodiscard]] auto worker_for(std::string_view key) const noexcept -> std::uint32_t;
    [[nodiscard]] auto worker_count() const noexcept -> std::uint32_t;
    [[nodiscard]] auto routing_epoch() const noexcept -> std::uint64_t;
    [[nodiscard]] auto routing() const noexcept -> WorkerRoutingState;
    [[nodiscard]] auto healthy() const noexcept -> bool;
    void close() noexcept;

  private:
    class Impl;
    explicit Client(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace glifistore::client
