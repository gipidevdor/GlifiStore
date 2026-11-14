#pragma once

#include "glifistore/core/little_endian.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/store/store.hpp"
#include "test.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace glifistore::test::server_reactor_support {

inline constexpr std::size_t kTestMutationArenaBytes = 1U * 1024U * 1024U;

[[nodiscard]] inline auto
open_paired_store_for_writer(std::size_t worker_count, std::size_t async_capacity,
                             std::size_t async_payload_bytes = kTestMutationArenaBytes,
                             glifistore::PairedConcurrencyConfig paired = {})
    -> glifistore::Result<std::unique_ptr<glifistore::Store>> {
    paired.async_lane_capacity = async_capacity;
    paired.async_lane_payload_bytes = async_payload_bytes;
    paired.reader_epoch_lease = true;
    if (paired.merge_delta_entries == 0) {
        paired.merge_delta_entries = glifistore::kDefaultPairedMergeDeltaEntries;
    }
    if (paired.merge_maximum_post_entries == 0) {
        paired.merge_maximum_post_entries = glifistore::kDefaultPairedMergeMaximumPostEntries;
    }
    if (paired.merge_quantum_slots == 0) {
        paired.merge_quantum_slots = glifistore::kDefaultPairedMergeQuantumSlots;
    }
    return glifistore::Store::open({
        .worker_config = {.explicit_count = worker_count},
        .concurrency = glifistore::StoreConcurrencyMode::paired,
        .paired = paired,
    });
}

[[nodiscard]] inline auto bytes(const std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

[[nodiscard]] inline auto text(const std::span<const std::byte> value) -> std::string_view {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] inline auto load_u32(const std::span<const std::byte> input) -> std::uint32_t {
    return glifistore::le::get_u32(input, 0);
}

[[nodiscard]] inline auto send_all(const int socket, const std::span<const std::byte> data) -> bool {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto written = ::send(socket, data.data() + sent, data.size() - sent, 0);
        if (written <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

[[nodiscard]] inline auto receive_exact(const int socket, const std::span<std::byte> output) -> bool {
    std::size_t received = 0;
    while (received < output.size()) {
        const auto count = ::recv(socket, output.data() + received, output.size() - received, 0);
        if (count <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(count);
    }
    return true;
}

[[nodiscard]] inline auto receive_response(const int socket) -> std::vector<std::byte> {
    std::array<std::byte, glifistore::server::kResponseHeaderBytes> header{};
    if (!receive_exact(socket, header)) {
        return {};
    }
    const auto frame_size = static_cast<std::size_t>(load_u32(header));
    if (frame_size < header.size() || frame_size > glifistore::server::kMaxFrameBytes) {
        return {};
    }
    std::vector<std::byte> frame(frame_size);
    std::ranges::copy(header, frame.begin());
    if (!receive_exact(socket, std::span<std::byte>{frame}.subspan(header.size()))) {
        return {};
    }
    return frame;
}

[[nodiscard]] inline auto connect_to(const std::uint16_t port) -> int {
    const auto socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0) {
        return -1;
    }
    const auto close_with_error = [socket] {
        static_cast<void>(::close(socket));
        return -1;
    };
    const auto original_flags = ::fcntl(socket, F_GETFL, 0);
    if (original_flags < 0 || ::fcntl(socket, F_SETFL, original_flags | O_NONBLOCK) != 0) {
        return close_with_error();
    }

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr));
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0 &&
        errno != EINPROGRESS) {
        return close_with_error();
    }

    pollfd descriptor{.fd = socket, .events = POLLOUT, .revents = 0};
    const int poll_result = ::poll(&descriptor, 1, 2'000);
    if (poll_result <= 0) {
        return close_with_error();
    }

    int socket_error{};
    socklen_t socket_error_size = sizeof(socket_error);
    if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) != 0 ||
        socket_error != 0 || ::fcntl(socket, F_SETFL, original_flags) != 0) {
        return close_with_error();
    }

    timeval timeout{.tv_sec = 2, .tv_usec = 0};
    if (::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        return close_with_error();
    }
    return socket;
}

