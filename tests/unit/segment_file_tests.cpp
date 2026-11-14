#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/record.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

class SegmentTemporaryDirectory final {
  public:
    SegmentTemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-segment-file-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        path_ = created;
    }

    ~SegmentTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

auto segment_identity(std::uint64_t segment = 0x12, std::uint32_t generation = 3)
    -> glifistore::SegmentHeaderIdentity {
    return {
        .store_id = {std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14},
                     std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18}, std::byte{0x19},
                     std::byte{0x1A}, std::byte{0x1B}, std::byte{0x1C}, std::byte{0x1D}, std::byte{0x1E},
                     std::byte{0x1F}},
        .segment_id = glifistore::SegmentId{segment},
        .generation = glifistore::GenerationId{generation},
        .owner_worker = glifistore::WorkerId{2},
    };
}

auto encoded_record(std::uint64_t sequence, std::string key, std::string value)
    -> glifistore::Result<std::vector<std::byte>> {
    const auto key_bytes = std::as_bytes(std::span{key});
    const auto value_bytes = std::as_bytes(std::span{value});
    return glifistore::encode_record({
        .sequence = glifistore::SequenceNumber{sequence},
        .opcode = glifistore::Opcode::put,
        .type = glifistore::ValueType::bytes,
        .flags = 0,
        .key_hash = sequence * 17,
        .expire_at_ns = 0,
        .key = key_bytes,
        .value = value_bytes,
    });
}

struct SegmentInjectedFailure {
    glifistore::FilesystemOperation operation{glifistore::FilesystemOperation::preallocate_segment};
    bool enabled{};
};

struct PacedRecordIo final {
    static constexpr std::size_t kGrantBytes = 32;

    bool inside_record_write{};
    std::vector<std::size_t> physical_write_sizes{};
    std::size_t grants{};

    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& state = *static_cast<PacedRecordIo*>(opaque);
        if (operation == glifistore::FilesystemOperation::write_record) {
            state.inside_record_write = true;
        }
        return {};
    }

    static void after(void* opaque, const glifistore::FilesystemOperation operation) {
        auto& state = *static_cast<PacedRecordIo*>(opaque);
        if (operation == glifistore::FilesystemOperation::write_record) {
            state.inside_record_write = false;
        }
    }

    static auto write_some_at(void* opaque, const int descriptor, const std::span<const std::byte> bytes,
                              const std::uint64_t offset) -> std::ptrdiff_t {
        auto& state = *static_cast<PacedRecordIo*>(opaque);
        if (state.inside_record_write) {
            state.physical_write_sizes.push_back(bytes.size());
        }
        return ::pwrite(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset));
    }

    static auto acquire(void* opaque, const std::size_t requested_bytes) -> glifistore::Result<std::size_t> {
        auto& state = *static_cast<PacedRecordIo*>(opaque);
        ++state.grants;
        return std::min(requested_bytes, kGrantBytes);
    }
};

auto fail_segment_operation(void* context, glifistore::FilesystemOperation operation) -> glifistore::Status {
    auto& failure = *static_cast<SegmentInjectedFailure*>(context);
    if (failure.enabled && failure.operation == operation) {
        return glifistore::fail(glifistore::ErrorCode::io_error, "injected Segment failure");
    }
    return {};
}

} // namespace

GLIFI_TEST("Segment filenames are fixed-width lowercase and generation-specific") {
    const auto identity = segment_identity();
    GLIFI_REQUIRE(glifistore::segment_filename(identity) == "segment-0000000000000012-00000003.glifi");
}

