#include "glifistore/core/little_endian.hpp"
#include "glifistore/persistence/namespace_audit.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/record.hpp"
#include "segment_file_detail.hpp"
#include "system_error.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace glifistore {

using le::get_u32;
using segment_file_detail::commit_failure;

auto DurableSegmentFile::publish_commit(const SegmentCommit& commit, const SegmentCommitSync sync)
    -> SegmentCommitResult {
    std::array<std::byte, kSegmentCommitSlotBytes> encoded{};
    if (auto valid = encode_segment_commit_slot(encoded, commit); !valid) {
        rollback_pending_metadata();
        return commit_failure(SegmentCommitOutcome::not_committed, valid.error());
    }
    if (commit.commit_generation != persisted_.commit.commit_generation + 1) {
        rollback_pending_metadata();
        return commit_failure(
            SegmentCommitOutcome::not_committed,
            Error{ErrorCode::sequence_conflict, "Segment commit generation must advance by exactly one"});
    }

    const auto next_slot = (persisted_.slot_index + 1) % kSegmentCommitSlotCount;
    const auto slot_offset = kSegmentCommitSlotsOffset + next_slot * kSegmentCommitSlotBytes;
    if (auto allowed = before(FilesystemOperation::write_commit_slot); !allowed) {
        rollback_pending_metadata();
        return commit_failure(SegmentCommitOutcome::not_committed, allowed.error());
    }
    if (auto written = file_.write_all_at(encoded, slot_offset); !written) {
        rollback_pending_metadata();
        poison();
        return commit_failure(SegmentCommitOutcome::indeterminate, written.error());
    }
    after(FilesystemOperation::write_commit_slot);
    selected_ = {.slot_index = next_slot, .commit = commit};
    persisted_ = selected_;
    pending_record_count_ = 0;
    pending_bytes_ = 0;
    if (sync == SegmentCommitSync::deferred) {
        dirty_ = true;
        return {.outcome = SegmentCommitOutcome::committed, .error = std::nullopt};
    }
    if (auto allowed = before(FilesystemOperation::sync_commit_slot); !allowed) {
        poison();
        return commit_failure(SegmentCommitOutcome::indeterminate, allowed.error());
    }
    if (auto synced = file_.sync(FileSyncMode::data); !synced) {
        poison();
        return commit_failure(SegmentCommitOutcome::indeterminate, synced.error());
    }
    after(FilesystemOperation::sync_commit_slot);
    dirty_ = false;
    return {.outcome = SegmentCommitOutcome::committed, .error = std::nullopt};
}

auto DurableSegmentFile::sync_file() -> SegmentCommitResult {
    if (!healthy()) {
        return commit_failure(SegmentCommitOutcome::indeterminate,
                              Error{ErrorCode::io_error, "Segment file is poisoned"});
    }
    if (!dirty_) {
        return {.outcome = SegmentCommitOutcome::committed, .error = std::nullopt};
    }
    if (auto allowed = before(FilesystemOperation::sync_commit_slot); !allowed) {
        poison();
        return commit_failure(SegmentCommitOutcome::indeterminate, allowed.error());
    }
    if (auto synced = file_.sync(FileSyncMode::data); !synced) {
        poison();
        return commit_failure(SegmentCommitOutcome::indeterminate, synced.error());
    }
    after(FilesystemOperation::sync_commit_slot);
    dirty_ = false;
    return {.outcome = SegmentCommitOutcome::committed, .error = std::nullopt};
}

auto DurableSegmentFile::is_dirty() const noexcept -> bool {
    return dirty_;
}

auto DurableSegmentFile::has_pending_commit() const noexcept -> bool {
    return pending_record_count_ > 0;
}

auto DurableSegmentFile::pending_record_count() const noexcept -> std::uint64_t {
    return pending_record_count_;
}

auto DurableSegmentFile::pending_bytes() const noexcept -> std::uint64_t {
    return pending_bytes_;
}

