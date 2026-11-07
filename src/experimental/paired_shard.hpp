#pragma once

#include "glifistore/core/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace glifistore::experimental {

// Lab-only volatile Reader/Writer microbench prototype (ADR 0032). Not installed,
// not selectable by glifistored, and not a second product runtime beside
// store::paired::ShardPairRuntime.

enum class PrototypeMutationKind : std::uint8_t { put, erase };
enum class PrototypeSubmitStatus : std::uint8_t {
    submitted,
    stopped,
    queue_full,
    key_too_large,
    value_too_large,
};

struct PrototypeCompletion final {
    std::uint64_t request_id{};
    std::optional<ErrorCode> error{};
    std::uint64_t visible_through{};
    std::uint64_t epoch{};
};

struct PrototypeRead final {
    std::span<const std::byte> value{};
    std::uint64_t sequence{};
};

struct PrototypeWriterBatchConfig final {
    std::size_t max_records{32};
    std::chrono::microseconds max_wait{2};
};

struct PrototypeCompletionNotifier final {
    void* context{};
    void (*notify)(void* context) noexcept {};
};

class PrototypeReadPin final {
  public:
    PrototypeReadPin() = default;
    ~PrototypeReadPin();
    PrototypeReadPin(PrototypeReadPin&& other) noexcept;
    auto operator=(PrototypeReadPin&& other) noexcept -> PrototypeReadPin&;
    PrototypeReadPin(const PrototypeReadPin&) = delete;
    auto operator=(const PrototypeReadPin&) -> PrototypeReadPin& = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }

    void reset() noexcept;

  private:
    using Release = void (*)(void*, std::uint32_t) noexcept;

    PrototypeReadPin(void* owner, const std::uint32_t slot, Release release) noexcept
        : owner_(owner), slot_(slot), release_(release) {}

    void* owner_{};
    std::uint32_t slot_{};
    Release release_{};

    friend class VolatileShardPairPrototype;
};

struct PrototypePairStats final {
    std::uint64_t mutation_pushes{};
    std::uint64_t mutation_pops{};
    std::uint64_t completion_pushes{};
    std::uint64_t completion_pops{};
    std::uint64_t queue_full{};
    std::size_t mutation_queue_depth{};
    std::size_t mutation_queue_high_watermark{};
    std::size_t completion_queue_depth{};
    std::size_t completion_queue_high_watermark{};
    std::size_t last_writer_batch_size{};
    std::size_t maximum_writer_batch_size{};
    std::uint64_t writer_batch_wait_ns{};
    std::uint64_t writer_batch_deadline_closes{};
    std::uint64_t reader_gets{};
    std::uint64_t publications{};
    std::uint64_t publication_records{};
    std::uint64_t publication_latency_ns{};
    std::uint64_t publication_bytes{};
    std::uint64_t ingress_value_bytes_copied{};
    std::uint64_t payload_allocations{};
    std::uint64_t payload_bytes_allocated{};
    std::uint64_t delta_directory_entries_copied{};
    std::uint64_t delta_page_view_entries_copied{};
    std::uint64_t delta_pages_copied{};
    std::uint64_t delta_pages_allocated{};
    std::uint64_t delta_merges{};
    std::uint64_t publication_backpressure{};
    std::uint64_t generation_slot_exhaustions{};
    std::uint64_t generation_slot_reuses{};
    std::size_t generation_live{};
    std::size_t generation_high_watermark{};
    std::uint64_t generation_retire_count{};
    std::uint64_t generation_retire_delay_ns{};
    std::size_t generation_output_pins{};
    std::size_t generation_output_pin_high_watermark{};
    std::uint64_t generation_retire_pin_blocks{};
    std::uint64_t reader_turns{};
    std::uint64_t reader_epoch{};
    std::uint64_t writer_epoch{};
    std::uint64_t visible_through{};
};

// Phase-2 volatile prototype. Exactly one caller thread owns the Reader API;
// one persistent internal thread owns all Writer state. It is intentionally
// not linked into Store or Reactor yet.
class VolatileShardPairPrototype final {
  public:
    static constexpr std::size_t kQueueCapacity = 256;
    static constexpr std::size_t kMaximumKeyBytes = 256;

    [[nodiscard]] static auto
    create(std::size_t maximum_value_bytes = std::size_t{256} * 1024U, std::size_t merge_delta_entries = 128,
           PrototypeWriterBatchConfig batch_config = {}, PrototypeCompletionNotifier completion_notifier = {})
        -> Result<std::unique_ptr<VolatileShardPairPrototype>>;
    ~VolatileShardPairPrototype();

    VolatileShardPairPrototype(const VolatileShardPairPrototype&) = delete;
    auto operator=(const VolatileShardPairPrototype&) -> VolatileShardPairPrototype& = delete;

    [[nodiscard]] auto try_submit_put(std::uint64_t request_id, std::string_view key,
                                      std::span<const std::byte> value,
                                      std::uint64_t expire_at_ns = 0) noexcept -> PrototypeSubmitStatus;
    [[nodiscard]] auto try_submit_erase(std::uint64_t request_id, std::string_view key) noexcept
        -> PrototypeSubmitStatus;
    [[nodiscard]] auto try_pop_completion() noexcept -> std::optional<PrototypeCompletion>;

    // Called once per Reactor/event-loop turn, never once per GET. Any span
    // returned by get() is valid only until the next call on this Reader.
    void adopt_publication() noexcept;
    [[nodiscard]] auto get(std::string_view key, std::uint64_t now_ns = 0) noexcept
        -> std::optional<PrototypeRead>;
    // Slow-output path only. The pin keeps the currently adopted generation
    // alive across later Reader turns until scatter/gather output completes.
    [[nodiscard]] auto pin_read_generation() noexcept -> PrototypeReadPin;

    void stop_and_drain() noexcept;
    [[nodiscard]] auto stats() const noexcept -> PrototypePairStats;

  private:
    struct Impl;
    explicit VolatileShardPairPrototype(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace glifistore::experimental
