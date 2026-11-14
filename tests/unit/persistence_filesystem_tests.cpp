#include "glifistore/persistence/bootstrap.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/resource_limits.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-fs-test-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        path_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

auto test_manifest(std::uint64_t manifest_generation = 1) -> glifistore::Manifest {
    return {
        .store_id = {std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14},
                     std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18}, std::byte{0x19},
                     std::byte{0x1A}, std::byte{0x1B}, std::byte{0x1C}, std::byte{0x1D}, std::byte{0x1E},
                     std::byte{0x1F}},
        .manifest_generation = manifest_generation,
        .worker_count = 1,
        .routing_epoch = 1,
        .next_segment_id = glifistore::SegmentId{2},
        .next_segment_generation = glifistore::GenerationId{1},
        .segments = {{.segment_id = glifistore::SegmentId{1},
                      .generation = glifistore::GenerationId{1},
                      .owner_worker = glifistore::WorkerId{0},
                      .role = glifistore::ManifestSegmentRole::active}},
    };
}

auto test_compaction_intent() -> glifistore::DurableCompactionIntent {
    auto old = test_manifest(7);
    old.next_segment_id = glifistore::SegmentId{4};
    old.segments = {
        {.segment_id = glifistore::SegmentId{1},
         .generation = glifistore::GenerationId{1},
         .owner_worker = glifistore::WorkerId{0},
         .role = glifistore::ManifestSegmentRole::sealed},
        {.segment_id = glifistore::SegmentId{2},
         .generation = glifistore::GenerationId{1},
         .owner_worker = glifistore::WorkerId{0},
         .role = glifistore::ManifestSegmentRole::sealed},
        {.segment_id = glifistore::SegmentId{3},
         .generation = glifistore::GenerationId{1},
         .owner_worker = glifistore::WorkerId{0},
         .role = glifistore::ManifestSegmentRole::active},
    };
    auto next = old;
    ++next.manifest_generation;
    next.segments = {
        {.segment_id = glifistore::SegmentId{1},
         .generation = glifistore::GenerationId{2},
         .owner_worker = glifistore::WorkerId{0},
         .role = glifistore::ManifestSegmentRole::sealed},
        old.segments.back(),
    };
    return {.worker_id = glifistore::WorkerId{0},
            .old_manifest = std::move(old),
            .next_manifest = std::move(next)};
}

struct InjectedFailure {
    glifistore::FilesystemOperation operation{glifistore::FilesystemOperation::write_manifest};
    bool enabled{};
};

struct AvailableSpaceProbe {
    std::uint64_t bytes{};

    static auto read(void* context) -> glifistore::Result<std::uint64_t> {
        return static_cast<AvailableSpaceProbe*>(context)->bytes;
    }
};

struct FragmentedPositionalIo {
    std::size_t maximum_chunk{2};
    std::size_t read_calls{};
    std::size_t write_calls{};
    std::size_t sync_calls{};
    bool interrupt_next_read{true};
    bool interrupt_next_write{true};
    bool interrupt_next_sync{true};

    static auto read(void* context, const int descriptor, const std::span<std::byte> bytes,
                     const std::uint64_t offset) -> std::ptrdiff_t {
        auto& io = *static_cast<FragmentedPositionalIo*>(context);
        ++io.read_calls;
        if (io.interrupt_next_read) {
            io.interrupt_next_read = false;
            errno = EINTR;
            return -1;
        }
        const auto count = std::min(bytes.size(), io.maximum_chunk);
        return static_cast<std::ptrdiff_t>(
            ::pread(descriptor, bytes.data(), count, static_cast<off_t>(offset)));
    }

    static auto write(void* context, const int descriptor, const std::span<const std::byte> bytes,
                      const std::uint64_t offset) -> std::ptrdiff_t {
        auto& io = *static_cast<FragmentedPositionalIo*>(context);
        ++io.write_calls;
        if (io.interrupt_next_write) {
            io.interrupt_next_write = false;
            errno = EINTR;
            return -1;
        }
        const auto count = std::min(bytes.size(), io.maximum_chunk);
        return static_cast<std::ptrdiff_t>(
            ::pwrite(descriptor, bytes.data(), count, static_cast<off_t>(offset)));
    }