auto DurableSegmentFile::append_record(const std::span<const std::byte> encoded_record,
                                       const SegmentRecordWritePacing pacing) -> SegmentCommitResult {
    if (!healthy()) {
        return commit_failure(SegmentCommitOutcome::indeterminate,
                              Error{ErrorCode::io_error, "Segment file is poisoned"});
    }
    if (!writable_) {
        return commit_failure(
            SegmentCommitOutcome::not_committed,
            Error{ErrorCode::invalid_argument, "cannot append through a read-only Segment handle"});
    }
    if (selected_.commit.state != PersistedSegmentState::active) {
        return commit_failure(SegmentCommitOutcome::not_committed,
                              Error{ErrorCode::segment_sealed, "cannot append to a sealed Segment"});
    }
    const auto record = decode_record(encoded_record);
    if (!record) {
        return commit_failure(SegmentCommitOutcome::not_committed, record.error());
    }
    if (record->format_version != identity_.format_version) {
        return commit_failure(SegmentCommitOutcome::not_committed,
                              Error{ErrorCode::invalid_record, "Record/Segment version mismatch"});
    }
    if (record->sequence.value == 0 || (selected_.commit.record_count != 0 &&
                                        record->sequence.value <= selected_.commit.last_sequence.value)) {
        return commit_failure(
            SegmentCommitOutcome::not_committed,
            Error{ErrorCode::sequence_conflict, "Record sequence must increase strictly within a Segment"});
    }
    if (selected_.commit.record_count == std::numeric_limits<std::uint64_t>::max() ||
        persisted_.commit.commit_generation == std::numeric_limits<std::uint64_t>::max()) {
        return commit_failure(SegmentCommitOutcome::not_committed,
                              Error{ErrorCode::arithmetic_overflow, "Segment commit metadata overflow"});
    }
    const auto remaining = static_cast<std::uint64_t>(kSegmentSizeBytes) - selected_.commit.committed_end;
    if (encoded_record.size() > remaining) {
        return commit_failure(SegmentCommitOutcome::not_committed,
                              Error{ErrorCode::segment_full, "Record does not fit in the Segment"});
    }

    const auto record_offset = selected_.commit.committed_end;
    if (auto allowed = before(FilesystemOperation::write_record); !allowed) {
        return commit_failure(SegmentCommitOutcome::not_committed, allowed.error());
    }
    std::size_t written_bytes{};
    while (written_bytes < encoded_record.size()) {
        const auto remaining_record_bytes = encoded_record.size() - written_bytes;
        auto write_bytes = remaining_record_bytes;
        if (pacing.acquire != nullptr) {
            auto granted = pacing.acquire(pacing.context, remaining_record_bytes);
            if (!granted) {
                return commit_failure(SegmentCommitOutcome::not_committed, granted.error());
            }
            if (*granted == 0U || *granted > remaining_record_bytes) {
                return commit_failure(
                    SegmentCommitOutcome::not_committed,
                    Error{ErrorCode::internal_error, "Segment Record write pacer returned an invalid grant"});
            }
            write_bytes = *granted;
        }
        if (auto written = file_.write_all_at(encoded_record.subspan(written_bytes, write_bytes),
                                              record_offset + written_bytes);
            !written) {
            poison();
            return commit_failure(SegmentCommitOutcome::not_committed, written.error());
        }
        written_bytes += write_bytes;
    }
    after(FilesystemOperation::write_record);

    const auto first_sequence =
        selected_.commit.record_count == 0 ? record->sequence : selected_.commit.first_sequence;
    selected_.commit.committed_end = static_cast<std::uint32_t>(record_offset + encoded_record.size());
    selected_.commit.record_count += 1;
    selected_.commit.first_sequence = first_sequence;
    selected_.commit.last_sequence = record->sequence;
    ++pending_record_count_;
    pending_bytes_ += encoded_record.size();
    dirty_ = true;
    return {.outcome = SegmentCommitOutcome::committed, .error = std::nullopt};
}