[[nodiscard]] inline auto initialize_and_bind(const int socket, const std::uint32_t worker,
                                              const std::uint32_t worker_count) -> bool {
    const auto init = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::init,
        .request_id = 1,
    });
    if (!init || !send_all(socket, *init)) {
        return false;
    }
    const auto init_frame = receive_response(socket);
    const auto initialized = glifistore::server::decode_response(init_frame);
    if (!initialized || initialized->frame.status != glifistore::server::ResponseStatus::ok ||
        initialized->frame.worker_count != worker_count || initialized->frame.routing_epoch == 0) {
        return false;
    }
    const auto bind = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::bind_worker,
        .request_id = 2,
        .target_worker = worker,
    });
    if (!bind || !send_all(socket, *bind)) {
        return false;
    }
    const auto bind_frame = receive_response(socket);
    const auto bound = glifistore::server::decode_response(bind_frame);
    return bound && bound->frame.status == glifistore::server::ResponseStatus::ok &&
           bound->frame.owner_worker == worker && bound->frame.worker_count == worker_count;
}

struct LifecycleProbeResponse final {
    glifistore::server::DecodedFrame<glifistore::server::ResponseView> decoded{};
    std::vector<std::byte> frame_bytes{};
};

[[nodiscard]] inline auto probe_lifecycle(const int socket, const glifistore::server::RequestOpcode opcode,
                                          const std::uint64_t request_id)
    -> std::optional<LifecycleProbeResponse> {
    const auto request = glifistore::server::encode_request({.opcode = opcode, .request_id = request_id});
    if (!request || !send_all(socket, *request)) {
        return std::nullopt;
    }
    LifecycleProbeResponse result{.frame_bytes = receive_response(socket)};
    const auto decoded = glifistore::server::decode_response(result.frame_bytes);
    if (!decoded) {
        return std::nullopt;
    }
    result.decoded = *decoded;
    return result;
}

class ServerTemporaryDirectory final {
  public:
    ServerTemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-server-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        root_ = created;
    }

    ~ServerTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] auto store_path() const -> std::filesystem::path {
        return root_ / "store";
    }

  private:
    std::filesystem::path root_;
};

class BlockingColdRead final {
  public:
    void arm() {
        const std::lock_guard lock{mutex_};
        armed_ = true;
    }

    [[nodiscard]] auto wait_until_blocked() -> bool {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, std::chrono::seconds{15}, [this] { return blocked_; });
    }

    void release() {
        {
            const std::lock_guard lock{mutex_};
            released_ = true;
        }
        condition_.notify_all();
    }

    [[nodiscard]] auto wait_until_finished() -> bool {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, std::chrono::seconds{15}, [this] { return finished_; });
    }

    static auto read_some_at(void* opaque, const int descriptor, const std::span<std::byte> output,
                             const std::uint64_t offset) -> std::ptrdiff_t {
        auto& state = *static_cast<BlockingColdRead*>(opaque);
        bool claimed_here{};
        if (offset >= glifistore::kSegmentHeaderReservedBytes) {
            std::unique_lock lock{state.mutex_};
            if (state.armed_ && !state.claimed_) {
                state.claimed_ = true;
                claimed_here = true;
                state.blocked_ = true;
                state.condition_.notify_all();
                state.condition_.wait(lock, [&state] { return state.released_; });
            }
        }
        const auto result = ::pread(descriptor, output.data(), output.size(), static_cast<off_t>(offset));
        if (claimed_here) {
            {
                const std::lock_guard lock{state.mutex_};
                state.finished_ = true;
            }
            state.condition_.notify_all();
        }
        return result;
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool armed_{};
    bool claimed_{};
    bool blocked_{};
    bool released_{};
    bool finished_{};
};

class BlockingFileSync final {
  public:
    void arm() {
        const std::lock_guard lock{mutex_};
        armed_ = true;
    }

    [[nodiscard]] auto wait_until_blocked() -> bool {
        std::unique_lock lock{mutex_};
        // Hosted BSD VMs under qemu can take several seconds to reach the hooked sync.
        return condition_.wait_for(lock, std::chrono::seconds{15}, [this] { return blocked_; });
    }