    static auto sync(void* context, const int descriptor, glifistore::FileSyncMode) -> int {
        auto& io = *static_cast<FragmentedPositionalIo*>(context);
        ++io.sync_calls;
        if (io.interrupt_next_sync) {
            io.interrupt_next_sync = false;
            errno = EINTR;
            return -1;
        }
        return ::fsync(descriptor);
    }
};

struct FailingPositionalIo {
    int error_number{EIO};

    static auto read(void* context, int, std::span<std::byte>, std::uint64_t) -> std::ptrdiff_t {
        errno = static_cast<FailingPositionalIo*>(context)->error_number;
        return -1;
    }

    static auto write(void* context, int, std::span<const std::byte>, std::uint64_t) -> std::ptrdiff_t {
        errno = static_cast<FailingPositionalIo*>(context)->error_number;
        return -1;
    }

    static auto sync(void* context, int, glifistore::FileSyncMode) -> int {
        errno = static_cast<FailingPositionalIo*>(context)->error_number;
        return -1;
    }
};

auto fail_before(void* context, glifistore::FilesystemOperation operation) -> glifistore::Status {
    auto& failure = *static_cast<InjectedFailure*>(context);
    if (failure.enabled && operation == failure.operation) {
        return glifistore::fail(glifistore::ErrorCode::io_error, "injected filesystem failure");
    }
    return {};
}

} // namespace

GLIFI_TEST("positional file IO completes exact extents and rejects offset overflow") {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "positional.bin";
    glifistore::FileDescriptor file{::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
    GLIFI_REQUIRE(file.valid());

    static constexpr std::array payload{std::byte{0xA1}, std::byte{0xB2}, std::byte{0xC3}};
    GLIFI_REQUIRE(file.write_all_at(payload, 3).has_value());
    GLIFI_REQUIRE(file.sync(glifistore::FileSyncMode::ordered).has_value());
    GLIFI_REQUIRE(file.sync(glifistore::FileSyncMode::data).has_value());
    const auto size = file.size();
    GLIFI_REQUIRE(size.has_value());
    GLIFI_REQUIRE(*size == 6);

    std::array<std::byte, payload.size()> decoded{};
    GLIFI_REQUIRE(file.read_exact_at(decoded, 3).has_value());
    GLIFI_REQUIRE(decoded == payload);
    GLIFI_REQUIRE(!file.read_exact_at(decoded, 6).has_value());
    GLIFI_REQUIRE(!file.write_all_at(payload, std::numeric_limits<std::uint64_t>::max()).has_value());
}

GLIFI_TEST("positional file IO retries EINTR and completes deterministic short transfers") {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "fragmented.bin";
    FragmentedPositionalIo io{};
    glifistore::FileDescriptor original{::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600),
                                        {.context = &io,
                                         .read_some_at = &FragmentedPositionalIo::read,
                                         .write_some_at = &FragmentedPositionalIo::write,
                                         .sync_file = &FragmentedPositionalIo::sync}};
    glifistore::FileDescriptor file{std::move(original)};
    GLIFI_REQUIRE(file.valid());
    GLIFI_REQUIRE(!original.valid());

    static constexpr std::array payload{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
                                        std::byte{0x50}, std::byte{0x60}, std::byte{0x70}};
    GLIFI_REQUIRE(file.write_all_at(payload, 5).has_value());
    GLIFI_REQUIRE(file.sync(glifistore::FileSyncMode::full).has_value());
    std::array<std::byte, payload.size()> decoded{};
    GLIFI_REQUIRE(file.read_exact_at(decoded, 5).has_value());
    GLIFI_REQUIRE(decoded == payload);
    GLIFI_REQUIRE(io.write_calls == 5);
    GLIFI_REQUIRE(io.read_calls == 5);
    GLIFI_REQUIRE(io.sync_calls == 2);
}

GLIFI_TEST("positional write faults preserve native resource categories") {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "faulted.bin";
    FailingPositionalIo io{};
    glifistore::FileDescriptor file{::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600),
                                    {.context = &io,
                                     .read_some_at = &FailingPositionalIo::read,
                                     .write_some_at = &FailingPositionalIo::write,
                                     .sync_file = &FailingPositionalIo::sync}};
    GLIFI_REQUIRE(file.valid());
    static constexpr std::array payload{std::byte{0x01}};

    io.error_number = ENOSPC;
    auto result = file.write_all_at(payload, 0);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::storage_exhausted);