auto DurableSegmentFile::flush_pending_commit(const SegmentCommitSync sync) -> SegmentCommitResult {
    if (!has_pending_commit()) {
        if (dirty_ && sync == SegmentCommitSync::immediate) {
            return sync_file();
        }
        return {.outcome = SegmentCommitOutcome::committed, .error = std::nullopt};
    }
    if (auto allowed = before(FilesystemOperation::sync_record); !allowed) {
        rollback_pending_metadata();
        return commit_failure(SegmentCommitOutcome::not_committed, allowed.error());
    }
    if (auto synced = file_.sync(FileSyncMode::ordered); !synced) {
        rollback_pending_metadata();
        poison();
        return commit_failure(SegmentCommitOutcome::not_committed, synced.error());
    }
    after(FilesystemOperation::sync_record);
    SegmentCommit next = selected_.commit;
    next.commit_generation = persisted_.commit.commit_generation + 1;
    return publish_commit(next, sync);
}

auto DurableSegmentFile::append(const std::span<const std::byte> encoded_record, const SegmentCommitSync sync)
    -> SegmentCommitResult {
    auto appended = append_record(encoded_record);
    if (!appended.committed()) {
        return appended;
    }
    return flush_pending_commit(sync);
}

auto DurableSegmentFile::seal() -> SegmentCommitResult {
    if (!healthy()) {
        return commit_failure(SegmentCommitOutcome::indeterminate,
                              Error{ErrorCode::io_error, "Segment file is poisoned"});
    }
    if (!writable_) {
        return commit_failure(
            SegmentCommitOutcome::not_committed,
            Error{ErrorCode::invalid_argument, "cannot seal through a read-only Segment handle"});
    }
    if (selected_.commit.state == PersistedSegmentState::sealed) {
        return commit_failure(SegmentCommitOutcome::not_committed,
                              Error{ErrorCode::segment_sealed, "Segment is already sealed"});
    }
    if (selected_.commit.commit_generation == std::numeric_limits<std::uint64_t>::max()) {
        return commit_failure(SegmentCommitOutcome::not_committed,
                              Error{ErrorCode::arithmetic_overflow, "Segment commit generation overflow"});
    }
    if (has_pending_commit()) {
        const auto flushed = flush_pending_commit(SegmentCommitSync::immediate);
        if (!flushed.committed()) {
            return flushed;
        }
    }
    auto sealed = selected_.commit;
    ++sealed.commit_generation;
    sealed.state = PersistedSegmentState::sealed;
    return publish_commit(sealed, SegmentCommitSync::immediate);
}