GLIFI_TEST("durable Segment creation preallocates exact size and reopens verified identity") {
    SegmentTemporaryDirectory temporary;
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    const auto identity = segment_identity();

    auto created = glifistore::DurableSegmentFile::create(*directory, identity);
    GLIFI_REQUIRE(created.durable());
    GLIFI_REQUIRE(created.file.has_value());
    GLIFI_REQUIRE(created.file->selected_commit().slot_index == 0);
    GLIFI_REQUIRE(created.file->selected_commit().commit.commit_generation == 1);
    GLIFI_REQUIRE(created.file->scan_committed().has_value());

    const auto path = temporary.path() / glifistore::segment_filename(identity);
    GLIFI_REQUIRE(std::filesystem::file_size(path) == glifistore::kSegmentSizeBytes);
    created.file.reset();
    const auto reopened = glifistore::DurableSegmentFile::open(*directory, identity);
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(reopened->identity() == identity);

    auto wrong_identity = identity;
    wrong_identity.store_id[0] = std::byte{0xFF};
    const auto mismatched = glifistore::DurableSegmentFile::open(*directory, wrong_identity);
    GLIFI_REQUIRE(!mismatched.has_value());
    GLIFI_REQUIRE(mismatched.error().code == glifistore::ErrorCode::corrupted_data);

    const auto duplicate = glifistore::DurableSegmentFile::create(*directory, identity);
    GLIFI_REQUIRE(duplicate.outcome == glifistore::SegmentFileCreationOutcome::not_published);
    GLIFI_REQUIRE(duplicate.error.has_value());
    GLIFI_REQUIRE(duplicate.error->code == glifistore::ErrorCode::sequence_conflict);
}

GLIFI_TEST("staged Segment stays private until sealed promotion") {
    SegmentTemporaryDirectory temporary;
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    const auto identity = segment_identity(0x21, 4);
    const auto final_path = temporary.path() / glifistore::segment_filename(identity);
    const auto staged_path = temporary.path() / ('.' + glifistore::segment_filename(identity) + ".tmp");

    auto staged = glifistore::DurableSegmentFile::create_staged(*directory, identity);
    GLIFI_REQUIRE(staged.has_value());
    GLIFI_REQUIRE(std::filesystem::exists(staged_path));
    GLIFI_REQUIRE(!std::filesystem::exists(final_path));
    GLIFI_REQUIRE(!glifistore::DurableSegmentFile::open(*directory, identity).has_value());

    const auto record = encoded_record(3, "staged", "record");
    GLIFI_REQUIRE(record.has_value());
    GLIFI_REQUIRE(staged->append_record(*record).committed());
    GLIFI_REQUIRE(staged->seal().committed());
    auto verified = glifistore::DurableSegmentFile::open_staged(*directory, identity);
    GLIFI_REQUIRE(verified.has_value());
    GLIFI_REQUIRE(verified->selected_commit().commit.state == glifistore::PersistedSegmentState::sealed);

    const std::array identities{identity};
    GLIFI_REQUIRE(glifistore::DurableSegmentFile::promote_staged(*directory, identities).has_value());
    GLIFI_REQUIRE(!std::filesystem::exists(staged_path));
    GLIFI_REQUIRE(std::filesystem::exists(final_path));
    auto reopened = glifistore::DurableSegmentFile::open(*directory, identity);
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(reopened->selected_commit().commit.record_count == 1);
    GLIFI_REQUIRE(directory->healthy());
}

GLIFI_TEST("paced Segment append bounds each physical Record write") {
    SegmentTemporaryDirectory temporary;
    PacedRecordIo pacing;
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), glifistore::FilesystemHooks{
                              .context = &pacing,
                              .before = &PacedRecordIo::before,
                              .after = &PacedRecordIo::after,
                              .file_io = {.context = &pacing, .write_some_at = &PacedRecordIo::write_some_at},
                          });
    GLIFI_REQUIRE(directory.has_value());
    const auto identity = segment_identity(0x24, 7);
    auto staged = glifistore::DurableSegmentFile::create_staged(*directory, identity);
    GLIFI_REQUIRE(staged.has_value());

    const auto record = encoded_record(5, "paced", std::string(512, 'p'));
    GLIFI_REQUIRE(record.has_value());
    const glifistore::SegmentRecordWritePacing write_pacing{
        .context = &pacing,
        .acquire = &PacedRecordIo::acquire,
    };
    GLIFI_REQUIRE(staged->append_record(*record, write_pacing).committed());
    GLIFI_REQUIRE(pacing.grants == pacing.physical_write_sizes.size());
    GLIFI_REQUIRE(pacing.grants > 1);
    GLIFI_REQUIRE(std::ranges::all_of(pacing.physical_write_sizes, [](const std::size_t size) {
        return size > 0U && size <= PacedRecordIo::kGrantBytes;
    }));
    GLIFI_REQUIRE(staged->seal().committed());
    GLIFI_REQUIRE(staged->selected_commit().commit.record_count == 1);
}