#if defined(EDQUOT)
    io.error_number = EDQUOT;
    result = file.write_all_at(payload, 0);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::storage_exhausted);
#endif
    io.error_number = EROFS;
    result = file.write_all_at(payload, 0);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::read_only_filesystem);
    io.error_number = EIO;
    result = file.write_all_at(payload, 0);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::io_error);
    std::array<std::byte, 1> decoded{};
    result = file.read_exact_at(decoded, 0);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::io_error);
    result = file.sync(glifistore::FileSyncMode::data);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::io_error);
}

// GS-PERSIST-FAULT-001: deterministic syscall-class matrix (EINTR / short I/O / ENOSPC / EDQUOT / EIO).
// Labels are E0–E2 fault-injection only; never an E3/E4 physical certification claim.
GLIFI_TEST("positional IO fault matrix covers EINTR short IO and capacity errno classes") {
    struct MatrixIo {
        std::size_t maximum_chunk{1};
        std::size_t remaining_eintr{3};
        int fatal_errno{0};
        bool fail_sync_only{false};
        std::size_t read_calls{};
        std::size_t write_calls{};
        std::size_t sync_calls{};

        static auto read(void* context, const int descriptor, const std::span<std::byte> bytes,
                         const std::uint64_t offset) -> std::ptrdiff_t {
            auto& io = *static_cast<MatrixIo*>(context);
            ++io.read_calls;
            if (io.fatal_errno != 0 && !io.fail_sync_only) {
                errno = io.fatal_errno;
                return -1;
            }
            if (io.remaining_eintr > 0) {
                --io.remaining_eintr;
                errno = EINTR;
                return -1;
            }
            const auto count = std::min(bytes.size(), io.maximum_chunk);
            return static_cast<std::ptrdiff_t>(
                ::pread(descriptor, bytes.data(), count, static_cast<off_t>(offset)));
        }

        static auto write(void* context, const int descriptor, const std::span<const std::byte> bytes,
                          const std::uint64_t offset) -> std::ptrdiff_t {
            auto& io = *static_cast<MatrixIo*>(context);
            ++io.write_calls;
            if (io.fatal_errno != 0 && !io.fail_sync_only) {
                errno = io.fatal_errno;
                return -1;
            }
            if (io.remaining_eintr > 0) {
                --io.remaining_eintr;
                errno = EINTR;
                return -1;
            }
            const auto count = std::min(bytes.size(), io.maximum_chunk);
            return static_cast<std::ptrdiff_t>(
                ::pwrite(descriptor, bytes.data(), count, static_cast<off_t>(offset)));
        }

        static auto sync(void* context, const int descriptor, glifistore::FileSyncMode) -> int {
            auto& io = *static_cast<MatrixIo*>(context);
            ++io.sync_calls;
            if (io.fatal_errno != 0 && io.fail_sync_only) {
                errno = io.fatal_errno;
                return -1;
            }
            if (io.remaining_eintr > 0) {
                --io.remaining_eintr;
                errno = EINTR;
                return -1;
            }
            return ::fsync(descriptor);
        }
    };

    static constexpr std::array payload{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};

    // Repeated EINTR + single-byte short transfers still complete exact extents.
    {
        TemporaryDirectory temporary;
        MatrixIo io{.maximum_chunk = 1, .remaining_eintr = 4};
        glifistore::FileDescriptor file{
            ::open((temporary.path() / "matrix-ok.bin").c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600),
            {.context = &io,
             .read_some_at = &MatrixIo::read,
             .write_some_at = &MatrixIo::write,
             .sync_file = &MatrixIo::sync}};
        GLIFI_REQUIRE(file.valid());
        GLIFI_REQUIRE(file.write_all_at(payload, 0).has_value());
        io.remaining_eintr = 2;
        GLIFI_REQUIRE(file.sync(glifistore::FileSyncMode::full).has_value());
        std::array<std::byte, payload.size()> decoded{};
        GLIFI_REQUIRE(file.read_exact_at(decoded, 0).has_value());
        GLIFI_REQUIRE(decoded == payload);
        GLIFI_REQUIRE(io.write_calls >= payload.size());
        GLIFI_REQUIRE(io.read_calls >= payload.size());
        GLIFI_REQUIRE(io.sync_calls >= 3);
    }

    const auto expect_write_failure = [](const int error_number, const glifistore::ErrorCode expected) {
        TemporaryDirectory temporary;
        MatrixIo io{.fatal_errno = error_number};
        glifistore::FileDescriptor file{::open((temporary.path() / "matrix-fail.bin").c_str(),
                                               O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600),
                                        {.context = &io,
                                         .read_some_at = &MatrixIo::read,
                                         .write_some_at = &MatrixIo::write,
                                         .sync_file = &MatrixIo::sync}};
        GLIFI_REQUIRE(file.valid());
        const auto result = file.write_all_at(payload, 0);
        GLIFI_REQUIRE(!result.has_value());
        GLIFI_REQUIRE(result.error().code == expected);
    };

    expect_write_failure(ENOSPC, glifistore::ErrorCode::storage_exhausted);
#if defined(EDQUOT)
    expect_write_failure(EDQUOT, glifistore::ErrorCode::storage_exhausted);
#endif
    expect_write_failure(EIO, glifistore::ErrorCode::io_error);

    // Capacity / I/O faults on sync are classified the same way as write faults.
    const auto expect_sync_failure = [](const int error_number, const glifistore::ErrorCode expected) {
        TemporaryDirectory temporary;
        MatrixIo io{};
        glifistore::FileDescriptor file{::open((temporary.path() / "matrix-sync.bin").c_str(),
                                               O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600),
                                        {.context = &io,
                                         .read_some_at = &MatrixIo::read,
                                         .write_some_at = &MatrixIo::write,
                                         .sync_file = &MatrixIo::sync}};
        GLIFI_REQUIRE(file.valid());
        GLIFI_REQUIRE(file.write_all_at(payload, 0).has_value());
        io.fatal_errno = error_number;
        io.fail_sync_only = true;
        const auto result = file.sync(glifistore::FileSyncMode::data);
        GLIFI_REQUIRE(!result.has_value());
        GLIFI_REQUIRE(result.error().code == expected);
    };

    expect_sync_failure(ENOSPC, glifistore::ErrorCode::storage_exhausted);
#if defined(EDQUOT)
    expect_sync_failure(EDQUOT, glifistore::ErrorCode::storage_exhausted);
#endif
    expect_sync_failure(EIO, glifistore::ErrorCode::io_error);
}

