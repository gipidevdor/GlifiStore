#include "glifistore/index/index.hpp"

#include "glifistore/segment/record.hpp"

#include <unordered_map>
#include <utility>

namespace glifistore {
namespace {

struct LatestRecord {
    RecordRef ref;
    bool deleted{};
    bool expired{};
};

} // namespace

Index::Index() = default;

Index::Index(const std::uint64_t seed) : table_(seed) {}

Index::Index(const WorkerRoutingState routing) : table_(routing) {}

Index::Index(const WorkerRoutingState routing, const std::uint64_t seed) : table_(routing, seed) {}

Index::Index(SwissTableIndex table) : table_(std::move(table)) {}

Index::~Index() = default;
Index::Index(Index&&) noexcept = default;
auto Index::operator=(Index&&) noexcept -> Index& = default;

auto Index::seed() const noexcept -> std::uint64_t {
    return table_.seed();
}

auto Index::find(const std::string_view key) const -> std::optional<RecordRef> {
    return table_.find(key);
}

auto Index::find(const HashedKey& key) const -> std::optional<RecordRef> {
    return table_.find(key);
}

auto Index::insert_or_assign(const std::string_view key, RecordRef ref) -> Result<IndexMutationResult> {
    return table_.insert_or_assign(key, ref);
}

auto Index::insert_or_assign(const HashedKey& key, RecordRef ref) -> Result<IndexMutationResult> {
    return table_.insert_or_assign(key, ref);
}

auto Index::erase(const std::string_view key) -> IndexMutationResult {
    return table_.erase(key);
}

auto Index::erase(const HashedKey& key) -> IndexMutationResult {
    return table_.erase(key);
}

auto Index::erase_no_compact(const HashedKey& key) -> IndexMutationResult {
    return table_.erase_no_compact(key);
}

auto Index::reserve(const std::size_t count) -> Status {
    return table_.reserve(count);
}

auto Index::prepare_insert(const HashedKey& key) -> Status {
    return table_.prepare_insert(key);
}

auto Index::prepare_batch_insert(const std::size_t additional_entries,
                                 const std::size_t additional_heap_key_bytes) -> Status {
    return table_.prepare_batch_insert(additional_entries, additional_heap_key_bytes);
}

auto Index::entries() const -> std::vector<IndexEntry> {
    return table_.entries();
}

auto Index::stats() const noexcept -> IndexStats {
    return table_.stats();
}

auto Index::make_empty() const -> Index {
    return Index{table_.clone_empty()};
}

auto rebuild_index_from_segments(const std::span<const SegmentPtr> segments, const std::uint64_t now_ns,
                                 const WorkerRoutingState routing) -> Result<RebuildResult> {
    std::unordered_map<std::string, LatestRecord> latest;
    RebuildStats stats{};

    for (const auto& segment : segments) {
        auto refs = segment->scan();
        if (!refs) {
            return unexpected(refs.error());
        }
        for (const auto& ref : *refs) {
            ++stats.records_scanned;
            auto record = segment->read(ref);
            if (!record) {
                return unexpected(record.error());
            }
            const std::string key{record->key_string()};
            const auto current = latest.find(key);
            if (current != latest.end() && current->second.ref.sequence.value == record->sequence.value) {
                return fail(ErrorCode::sequence_conflict,
                            "duplicate sequence for the same key during rebuild");
            }
            if (current == latest.end() || current->second.ref.sequence.value < record->sequence.value) {
                latest.insert_or_assign(key, LatestRecord{
                                                 .ref = ref,
                                                 .deleted = record->opcode == Opcode::erase,
                                                 .expired = record->expired(now_ns),
                                             });
            }
        }
    }

    Index index{routing};
    if (auto reserved = index.reserve(latest.size()); !reserved) {
        return unexpected(reserved.error());
    }
    for (const auto& [key, record] : latest) {
        if (record.deleted) {
            ++stats.tombstones;
        } else if (record.expired) {
            ++stats.expired;
        } else {
            auto inserted = index.insert_or_assign(key, record.ref);
            if (!inserted) {
                return unexpected(inserted.error());
            }
            ++stats.records_visible;
        }
    }
    return RebuildResult{.index = std::move(index), .stats = stats};
}

} // namespace glifistore