GLIFI_TEST("staged Segment discard removes private output without poisoning authority") {
    SegmentTemporaryDirectory temporary;
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    const auto identity = segment_identity(0x22, 5);
    const auto staged_path = temporary.path() / ('.' + glifistore::segment_filename(identity) + ".tmp");
    {
        auto staged = glifistore::DurableSegmentFile::create_staged(*directory, identity);
        GLIFI_REQUIRE(staged.has_value());
        GLIFI_REQUIRE(std::filesystem::exists(staged_path));
    }
    const std::array identities{identity};
    glifistore::DurableSegmentFile::discard_staged(*directory, identities);
    GLIFI_REQUIRE(!std::filesystem::exists(staged_path));
    GLIFI_REQUIRE(directory->healthy());
}

GLIFI_TEST("staged Segment creation never replaces an existing private owner") {
    SegmentTemporaryDirectory temporary;
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    const auto identity = segment_identity(0x23, 6);
    const auto staged_path = temporary.path() / ('.' + glifistore::segment_filename(identity) + ".tmp");

    auto owner = glifistore::DurableSegmentFile::create_staged(*directory, identity);
    GLIFI_REQUIRE(owner.has_value());
    const auto collision = glifistore::DurableSegmentFile::create_staged(*directory, identity);
    GLIFI_REQUIRE(!collision.has_value());
    GLIFI_REQUIRE(collision.error().code == glifistore::ErrorCode::sequence_conflict);
    GLIFI_REQUIRE(std::filesystem::exists(staged_path));

    const auto record = encoded_record(4, "owner", "preserved");
    GLIFI_REQUIRE(record.has_value());
    GLIFI_REQUIRE(owner->append_record(*record).committed());
    GLIFI_REQUIRE(owner->seal().committed());
    GLIFI_REQUIRE(glifistore::DurableSegmentFile::open_staged(*directory, identity).has_value());
    const std::array identities{identity};
    glifistore::DurableSegmentFile::discard_staged(*directory, identities);
    GLIFI_REQUIRE(!std::filesystem::exists(staged_path));
    GLIFI_REQUIRE(directory->healthy());
}