GLIFI_TEST("data directory holds one process lock and rejects a symlink root") {
    TemporaryDirectory temporary;
    {
        const auto first = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(first.has_value());
        const auto second = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(!second.has_value());
        GLIFI_REQUIRE(second.error().code == glifistore::ErrorCode::io_error);
    }
    GLIFI_REQUIRE(glifistore::DataDirectory::open_and_lock(temporary.path()).has_value());

    TemporaryDirectory parent;
    const auto target = parent.path() / "target";
    const auto link = parent.path() / "link";
    GLIFI_REQUIRE(std::filesystem::create_directory(target));
    std::filesystem::create_directory_symlink(target, link);
    const auto through_symlink = glifistore::DataDirectory::open_and_lock(link);
    GLIFI_REQUIRE(!through_symlink.has_value());
}

GLIFI_TEST("data directory creation faults distinguish mkdir from parent synchronization") {
    {
        TemporaryDirectory parent;
        const auto path = parent.path() / "store";
        InjectedFailure failure{.operation = glifistore::FilesystemOperation::create_data_directory,
                                .enabled = true};
        const auto rejected =
            glifistore::DataDirectory::open_and_lock(path, glifistore::DataDirectoryOpenMode::create_new,
                                                     {.context = &failure, .before = &fail_before});
        GLIFI_REQUIRE(!rejected.has_value());
        GLIFI_REQUIRE(!std::filesystem::exists(path));
    }
    {
        TemporaryDirectory parent;
        const auto path = parent.path() / "store";
        InjectedFailure failure{.operation = glifistore::FilesystemOperation::sync_parent_directory,
                                .enabled = true};
        const auto indeterminate =
            glifistore::DataDirectory::open_and_lock(path, glifistore::DataDirectoryOpenMode::create_new,
                                                     {.context = &failure, .before = &fail_before});
        GLIFI_REQUIRE(!indeterminate.has_value());
        GLIFI_REQUIRE(std::filesystem::is_directory(path));
        auto reopened = glifistore::DataDirectory::open_and_lock(path);
        GLIFI_REQUIRE(reopened.has_value());
        const auto pristine = reopened->pristine_for_bootstrap();
        GLIFI_REQUIRE(pristine.has_value());
        GLIFI_REQUIRE(*pristine);
    }
}