auto DurableSegmentFile::visit_committed_records(void* context, const CommittedRecordVisitor visitor) const
    -> Status {
    if (!healthy()) {
        return fail(ErrorCode::io_error, "cannot scan a poisoned Segment file");
    }
    if (!visitor) {
        return fail(ErrorCode::invalid_argument, "committed Record visitor cannot be null");
    }
    const auto data_size =
        static_cast<std::size_t>(persisted_.commit.committed_end) - kSegmentHeaderReservedBytes;
    if (persisted_.commit.record_count == 0) {
        return {};
    }
    const auto maximum_records = data_size / kEncodedRecordHeaderSize;
    if (persisted_.commit.record_count > maximum_records) {
        return fail(ErrorCode::corrupted_data, "Segment commit Record count exceeds its extent");
    }

    std::vector<std::byte> bytes(data_size);
    if (auto read = file_.read_exact_at(bytes, kSegmentHeaderReservedBytes); !read) {
        return unexpected(read.error());
    }
    std::size_t cursor{};
    std::uint64_t record_count{};
    SequenceNumber first{};
    SequenceNumber previous{};
    while (cursor < bytes.size()) {
        const auto remaining = bytes.size() - cursor;
        if (remaining < kEncodedRecordHeaderSize) {
            return fail(ErrorCode::corrupted_data, "committed Segment extent ends in a truncated Record");
        }
        const auto encoded_size = get_u32(bytes, cursor + 8);
        if (encoded_size < kEncodedRecordHeaderSize || encoded_size > kMaxNormalRecordSize ||
            encoded_size % kRecordAlignment != 0 || encoded_size > remaining) {
            return fail(ErrorCode::corrupted_data, "committed Segment contains an invalid Record extent");
        }
        const auto record = decode_record(std::span<const std::byte>{bytes}.subspan(cursor, encoded_size));
        if (!record) {
            return unexpected(record.error());
        }
        if (record->format_version != identity_.format_version) {
            return fail(ErrorCode::invalid_record, "Record/Segment version mismatch");
        }
        if (record->sequence.value == 0 ||
            (previous.value != 0 && record->sequence.value <= previous.value)) {
            return fail(ErrorCode::corrupted_data,
                        "committed Segment Record sequences are not strictly increasing");
        }
        const RecordRef reference{
            .segment_id = identity_.segment_id,
            .offset = RecordOffset{static_cast<std::uint32_t>(kSegmentHeaderReservedBytes + cursor)},
            .size = RecordSize{encoded_size},
            .sequence = record->sequence,
            .generation = identity_.generation,
        };
        if (auto visited = visitor(context, reference, *record); !visited) {
            return unexpected(visited.error());
        }
        if (record_count == 0) {
            first = record->sequence;
        }
        ++record_count;
        previous = record->sequence;
        cursor += encoded_size;
    }
    if (record_count != persisted_.commit.record_count || first != persisted_.commit.first_sequence ||
        previous != persisted_.commit.last_sequence) {
        return fail(ErrorCode::corrupted_data, "Segment commit metadata does not match committed Records");
    }
    return {};
}

auto DurableSegmentFile::scan_committed() const -> Result<std::vector<RecordRef>> {
    std::vector<RecordRef> records;
    const auto data_size =
        static_cast<std::size_t>(persisted_.commit.committed_end) - kSegmentHeaderReservedBytes;
    if (persisted_.commit.record_count > data_size / kEncodedRecordHeaderSize) {
        return fail(ErrorCode::corrupted_data, "Segment commit Record count exceeds its extent");
    }
    records.reserve(static_cast<std::size_t>(persisted_.commit.record_count));
    const auto collect = [](void* context, const RecordRef& reference, const RecordView&) -> Status {
        static_cast<std::vector<RecordRef>*>(context)->push_back(reference);
        return {};
    };
    if (auto scanned = visit_committed_records(&records, collect); !scanned) {
        return unexpected(scanned.error());
    }
    return Result<std::vector<RecordRef>>{std::move(records)};
}

auto DurableSegmentFile::read_record_into(const RecordRef& reference, std::vector<std::byte>& bytes,
                                          RecordView& record) const -> Status {
    return read_record_into_extent(reference, persisted_.commit.committed_end, bytes, record);
}

auto DurableSegmentFile::read_record_into_extent(const RecordRef& reference, const std::uint64_t readable_end,
                                                 std::vector<std::byte>& bytes, RecordView& record) const
    -> Status {
    if (!healthy()) {
        return fail(ErrorCode::io_error, "cannot read a poisoned Segment file");
    }
    if (reference.segment_id != identity_.segment_id || reference.generation != identity_.generation) {
        return fail(ErrorCode::invalid_reference, "Record reference targets a different Segment generation");
    }
    const auto offset = static_cast<std::uint64_t>(reference.offset.value);
    const auto size = static_cast<std::uint64_t>(reference.size.value);
    if (offset < kSegmentHeaderReservedBytes || offset % kRecordAlignment != 0 ||
        size < kEncodedRecordHeaderSize || size > kMaxNormalRecordSize || size % kRecordAlignment != 0 ||
        readable_end > kSegmentSizeBytes || offset > readable_end || size > readable_end - offset) {
        return fail(ErrorCode::invalid_reference, "Record reference is outside the committed Segment extent");
    }
    bytes.resize(reference.size.value);
    if (auto read = file_.read_exact_at(bytes, offset); !read) {
        return unexpected(read.error());
    }
    const auto decoded = decode_record(bytes);
    if (!decoded) {
        return unexpected(decoded.error());
    }
    if (decoded->format_version != identity_.format_version) {
        return fail(ErrorCode::invalid_record, "Record/Segment version mismatch");
    }
    if (decoded->sequence != reference.sequence || decoded->encoded_size != reference.size.value) {
        return fail(ErrorCode::invalid_reference, "Record reference metadata does not match encoded data");
    }
    record = *decoded;
    return {};
}