    void release() {
        {
            const std::lock_guard lock{mutex_};
            released_ = true;
        }
        condition_.notify_all();
    }

    static auto sync_file(void* opaque, const int descriptor,
                          [[maybe_unused]] const glifistore::FileSyncMode mode) -> int {
        auto& state = *static_cast<BlockingFileSync*>(opaque);
        {
            std::unique_lock lock{state.mutex_};
            if (state.armed_ && !state.claimed_) {
                state.claimed_ = true;
                state.blocked_ = true;
                state.condition_.notify_all();
                state.condition_.wait(lock, [&state] { return state.released_; });
            }
        }
#if defined(__APPLE__)
        if (mode == glifistore::FileSyncMode::full) {
            return ::fcntl(descriptor, F_FULLFSYNC);
        }
#endif
        return ::fsync(descriptor);
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool armed_{};
    bool claimed_{};
    bool blocked_{};
    bool released_{};
};

class SyncReleaseGuard final {
  public:
    explicit SyncReleaseGuard(BlockingFileSync& sync) noexcept : sync_(sync) {}

    ~SyncReleaseGuard() {
        sync_.release();
    }

    SyncReleaseGuard(const SyncReleaseGuard&) = delete;
    auto operator=(const SyncReleaseGuard&) -> SyncReleaseGuard& = delete;

  private:
    BlockingFileSync& sync_;
};

class BlockingCompactionIntent final {
  public:
    ~BlockingCompactionIntent() {
        release();
    }

    void force_next_record_write_full() {
        const std::lock_guard lock{mutex_};
        force_record_full_ = true;
    }

    [[nodiscard]] auto wait_until_blocked() -> bool {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, std::chrono::seconds{15}, [this] { return blocked_; });
    }

    void release() {
        {
            const std::lock_guard lock{mutex_};
            released_ = true;
        }
        condition_.notify_all();
    }

    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& state = *static_cast<BlockingCompactionIntent*>(opaque);
        std::unique_lock lock{state.mutex_};
        if (operation == glifistore::FilesystemOperation::write_record && state.force_record_full_) {
            state.force_record_full_ = false;
            return glifistore::fail(glifistore::ErrorCode::segment_full,
                                    "injected full Segment before publication-lease wait");
        }
        if (operation != glifistore::FilesystemOperation::write_compaction_intent || state.claimed_) {
            return {};
        }
        state.claimed_ = true;
        state.blocked_ = true;
        state.condition_.notify_all();
        state.condition_.wait(lock, [&state] { return state.released_; });
        return {};
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool force_record_full_{};
    bool claimed_{};
    bool blocked_{};
    bool released_{};
};

class GroupBatchObserver final {
  public:
    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& state = *static_cast<GroupBatchObserver*>(opaque);
        const std::lock_guard lock{state.mutex_};
        if (operation == glifistore::FilesystemOperation::write_record) {
            ++state.writes_since_sync_;
        } else if (operation == glifistore::FilesystemOperation::sync_record) {
            state.maximum_writes_before_sync_ =
                std::max(state.maximum_writes_before_sync_, state.writes_since_sync_);
            state.writes_since_sync_ = 0;
            ++state.sync_count_;
        } else if (operation == glifistore::FilesystemOperation::sync_commit_slot) {
            ++state.commit_slot_sync_count_;
        }
        return {};
    }

    [[nodiscard]] auto maximum_writes_before_sync() const -> std::size_t {
        const std::lock_guard lock{mutex_};
        return maximum_writes_before_sync_;
    }

    [[nodiscard]] auto sync_count() const -> std::size_t {
        const std::lock_guard lock{mutex_};
        return sync_count_;
    }

    [[nodiscard]] auto commit_slot_sync_count() const -> std::size_t {
        const std::lock_guard lock{mutex_};
        return commit_slot_sync_count_;
    }

  private:
    mutable std::mutex mutex_;
    std::size_t writes_since_sync_{};
    std::size_t maximum_writes_before_sync_{};
    std::size_t sync_count_{};
    std::size_t commit_slot_sync_count_{};
};

} // namespace glifistore::test::server_reactor_support