GLIFI_TEST("data directory available-space preflight preserves the configured reserve") {
    TemporaryDirectory temporary;
    AvailableSpaceProbe probe{.bytes = 1023};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), {.context = &probe, .available_space_bytes = &AvailableSpaceProbe::read});
    GLIFI_REQUIRE(directory.has_value());
    const auto sampled = directory->available_space_bytes();
    GLIFI_REQUIRE(sampled.has_value());
    GLIFI_REQUIRE(*sampled == probe.bytes);

    auto limits = glifistore::DurableResourceLimits{};
    limits.reserved_free_bytes = 512;
    const auto insufficient = glifistore::require_durable_available_space(*directory, 512, limits);
    GLIFI_REQUIRE(!insufficient.has_value());
    GLIFI_REQUIRE(insufficient.error().code == glifistore::ErrorCode::storage_exhausted);

    probe.bytes = 1024;
    GLIFI_REQUIRE(glifistore::require_durable_available_space(*directory, 512, limits).has_value());
}

GLIFI_TEST("durable bootstrap rejects insufficient space before publishing intent") {
    TemporaryDirectory temporary;
    AvailableSpaceProbe probe{.bytes = glifistore::kSegmentSizeBytes};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), {.context = &probe, .available_space_bytes = &AvailableSpaceProbe::read});
    GLIFI_REQUIRE(directory.has_value());
    auto limits = glifistore::DurableResourceLimits{};
    limits.reserved_free_bytes = 0;
    const auto prepared = glifistore::prepare_durable_store(
        *directory, glifistore::DurableOpenMode::open_or_create, 1, 1, limits);
    GLIFI_REQUIRE(!prepared.has_value());
    GLIFI_REQUIRE(prepared.error().code == glifistore::ErrorCode::storage_exhausted);
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kBootstrapIntentFilename));
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kManifestFilename));
}

GLIFI_TEST("manifest publication atomically replaces and reads a complete generation") {
    TemporaryDirectory temporary;
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());

    const auto first = test_manifest(1);
    const auto first_publication = directory->publish_manifest(first);
    GLIFI_REQUIRE(first_publication.durable());
    GLIFI_REQUIRE(!first_publication.error.has_value());
    GLIFI_REQUIRE(std::filesystem::exists(temporary.path() / glifistore::kManifestFilename));
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kManifestTemporaryFilename));
    const auto first_read = directory->read_manifest();
    GLIFI_REQUIRE(first_read.has_value());
    GLIFI_REQUIRE(*first_read == first);
    const auto encoded_size = glifistore::encoded_manifest_size(first);
    GLIFI_REQUIRE(encoded_size.has_value());
    const auto limited_read = directory->read_manifest(*encoded_size - 1U);
    GLIFI_REQUIRE(!limited_read.has_value());
    GLIFI_REQUIRE(limited_read.error().code == glifistore::ErrorCode::storage_exhausted);
    const auto second = test_manifest(2);
    const auto limited_publication = directory->publish_manifest(second, *encoded_size - 1U);
    GLIFI_REQUIRE(limited_publication.outcome == glifistore::ManifestPublicationOutcome::not_published);
    GLIFI_REQUIRE(limited_publication.error.has_value());
    GLIFI_REQUIRE(limited_publication.error->code == glifistore::ErrorCode::storage_exhausted);

    GLIFI_REQUIRE(directory->publish_manifest(second).durable());
    const auto second_read = directory->read_manifest();
    GLIFI_REQUIRE(second_read.has_value());
    GLIFI_REQUIRE(*second_read == second);

    const auto rollback = directory->publish_manifest(first);
    GLIFI_REQUIRE(rollback.outcome == glifistore::ManifestPublicationOutcome::not_published);
    GLIFI_REQUIRE(rollback.error.has_value());
    GLIFI_REQUIRE(rollback.error->code == glifistore::ErrorCode::sequence_conflict);
    const auto after_rollback = directory->read_manifest();
    GLIFI_REQUIRE(after_rollback.has_value());
    GLIFI_REQUIRE(*after_rollback == second);
}