GLIFI_TEST("Segment append synchronizes data before alternating commit slots and scans Records") {
    SegmentTemporaryDirectory temporary;
    std::vector<glifistore::FilesystemOperation> completed_operations;
    const auto hooks = glifistore::FilesystemHooks{
        .context = &completed_operations,
        .after =
            [](void* context, const glifistore::FilesystemOperation operation) {
                static_cast<std::vector<glifistore::FilesystemOperation>*>(context)->push_back(operation);
            },
    };
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path(), hooks);
    GLIFI_REQUIRE(directory.has_value());
    const auto identity = segment_identity();
    auto created = glifistore::DurableSegmentFile::create(*directory, identity);
    GLIFI_REQUIRE(created.durable());
    auto& file = *created.file;

    const auto first = encoded_record(7, "alpha", "one");
    const auto second = encoded_record(11, "beta", "two");
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE(second.has_value());
    completed_operations.clear();
    GLIFI_REQUIRE(file.append(*first).committed());
    const std::vector expected_operations{
        glifistore::FilesystemOperation::write_record,
        glifistore::FilesystemOperation::sync_record,
        glifistore::FilesystemOperation::write_commit_slot,
        glifistore::FilesystemOperation::sync_commit_slot,
    };
    GLIFI_REQUIRE(completed_operations == expected_operations);
    GLIFI_REQUIRE(file.selected_commit().slot_index == 1);
    GLIFI_REQUIRE(file.append(*second).committed());
    GLIFI_REQUIRE(file.selected_commit().slot_index == 0);
    GLIFI_REQUIRE(file.selected_commit().commit.commit_generation == 3);
    GLIFI_REQUIRE(file.selected_commit().commit.record_count == 2);

    const auto scanned = file.scan_committed();
    GLIFI_REQUIRE(scanned.has_value());
    GLIFI_REQUIRE(scanned->size() == 2);
    GLIFI_REQUIRE((*scanned)[0].sequence.value == 7);
    GLIFI_REQUIRE((*scanned)[1].sequence.value == 11);
    const auto bytes = file.read_record((*scanned)[1]);
    GLIFI_REQUIRE(bytes.has_value());
    const auto decoded = glifistore::decode_record(*bytes);
    GLIFI_REQUIRE(decoded.has_value());
    GLIFI_REQUIRE(decoded->key_string() == "beta");

    GLIFI_REQUIRE(file.seal().committed());
    GLIFI_REQUIRE(file.selected_commit().commit.state == glifistore::PersistedSegmentState::sealed);
    GLIFI_REQUIRE(file.append(*second).outcome == glifistore::SegmentCommitOutcome::not_committed);

    created.file.reset();
    const auto reopened = glifistore::DurableSegmentFile::open(*directory, identity);
    GLIFI_REQUIRE(reopened.has_value());
    const auto recovered = reopened->scan_committed();
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE(recovered->size() == 2);
    GLIFI_REQUIRE(reopened->selected_commit().commit.state == glifistore::PersistedSegmentState::sealed);

    auto read_only = glifistore::DurableSegmentFile::open(*directory, identity,
                                                          glifistore::SegmentFileOpenMode::read_only);
    GLIFI_REQUIRE(read_only.has_value());
    const auto rejected_append = read_only->append(*second);
    GLIFI_REQUIRE(rejected_append.outcome == glifistore::SegmentCommitOutcome::not_committed);
    GLIFI_REQUIRE(rejected_append.error.has_value());
    GLIFI_REQUIRE(rejected_append.error->code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("generation-pinned runtime read accepts a Record committed after handle open") {
    SegmentTemporaryDirectory temporary;
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    const auto identity = segment_identity();
    auto created = glifistore::DurableSegmentFile::create(*directory, identity);
    GLIFI_REQUIRE(created.durable());
    auto reader = glifistore::DurableSegmentFile::open(*directory, identity,
                                                       glifistore::SegmentFileOpenMode::read_only);
    GLIFI_REQUIRE(reader.has_value());

    const auto record = encoded_record(7, "active-key", "active-value");
    GLIFI_REQUIRE(record.has_value());
    const auto offset = created.file->selected_commit().commit.committed_end;
    GLIFI_REQUIRE(created.file->append(*record).committed());
    const glifistore::RecordRef reference{
        .segment_id = identity.segment_id,
        .offset = glifistore::RecordOffset{offset},
        .size = glifistore::RecordSize{static_cast<std::uint32_t>(record->size())},
        .sequence = glifistore::SequenceNumber{7},
        .generation = identity.generation};
    const auto visitor = [](void* context, const glifistore::RecordView& view) -> glifistore::Status {
        *static_cast<std::string*>(context) =
            std::string{reinterpret_cast<const char*>(view.value.data()), view.value.size()};
        return {};
    };
    std::string value;
    GLIFI_REQUIRE(!reader->visit_record(reference, &value, visitor).has_value());
    GLIFI_REQUIRE(reader->visit_runtime_record(reference, &value, visitor).has_value());
    GLIFI_REQUIRE(value == "active-value");

    std::vector<std::byte> scratch;
    scratch.reserve(record->size() * 2U);
    const auto reserved_capacity = scratch.capacity();
    value.clear();
    GLIFI_REQUIRE(reader->visit_runtime_record(reference, scratch, &value, visitor).has_value());
    GLIFI_REQUIRE(value == "active-value");
    GLIFI_REQUIRE(scratch.size() == record->size());
    GLIFI_REQUIRE(scratch.capacity() == reserved_capacity);
}

GLIFI_TEST("preallocation failure publishes no Segment and keeps directory healthy") {
    SegmentTemporaryDirectory temporary;
    SegmentInjectedFailure failure{.operation = glifistore::FilesystemOperation::preallocate_segment,
                                   .enabled = true};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), {.context = &failure, .before = &fail_segment_operation});
    GLIFI_REQUIRE(directory.has_value());
    const auto identity = segment_identity();
    const auto created = glifistore::DurableSegmentFile::create(*directory, identity);
    GLIFI_REQUIRE(created.outcome == glifistore::SegmentFileCreationOutcome::not_published);
    GLIFI_REQUIRE(directory->healthy());
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::segment_filename(identity)));
    const auto temporary_name = '.' + glifistore::segment_filename(identity) + ".tmp";
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / temporary_name));
}