auto DurableSegmentFile::read_record(const RecordRef& reference) const -> Result<std::vector<std::byte>> {
    std::vector<std::byte> bytes;
    RecordView record{};
    if (auto read = read_record_into(reference, bytes, record); !read) {
        return unexpected(read.error());
    }
    return Result<std::vector<std::byte>>{std::move(bytes)};
}

auto DurableSegmentFile::visit_record(const RecordRef& reference, void* context,
                                      const RecordVisitor visitor) const -> Status {
    std::vector<std::byte> scratch;
    return visit_record(reference, scratch, context, visitor);
}

auto DurableSegmentFile::visit_record(const RecordRef& reference, std::vector<std::byte>& scratch,
                                      void* context, const RecordVisitor visitor) const -> Status {
    if (!visitor) {
        return fail(ErrorCode::invalid_argument, "Record visitor cannot be null");
    }
    RecordView record{};
    if (auto read = read_record_into(reference, scratch, record); !read) {
        return unexpected(read.error());
    }
    return visitor(context, record);
}

auto DurableSegmentFile::visit_runtime_record(const RecordRef& reference, void* context,
                                              const RecordVisitor visitor) const -> Status {
    std::vector<std::byte> scratch;
    return visit_runtime_record(reference, scratch, context, visitor);
}

auto DurableSegmentFile::visit_runtime_record(const RecordRef& reference, std::vector<std::byte>& scratch,
                                              void* context, const RecordVisitor visitor) const -> Status {
    if (!visitor) {
        return fail(ErrorCode::invalid_argument, "Record visitor cannot be null");
    }
    RecordView record{};
    if (auto read = read_record_into_extent(reference, kSegmentSizeBytes, scratch, record); !read) {
        return unexpected(read.error());
    }
    return visitor(context, record);
}

auto inspect_durable_segment(const std::filesystem::path& path, const bool scan_records)
    -> Result<DurableSegmentInspectReport> {
    auto opened = DurableSegmentFile::open_path(path);
    if (!opened) {
        return unexpected(opened.error());
    }

    DurableSegmentInspectReport report{
        .path = path,
        .identity = opened->identity(),
        .selected = opened->selected_commit(),
    };

    const auto basename = path.filename().string();
    if (const auto parsed = parse_segment_filename(basename); parsed) {
        const bool matches = parsed->segment_id == report.identity.segment_id &&
                             parsed->generation == report.identity.generation && !parsed->temporary;
        if (!matches) {
            return fail(ErrorCode::corrupted_data,
                        "Segment filename identity disagrees with Segment header identity");
        }
        report.filename_matches_identity = true;
    }

    if (scan_records) {
        std::uint64_t counted{};
        const auto count = [](void* context, const RecordRef&, const RecordView&) -> Status {
            ++*static_cast<std::uint64_t*>(context);
            return {};
        };
        if (auto scanned = opened->visit_committed_records(&counted, count); !scanned) {
            return unexpected(scanned.error());
        }
        report.scanned_records = counted;
    } else {
        report.scanned_records = report.selected.commit.record_count;
    }
    return report;
}

} // namespace glifistore