GLIFI_TEST("manifest publication and reopen tolerate EINTR and fragmented positional IO") {
    TemporaryDirectory temporary;
    FragmentedPositionalIo io{.maximum_chunk = 7};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), {.file_io = {.context = &io,
                                       .read_some_at = &FragmentedPositionalIo::read,
                                       .write_some_at = &FragmentedPositionalIo::write}});
    GLIFI_REQUIRE(directory.has_value());
    const auto expected = test_manifest();
    GLIFI_REQUIRE(directory->publish_manifest(expected).durable());
    const auto recovered = directory->read_manifest();
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE(*recovered == expected);
    GLIFI_REQUIRE(io.write_calls > 1);
    GLIFI_REQUIRE(io.read_calls > 1);
}

GLIFI_TEST("pre-rename failure preserves the old manifest and keeps the directory usable") {
    TemporaryDirectory temporary;
    InjectedFailure failure{};
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path(),
                                                              {.context = &failure, .before = &fail_before});
    GLIFI_REQUIRE(directory.has_value());
    const auto first = test_manifest(1);
    GLIFI_REQUIRE(directory->publish_manifest(first).durable());

    failure.operation = glifistore::FilesystemOperation::sync_manifest;
    failure.enabled = true;
    const auto failed = directory->publish_manifest(test_manifest(2));
    GLIFI_REQUIRE(failed.outcome == glifistore::ManifestPublicationOutcome::not_published);
    GLIFI_REQUIRE(failed.error.has_value());
    GLIFI_REQUIRE(directory->healthy());
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kManifestTemporaryFilename));
    const auto recovered = directory->read_manifest();
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE(*recovered == first);
}

GLIFI_TEST("manifest prepublication fault matrix preserves the durable generation") {
    static constexpr std::array boundaries{
        glifistore::FilesystemOperation::write_manifest,
        glifistore::FilesystemOperation::sync_manifest,
        glifistore::FilesystemOperation::rename_manifest,
    };
    for (const auto boundary : boundaries) {
        TemporaryDirectory temporary;
        InjectedFailure failure{};
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), {.context = &failure, .before = &fail_before});
        GLIFI_REQUIRE(directory.has_value());
        const auto first = test_manifest(1);
        GLIFI_REQUIRE(directory->publish_manifest(first).durable());

        failure.operation = boundary;
        failure.enabled = true;
        const auto rejected = directory->publish_manifest(test_manifest(2));
        GLIFI_REQUIRE(rejected.outcome == glifistore::ManifestPublicationOutcome::not_published);
        GLIFI_REQUIRE(rejected.error.has_value());
        GLIFI_REQUIRE(directory->healthy());
        GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kManifestTemporaryFilename));
        failure.enabled = false;
        const auto recovered = directory->read_manifest();
        GLIFI_REQUIRE(recovered.has_value());
        GLIFI_REQUIRE(*recovered == first);
    }
}

GLIFI_TEST("post-rename sync failure is indeterminate and poisons the directory instance") {
    TemporaryDirectory temporary;
    InjectedFailure failure{};
    {
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), {.context = &failure, .before = &fail_before});
        GLIFI_REQUIRE(directory.has_value());
        GLIFI_REQUIRE(directory->publish_manifest(test_manifest(1)).durable());

        failure.operation = glifistore::FilesystemOperation::sync_directory;
        failure.enabled = true;
        const auto failed = directory->publish_manifest(test_manifest(2));
        GLIFI_REQUIRE(failed.outcome == glifistore::ManifestPublicationOutcome::indeterminate);
        GLIFI_REQUIRE(failed.error.has_value());
        GLIFI_REQUIRE(!directory->healthy());
        GLIFI_REQUIRE(!directory->read_manifest().has_value());
        GLIFI_REQUIRE(directory->publish_manifest(test_manifest(3)).outcome ==
                      glifistore::ManifestPublicationOutcome::indeterminate);
    }

    const auto reopened = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(reopened.has_value());
    const auto recovered = reopened->read_manifest();
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE(recovered->manifest_generation == 2);
}