GLIFI_TEST("Segment creation fault matrix distinguishes pre and post rename failures") {
    static constexpr std::array prepublication_boundaries{
        glifistore::FilesystemOperation::preallocate_segment,
        glifistore::FilesystemOperation::write_segment_header,
        glifistore::FilesystemOperation::sync_segment_file,
        glifistore::FilesystemOperation::rename_segment,
    };
    for (const auto boundary : prepublication_boundaries) {
        SegmentTemporaryDirectory temporary;
        SegmentInjectedFailure failure{.operation = boundary, .enabled = true};
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), {.context = &failure, .before = &fail_segment_operation});
        GLIFI_REQUIRE(directory.has_value());
        const auto identity = segment_identity();
        const auto created = glifistore::DurableSegmentFile::create(*directory, identity);
        GLIFI_REQUIRE(created.outcome == glifistore::SegmentFileCreationOutcome::not_published);
        GLIFI_REQUIRE(created.error.has_value());
        GLIFI_REQUIRE(directory->healthy());
        GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::segment_filename(identity)));
        GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() /
                                               ('.' + glifistore::segment_filename(identity) + ".tmp")));
    }

    SegmentTemporaryDirectory temporary;
    SegmentInjectedFailure failure{.operation = glifistore::FilesystemOperation::sync_directory,
                                   .enabled = true};
    const auto identity = segment_identity();
    {
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), {.context = &failure, .before = &fail_segment_operation});
        GLIFI_REQUIRE(directory.has_value());
        const auto created = glifistore::DurableSegmentFile::create(*directory, identity);
        GLIFI_REQUIRE(created.outcome == glifistore::SegmentFileCreationOutcome::indeterminate);
        GLIFI_REQUIRE(created.error.has_value());
        GLIFI_REQUIRE(!directory->healthy());
    }
    auto reopened = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(glifistore::DurableSegmentFile::open(*reopened, identity).has_value());
}

