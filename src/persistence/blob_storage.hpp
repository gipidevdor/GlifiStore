#pragma once

#include "glifistore/persistence/blob.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/store/blob.hpp"
#include "glifistore/store/value.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace glifistore::detail {
struct BlobTransfer;

/**
 * @brief Shared blob namespace, transfer budgets, roots, and bounded I/O ownership.
 *
 * The Store binds callbacks for serialized reference publication and identity
 * allocation. Transfers retain shared state, while close() invalidates further I/O.
 * A reference snapshot holds a shared roots lock until it has pinned the selected
 * file; collection holds the exclusive lock while determining reclaimability.
 */
class BlobStorage final : public std::enable_shared_from_this<BlobStorage> {
  public:
    using Commit =
        std::function<Status(std::string_view, std::span<const std::byte>, std::uint64_t, ValueType)>;
    using Allocate = std::function<Result<BlobId>()>;
    [[nodiscard]] static auto open(DataDirectory& directory, const Manifest& manifest,
                                   BlobResourceLimits limits, std::uint64_t reserved_free_bytes)
        -> Result<std::shared_ptr<BlobStorage>>;
    BlobStorage();
    ~BlobStorage();
    BlobStorage(const BlobStorage&) = delete;
    auto operator=(const BlobStorage&) -> BlobStorage& = delete;
    void bind(Commit commit, Allocate allocate, std::function<void()> fail_closed);
    [[nodiscard]] auto begin(std::string_view key, BlobOptions options, std::size_t shard)
        -> Result<std::unique_ptr<BlobWriter>>;
    [[nodiscard]] auto reader(OwnedValue value, std::size_t shard) -> Result<std::unique_ptr<BlobReader>>;
    [[nodiscard]] auto verify(const BlobRef& reference, WorkerId owner) -> Status;
    [[nodiscard]] auto collect(std::span<const BlobId> roots) -> Result<BlobGcResult>;
    /**
     * @brief Hold roots stable while selecting a visible reference and acquiring its transfer pin.
     */
    [[nodiscard]] auto snapshot_lock() -> std::shared_lock<std::shared_mutex>;
    /**
     * @brief Exclude snapshot/transfer root changes while collecting unreferenced files.
     */
    [[nodiscard]] auto collection_lock() -> std::unique_lock<std::shared_mutex>;
    void close() noexcept;
    [[nodiscard]] auto healthy() const noexcept -> bool;
    struct State;

  private:
    std::shared_ptr<State> state_;
    friend struct BlobTransfer;
};

[[nodiscard]] auto sync_blob_parent(const DataDirectory& directory) -> Status;
[[nodiscard]] auto blob_filename(const BlobId& id, bool temporary = false) -> std::string;
/**
 * @brief Verify every blob descriptor in authoritative committed Segment history.
 * @note Historical descriptors remain roots until compaction removes their containing
 * Records; checking only the current logical Index would miss recovery dependencies.
 * @return Distinct verified blob identities or a fail-closed integrity/resource error.
 */
[[nodiscard]] auto verify_blob_closure(DataDirectory& directory, const Manifest& manifest,
                                       BlobStorage& storage) -> Result<std::vector<BlobId>>;
/**
 * @brief Copy and synchronize the verified historical blob closure, then verify destination agreement.
 */
[[nodiscard]] auto copy_blob_closure(DataDirectory& source, DataDirectory& destination,
                                     const Manifest& manifest, BlobResourceLimits limits,
                                     std::size_t& files_copied, std::uint64_t& bytes_copied) -> Status;
} // namespace glifistore::detail