GLIFI_TEST("invalid manifest fails before creating a publication") {
    TemporaryDirectory temporary;
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    auto invalid = test_manifest();
    invalid.worker_count = 0;
    const auto result = directory->publish_manifest(invalid);
    GLIFI_REQUIRE(result.outcome == glifistore::ManifestPublicationOutcome::not_published);
    GLIFI_REQUIRE(result.error.has_value());
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kManifestFilename));
}

GLIFI_TEST("bootstrap intent publication cleans pre-rename failures") {
    TemporaryDirectory temporary;
    InjectedFailure failure{.operation = glifistore::FilesystemOperation::sync_bootstrap, .enabled = true};
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path(),
                                                              {.context = &failure, .before = &fail_before});
    GLIFI_REQUIRE(directory.has_value());
    const auto published = directory->publish_bootstrap_intent(test_manifest());
    GLIFI_REQUIRE(!published.has_value());
    GLIFI_REQUIRE(directory->healthy());
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kBootstrapTemporaryFilename));
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kBootstrapIntentFilename));
}

GLIFI_TEST("bootstrap intent prepublication fault matrix leaves a pristine namespace") {
    static constexpr std::array boundaries{
        glifistore::FilesystemOperation::write_bootstrap,
        glifistore::FilesystemOperation::sync_bootstrap,
        glifistore::FilesystemOperation::rename_bootstrap,
    };
    for (const auto boundary : boundaries) {
        TemporaryDirectory temporary;
        InjectedFailure failure{.operation = boundary, .enabled = true};
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), {.context = &failure, .before = &fail_before});
        GLIFI_REQUIRE(directory.has_value());
        const auto rejected = directory->publish_bootstrap_intent(test_manifest());
        GLIFI_REQUIRE(!rejected.has_value());
        GLIFI_REQUIRE(directory->healthy());
        GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kBootstrapTemporaryFilename));
        GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kBootstrapIntentFilename));
        const auto pristine = directory->pristine_for_bootstrap();
        GLIFI_REQUIRE(pristine.has_value());
        GLIFI_REQUIRE(*pristine);
    }
}

GLIFI_TEST("bootstrap intent post-rename sync failure reopens as a complete intent") {
    TemporaryDirectory temporary;
    InjectedFailure failure{.operation = glifistore::FilesystemOperation::sync_directory, .enabled = true};
    const auto expected = test_manifest();
    {
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), {.context = &failure, .before = &fail_before});
        GLIFI_REQUIRE(directory.has_value());
        const auto published = directory->publish_bootstrap_intent(expected);
        GLIFI_REQUIRE(!published.has_value());
        GLIFI_REQUIRE(!directory->healthy());
    }
    auto reopened = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(reopened.has_value());
    const auto intent = reopened->read_bootstrap_intent();
    GLIFI_REQUIRE(intent.has_value());
    GLIFI_REQUIRE(*intent == expected);
}

GLIFI_TEST("compaction intent publication reads and removes one exact transaction") {
    TemporaryDirectory temporary;
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    const auto expected = test_compaction_intent();
    const auto published = directory->publish_compaction_intent(expected);
    GLIFI_REQUIRE(published.durable());
    GLIFI_REQUIRE(std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionTemporaryFilename));
    const auto recovered = directory->read_compaction_intent();
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE(*recovered == expected);
    const auto over_budget = directory->read_compaction_intent(glifistore::kManifestHeaderBytes);
    GLIFI_REQUIRE(!over_budget.has_value());
    GLIFI_REQUIRE(over_budget.error().code == glifistore::ErrorCode::storage_exhausted);

    const auto duplicate = directory->publish_compaction_intent(expected);
    GLIFI_REQUIRE(duplicate.outcome == glifistore::CompactionIntentPublicationOutcome::not_published);
    GLIFI_REQUIRE(duplicate.error.has_value());
    GLIFI_REQUIRE(duplicate.error->code == glifistore::ErrorCode::sequence_conflict);
    GLIFI_REQUIRE(directory->healthy());

    const auto removed = directory->remove_compaction_intent();
    GLIFI_REQUIRE(removed.durable());
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    const auto absent = directory->read_compaction_intent();
    GLIFI_REQUIRE(!absent.has_value());
    GLIFI_REQUIRE(absent.error().code == glifistore::ErrorCode::not_found);
}