GLIFI_TEST("fault boundaries distinguish uncommitted Record tails from indeterminate slots") {
    SegmentTemporaryDirectory temporary;
    SegmentInjectedFailure failure{};
    const auto identity = segment_identity();
    {
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(), {.context = &failure, .before = &fail_segment_operation});
        GLIFI_REQUIRE(directory.has_value());
        auto created = glifistore::DurableSegmentFile::create(*directory, identity);
        GLIFI_REQUIRE(created.durable());
        auto& file = *created.file;
        const auto record = encoded_record(4, "key", "value");
        GLIFI_REQUIRE(record.has_value());

        failure.operation = glifistore::FilesystemOperation::write_commit_slot;
        failure.enabled = true;
        const auto before_slot = file.append(*record);
        GLIFI_REQUIRE(before_slot.outcome == glifistore::SegmentCommitOutcome::not_committed);
        GLIFI_REQUIRE(file.healthy());
        GLIFI_REQUIRE(file.selected_commit().commit.record_count == 0);

        failure.enabled = false;
        created.file.reset();
        auto reopened = glifistore::DurableSegmentFile::open(*directory, identity);
        GLIFI_REQUIRE(reopened.has_value());
        GLIFI_REQUIRE(reopened->selected_commit().commit.record_count == 0);
        const auto empty_scan = reopened->scan_committed();
        GLIFI_REQUIRE(empty_scan.has_value());
        GLIFI_REQUIRE(empty_scan->empty());
        GLIFI_REQUIRE(reopened->append(*record).committed());
        const auto next = encoded_record(5, "next", "record");
        GLIFI_REQUIRE(next.has_value());
        failure.operation = glifistore::FilesystemOperation::sync_commit_slot;
        failure.enabled = true;
        const auto after_slot = reopened->append(*next);
        GLIFI_REQUIRE(after_slot.outcome == glifistore::SegmentCommitOutcome::indeterminate);
        GLIFI_REQUIRE(!reopened->healthy());
        GLIFI_REQUIRE(!directory->healthy());
        GLIFI_REQUIRE(!reopened->scan_committed().has_value());
        GLIFI_REQUIRE(!glifistore::DurableSegmentFile::open(*directory, identity).has_value());
        const auto blocked_create =
            glifistore::DurableSegmentFile::create(*directory, segment_identity(0x13));
        GLIFI_REQUIRE(blocked_create.outcome == glifistore::SegmentFileCreationOutcome::indeterminate);
    }

    auto recovered_directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(recovered_directory.has_value());
    const auto recovered = glifistore::DurableSegmentFile::open(*recovered_directory, identity);
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE(recovered->selected_commit().commit.record_count >= 1);
    GLIFI_REQUIRE(recovered->selected_commit().commit.record_count <= 2);
    GLIFI_REQUIRE(recovered->scan_committed().has_value());
}

GLIFI_TEST("committed Record corruption is detected by recovery scan") {
    SegmentTemporaryDirectory temporary;
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    const auto identity = segment_identity();
    auto created = glifistore::DurableSegmentFile::create(*directory, identity);
    GLIFI_REQUIRE(created.durable());
    const auto record = encoded_record(9, "checksum", "protected");
    GLIFI_REQUIRE(record.has_value());
    GLIFI_REQUIRE(created.file->append(*record).committed());

    const auto path = temporary.path() / glifistore::segment_filename(identity);
    glifistore::FileDescriptor raw{::open(path.c_str(), O_RDWR | O_CLOEXEC)};
    GLIFI_REQUIRE(raw.valid());
    const std::byte corrupt{0x00};
    GLIFI_REQUIRE(
        raw.write_all_at(std::span{&corrupt, 1}, glifistore::kSegmentHeaderReservedBytes).has_value());
    GLIFI_REQUIRE(raw.sync(glifistore::FileSyncMode::data).has_value());

    const auto scan = created.file->scan_committed();
    GLIFI_REQUIRE(!scan.has_value());
    GLIFI_REQUIRE(scan.error().code == glifistore::ErrorCode::invalid_record ||
                  scan.error().code == glifistore::ErrorCode::checksum_mismatch);
}

GLIFI_TEST("Segment handles fail closed when their data directory lifetime ends") {
    SegmentTemporaryDirectory temporary;
    std::optional<glifistore::DurableSegmentFile> surviving;
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto created = glifistore::DurableSegmentFile::create(*directory, segment_identity());
        GLIFI_REQUIRE(created.durable());
        surviving = std::move(*created.file);
        GLIFI_REQUIRE(surviving->healthy());
    }

    GLIFI_REQUIRE(!surviving->healthy());
    const auto record = encoded_record(1, "closed", "directory");
    GLIFI_REQUIRE(record.has_value());
    GLIFI_REQUIRE(surviving->append(*record).outcome == glifistore::SegmentCommitOutcome::indeterminate);
    GLIFI_REQUIRE(glifistore::DataDirectory::open_and_lock(temporary.path()).has_value());
}

