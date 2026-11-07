/**
 * @file
 * @brief Bounded streaming blob handles for the opt-in durable embedded profile.
 */
#pragma once

#include "glifistore/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace glifistore {
namespace detail {
struct BlobTransfer;
}

/**
 * @brief Expiration and optional exact byte length of one staged upload.
 */
struct BlobOptions {
    std::uint64_t expire_at_ns{}; ///< Absolute Unix nanoseconds; zero disables expiration.
    /// Exact size required at commit; unset still obeys the maximum value size.
    std::optional<std::uint64_t> expected_size{};
};

/**
 * @brief Independent byte/file/transfer admission budgets for the blob profile.
 */
struct BlobResourceLimits {
    std::uint64_t max_bytes{8ULL * 1024 * 1024 * 1024}; ///< Maximum blob storage byte budget.
    std::size_t max_files{4096};                        ///< Maximum admitted blob-file count.
    std::size_t max_transfers{8};                       ///< Store-wide active reader/writer transfer limit.
    std::size_t max_transfers_per_shard{2};             ///< Per-owner active reader/writer transfer limit.
    auto operator==(const BlobResourceLimits&) const -> bool = default;
};

/**
 * @brief Owning sequential upload; only commit() publishes key visibility.
 *
 * Destroying an unfinished writer invokes abort(). Transfer methods serialize on
 * shared state; the caller must still ensure the handle outlives every call.
 * Store shutdown causes subsequent transfer I/O to fail.
 */
class BlobWriter final {
  public:
    /**
     * @brief Adopt implementation-created transfer state.
     * @pre state is a valid upload state produced by the Store blob subsystem.
     * @note Applications obtain writers through Store::begin_blob_put().
     */
    explicit BlobWriter(std::shared_ptr<detail::BlobTransfer> state) noexcept;
    /**
     * @brief Abort any unfinished upload and release its transfer reservation.
     */
    ~BlobWriter();
    BlobWriter(const BlobWriter&) = delete;
    auto operator=(const BlobWriter&) -> BlobWriter& = delete;
    /**
     * @brief Append bytes to the staged value without retaining the input view.
     * @param bytes Next sequential bytes; chunking and resource limits are enforced internally.
     * @return Success or a validation, capacity, availability, or I/O error.
     * @note A failed append may terminate the upload; write() after termination is invalid_argument.
     */
    [[nodiscard]] auto write(std::span<const std::byte> bytes) -> Status;
    /**
     * @brief Finalize integrity metadata and publish the value through the Store Writer.
     * @return The stored terminal outcome on repeated calls; expected_size mismatch is invalid_argument.
     * @note Small staged values can be published inline. Failure does not imply that a
     * mutation was definitely uncommitted; unavailable may represent an indeterminate outcome.
     */
    [[nodiscard]] auto commit() -> Status;
    /**
     * @brief Best-effort, idempotent cleanup of an unfinished upload.
     * @note Does not roll back a value already installed by commit(); cleanup errors cannot be returned.
     */
    void abort() noexcept;

  private:
    std::shared_ptr<detail::BlobTransfer> state_;
};

/**
 * @brief Owning sequential read of one pinned visible value.
 *
 * The selected inline bytes or blob root stay pinned for this handle. Replacing or
 * erasing the key does not switch the stream to a newer value. Integrity is checked
 * as blob chunks are consumed; full digest verification requires reaching the last chunk.
 */
class BlobReader final {
  public:
    /**
     * @brief Adopt implementation-created transfer state.
     * @pre state is a valid read state produced by the Store blob subsystem.
     * @note Applications obtain readers through Store::open_blob().
     */
    explicit BlobReader(std::shared_ptr<detail::BlobTransfer> state) noexcept;
    /**
     * @brief Release the read pin and transfer reservation.
     */
    ~BlobReader();
    BlobReader(const BlobReader&) = delete;
    auto operator=(const BlobReader&) -> BlobReader& = delete;
    /**
     * @brief Copy the next sequential bytes, possibly fewer than the buffer capacity.
     * @param[out] bytes Caller-owned destination; must be nonempty before EOF.
     * @return Bytes copied, zero at EOF, or an availability/I/O/integrity error.
     * @note Integrity failure is sticky and fails blob storage closed. The buffer is never retained.
     */
    [[nodiscard]] auto read(std::span<std::byte> bytes) -> Result<std::size_t>;
    /**
     * @brief Return the selected value length in bytes without advancing the stream.
     */
    [[nodiscard]] auto size() const noexcept -> std::uint64_t;

  private:
    std::shared_ptr<detail::BlobTransfer> state_;
};

/**
 * @brief Observed file retention and reclamation from one explicit blob collection.
 */
struct BlobGcResult {
    std::size_t files_removed{};   ///< Unreferenced files successfully removed.
    std::uint64_t bytes_removed{}; ///< Physical bytes reclaimed from removed files.
    std::size_t files_retained{};  ///< Files retained because they are still rooted.
};
} // namespace glifistore