GLIFI_TEST("compaction intent prepublication fault matrix leaves no authority") {
    static constexpr std::array boundaries{
        glifistore::FilesystemOperation::write_compaction_intent,
        glifistore::FilesystemOperation::sync_compaction_intent,
        glifistore::FilesystemOperation::rename_compaction_intent,
    };
    for (const auto boundary : boundaries) {
        TemporaryDirectory temporary;
        InjectedFailure failure{.operation = boundary, .enabled = true};
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), {.context = &failure, .before = &fail_before});
        GLIFI_REQUIRE(directory.has_value());
        const auto rejected = directory->publish_compaction_intent(test_compaction_intent());
        GLIFI_REQUIRE(rejected.outcome == glifistore::CompactionIntentPublicationOutcome::not_published);
        GLIFI_REQUIRE(rejected.error.has_value());
        GLIFI_REQUIRE(directory->healthy());
        GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
        GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionTemporaryFilename));
    }
}

GLIFI_TEST("compaction intent post-rename sync failure is reopenable and indeterminate") {
    TemporaryDirectory temporary;
    InjectedFailure failure{.operation = glifistore::FilesystemOperation::sync_directory, .enabled = true};
    const auto expected = test_compaction_intent();
    {
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), {.context = &failure, .before = &fail_before});
        GLIFI_REQUIRE(directory.has_value());
        const auto published = directory->publish_compaction_intent(expected);
        GLIFI_REQUIRE(published.outcome == glifistore::CompactionIntentPublicationOutcome::indeterminate);
        GLIFI_REQUIRE(published.error.has_value());
        GLIFI_REQUIRE(!directory->healthy());
    }
    auto reopened = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(reopened.has_value());
    const auto recovered = reopened->read_compaction_intent();
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE(*recovered == expected);
}

GLIFI_TEST("compaction intent removal sync failure is indeterminate after unlink") {
    TemporaryDirectory temporary;
    InjectedFailure failure{};
    {
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), {.context = &failure, .before = &fail_before});
        GLIFI_REQUIRE(directory.has_value());
        GLIFI_REQUIRE(directory->publish_compaction_intent(test_compaction_intent()).durable());
        failure.operation = glifistore::FilesystemOperation::sync_directory;
        failure.enabled = true;
        const auto removed = directory->remove_compaction_intent();
        GLIFI_REQUIRE(removed.outcome == glifistore::CompactionIntentRemovalOutcome::indeterminate);
        GLIFI_REQUIRE(removed.error.has_value());
        GLIFI_REQUIRE(!directory->healthy());
    }
    auto reopened = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(reopened.has_value());
    const auto recovered = reopened->read_compaction_intent();
    GLIFI_REQUIRE(!recovered.has_value());
    GLIFI_REQUIRE(recovered.error().code == glifistore::ErrorCode::not_found);
}

GLIFI_TEST("compaction intent removal fault before unlink preserves the authority") {
    TemporaryDirectory temporary;
    InjectedFailure failure{.operation = glifistore::FilesystemOperation::remove_compaction_intent,
                            .enabled = true};
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path(),
                                                              {.context = &failure, .before = &fail_before});
    GLIFI_REQUIRE(directory.has_value());
    const auto expected = test_compaction_intent();
    failure.enabled = false;
    GLIFI_REQUIRE(directory->publish_compaction_intent(expected).durable());
    failure.enabled = true;
    const auto rejected = directory->remove_compaction_intent();
    GLIFI_REQUIRE(rejected.outcome == glifistore::CompactionIntentRemovalOutcome::not_removed);
    GLIFI_REQUIRE(rejected.error.has_value());
    GLIFI_REQUIRE(directory->healthy());
    failure.enabled = false;
    const auto recovered = directory->read_compaction_intent();
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE(*recovered == expected);
}