GLIFI_TEST("deferred append defers synchronization until sync_file") {
    SegmentTemporaryDirectory temporary;

    struct SyncCounter {
        std::size_t sync_record_calls{};
        std::size_t sync_commit_slot_calls{};
    } counter;

    const auto hooks = glifistore::FilesystemHooks{
        .context = &counter,
        .before = [](void* context, const glifistore::FilesystemOperation operation) -> glifistore::Status {
            if (operation == glifistore::FilesystemOperation::sync_record) {
                ++static_cast<SyncCounter*>(context)->sync_record_calls;
            }
            if (operation == glifistore::FilesystemOperation::sync_commit_slot) {
                ++static_cast<SyncCounter*>(context)->sync_commit_slot_calls;
            }
            return {};
        },
    };
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path(), hooks);
    GLIFI_REQUIRE(directory.has_value());
    const auto identity = segment_identity();
    auto created = glifistore::DurableSegmentFile::create(*directory, identity);
    GLIFI_REQUIRE(created.durable());
    const auto record = encoded_record(1, "periodic", "value");
    GLIFI_REQUIRE(record.has_value());
    GLIFI_REQUIRE(created.file->append(*record, glifistore::SegmentCommitSync::deferred).committed());
    GLIFI_REQUIRE(created.file->is_dirty());
    GLIFI_REQUIRE(counter.sync_record_calls == 1);
    GLIFI_REQUIRE(counter.sync_commit_slot_calls == 0);
    GLIFI_REQUIRE(created.file->sync_file().committed());
    GLIFI_REQUIRE(!created.file->is_dirty());
    GLIFI_REQUIRE(counter.sync_commit_slot_calls == 1);

    created.file.reset();
    auto reopened = glifistore::DurableSegmentFile::open(*directory, identity);
    GLIFI_REQUIRE(reopened.has_value());
    const auto scan = reopened->scan_committed();
    GLIFI_REQUIRE(scan.has_value());
    GLIFI_REQUIRE(scan->size() == 1);
}

GLIFI_TEST("batched append_record defers commit slot until flush_pending_commit") {
    SegmentTemporaryDirectory temporary;

    struct SlotCounter {
        std::size_t sync_record_calls{};
        std::size_t write_commit_slot_calls{};
        std::size_t sync_commit_slot_calls{};
    } counter;

    const auto hooks = glifistore::FilesystemHooks{
        .context = &counter,
        .before = [](void* context, const glifistore::FilesystemOperation operation) -> glifistore::Status {
            auto& counts = *static_cast<SlotCounter*>(context);
            if (operation == glifistore::FilesystemOperation::sync_record) {
                ++counts.sync_record_calls;
            }
            if (operation == glifistore::FilesystemOperation::write_commit_slot) {
                ++counts.write_commit_slot_calls;
            }
            if (operation == glifistore::FilesystemOperation::sync_commit_slot) {
                ++counts.sync_commit_slot_calls;
            }
            return {};
        },
    };
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path(), hooks);
    GLIFI_REQUIRE(directory.has_value());
    const auto identity = segment_identity();
    auto created = glifistore::DurableSegmentFile::create(*directory, identity);
    GLIFI_REQUIRE(created.durable());
    auto& file = *created.file;

    const auto first = encoded_record(1, "one", "alpha");
    const auto second = encoded_record(2, "two", "beta");
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE(second.has_value());
    GLIFI_REQUIRE(file.append_record(*first).committed());
    GLIFI_REQUIRE(file.append_record(*second).committed());
    GLIFI_REQUIRE(file.has_pending_commit());
    GLIFI_REQUIRE(counter.sync_record_calls == 0);
    GLIFI_REQUIRE(counter.write_commit_slot_calls == 0);
    GLIFI_REQUIRE(counter.sync_commit_slot_calls == 0);

    GLIFI_REQUIRE(file.flush_pending_commit(glifistore::SegmentCommitSync::immediate).committed());
    GLIFI_REQUIRE(counter.sync_record_calls == 1);
    GLIFI_REQUIRE(counter.write_commit_slot_calls == 1);
    GLIFI_REQUIRE(counter.sync_commit_slot_calls == 1);
    GLIFI_REQUIRE(file.selected_commit().commit.record_count == 2);
    GLIFI_REQUIRE(file.selected_commit().commit.commit_generation == 2);

    created.file.reset();
    const auto reopened = glifistore::DurableSegmentFile::open(*directory, identity);
    GLIFI_REQUIRE(reopened.has_value());
    const auto scan = reopened->scan_committed();
    GLIFI_REQUIRE(scan.has_value());
    GLIFI_REQUIRE(scan->size() == 2);
}

GLIFI_TEST("inspect_durable_segment validates a sealed path without a directory lock") {
    SegmentTemporaryDirectory temporary;
    const auto identity = segment_identity(0x42, 7);
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto created = glifistore::DurableSegmentFile::create(*directory, identity);
        GLIFI_REQUIRE(created.durable());
        const auto record = encoded_record(1, "inspect", "ok");
        GLIFI_REQUIRE(record.has_value());
        GLIFI_REQUIRE(created.file->append(*record).committed());
        GLIFI_REQUIRE(created.file->seal().committed());
    }

    const auto path = temporary.path() / glifistore::segment_filename(identity);
    const auto report = glifistore::inspect_durable_segment(path);
    GLIFI_REQUIRE(report.has_value());
    GLIFI_REQUIRE(report->identity == identity);
    GLIFI_REQUIRE(report->selected.commit.state == glifistore::PersistedSegmentState::sealed);
    GLIFI_REQUIRE(report->selected.commit.record_count == 1);
    GLIFI_REQUIRE(report->scanned_records == 1);
    GLIFI_REQUIRE(report->filename_matches_identity.has_value());
    GLIFI_REQUIRE(*report->filename_matches_identity);

    const auto header_only = glifistore::inspect_durable_segment(path, false);
    GLIFI_REQUIRE(header_only.has_value());
    GLIFI_REQUIRE(header_only->scanned_records == 1);
}

GLIFI_TEST("inspect_durable_segment fails closed on committed Record corruption") {
    SegmentTemporaryDirectory temporary;
    const auto identity = segment_identity(0x43, 1);
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto created = glifistore::DurableSegmentFile::create(*directory, identity);
        GLIFI_REQUIRE(created.durable());
        const auto record = encoded_record(3, "corrupt", "me");
        GLIFI_REQUIRE(record.has_value());
        GLIFI_REQUIRE(created.file->append(*record).committed());
    }

    const auto path = temporary.path() / glifistore::segment_filename(identity);
    glifistore::FileDescriptor raw{::open(path.c_str(), O_RDWR | O_CLOEXEC)};
    GLIFI_REQUIRE(raw.valid());
    const std::byte corrupt{0x00};
    GLIFI_REQUIRE(
        raw.write_all_at(std::span{&corrupt, 1}, glifistore::kSegmentHeaderReservedBytes).has_value());
    GLIFI_REQUIRE(raw.sync(glifistore::FileSyncMode::data).has_value());
    raw.reset();

    const auto report = glifistore::inspect_durable_segment(path);
    GLIFI_REQUIRE(!report.has_value());
    GLIFI_REQUIRE(report.error().code == glifistore::ErrorCode::invalid_record ||
                  report.error().code == glifistore::ErrorCode::checksum_mismatch);

    const auto header_only = glifistore::inspect_durable_segment(path, false);
    GLIFI_REQUIRE(header_only.has_value());
}

GLIFI_TEST("inspect_durable_segment rejects filename identity disagreement") {
    SegmentTemporaryDirectory temporary;
    const auto identity = segment_identity(0x44, 2);
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto created = glifistore::DurableSegmentFile::create(*directory, identity);
        GLIFI_REQUIRE(created.durable());
    }

    const auto original = temporary.path() / glifistore::segment_filename(identity);
    const auto mismatched = temporary.path() / glifistore::segment_filename(segment_identity(0x99, 9));
    std::error_code ec;
    std::filesystem::rename(original, mismatched, ec);
    GLIFI_REQUIRE(!ec);

    const auto report = glifistore::inspect_durable_segment(mismatched);
    GLIFI_REQUIRE(!report.has_value());
    GLIFI_REQUIRE(report.error().code == glifistore::ErrorCode::corrupted_data);
}
