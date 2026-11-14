// Strict sealed-prefix durable compaction through the runtime (ADR 0044). Every transaction here replaces
// only the oldest k < n sealed Segments of the Worker. A sequential model of PUT, overwrite, ERASE and TTL
// is the oracle: it must agree with the live runtime after the transaction and with a fresh recovery, and
// no Segment outside the prefix, including the sealed Segments after it, may be touched.
#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/compaction.hpp"
#include "glifistore/segment/record.hpp"
#include "persistence_recovery_test_support.hpp"
#include "support/compaction_prefix_history.hpp"
#include "support/file_identity.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using glifistore::test::file_identity;
using glifistore::test::FileIdentity;
using namespace glifistore::test::compaction_prefix;

auto worker_zero() -> glifistore::WorkerId {
    return glifistore::WorkerId{0};
}

auto put_value(glifistore::DurableRuntimeCatalog& runtime, const std::string_view key,
               const std::string_view value) -> glifistore::DurableMutationResult {
    return runtime.put(text_bytes(key), text_bytes(value));
}

auto erase_key(glifistore::DurableRuntimeCatalog& runtime, const std::string_view key)
    -> glifistore::DurableMutationResult {
    return runtime.erase(text_bytes(key));
}

auto segment_entries(const History& history) -> std::vector<glifistore::ManifestSegmentEntry> {
    std::vector<glifistore::ManifestSegmentEntry> entries;
    for (std::size_t index = 0; index <= history.sealed_count; ++index) {
        entries.push_back({.segment_id = glifistore::SegmentId{index + 1U},
                           .generation = glifistore::GenerationId{1},
                           .owner_worker = worker_zero(),
                           .role = index < history.sealed_count ? glifistore::ManifestSegmentRole::sealed
                                                                : glifistore::ManifestSegmentRole::active});
    }
    return entries;
}

auto write_store(const std::filesystem::path& path, const History& history) -> glifistore::Manifest {
    const auto store_id = recovery_store_id();
    auto entries = segment_entries(history);
    auto directory = glifistore::DataDirectory::open_and_lock(path);
    GLIFI_REQUIRE(directory.has_value());
    std::size_t next_operation{};
    for (std::size_t index = 0; index < entries.size(); ++index) {
        auto file = create_segment(*directory, store_id, entries[index]);
        for (; next_operation < history.operations.size() &&
               history.operations[next_operation].segment == index;
             ++next_operation) {
            const auto& operation = history.operations[next_operation];
            append_record(file, next_operation + 1U, operation.key, operation.value, operation.opcode,
                          operation.expire_at_ns);
        }
        if (entries[index].role == glifistore::ManifestSegmentRole::sealed) {
            GLIFI_REQUIRE(file.seal().committed());
        }
    }
    GLIFI_REQUIRE(next_operation == history.operations.size());
    auto manifest = recovery_manifest(store_id, 1, std::move(entries));
    GLIFI_REQUIRE(directory->publish_manifest(manifest).durable());
    return manifest;
}

void require_model(glifistore::DurableRuntimeCatalog& runtime, const Model& model,
                   const std::uint64_t now_ns) {
    for (const auto& [key, expected] : model) {
        const auto found = runtime.get(key, now_ns);
        if (expected) {
            GLIFI_REQUIRE(found.has_value());
            GLIFI_REQUIRE(owned_text(*found) == *expected);
        } else {
            GLIFI_REQUIRE(!found.has_value());
            GLIFI_REQUIRE(found.error().code == glifistore::ErrorCode::not_found);
        }
    }
}

auto segment_path(const std::filesystem::path& directory, const glifistore::Manifest& manifest,
                  const glifistore::ManifestSegmentEntry& entry) -> std::filesystem::path {
    return directory / glifistore::segment_filename(segment_identity(manifest.store_id, entry));
}

// verify_index() reads every live Index key through the Segments, so a reference left pointing at a
// retired or wrong Record surfaces here with its error rather than as a bare failed requirement.
void require_index_verifies(glifistore::DurableRuntimeCatalog& runtime) {
    const auto verified = runtime.verify_index();
    if (!verified) {
        throw std::runtime_error("verify_index failed (" +
                                 std::to_string(static_cast<int>(verified.error().code)) +
                                 "): " + verified.error().message);
    }
}

void require_no_intent(const std::filesystem::path& directory) {
    GLIFI_REQUIRE(!std::filesystem::exists(directory / glifistore::kCompactionIntentFilename));
}

auto sealed_entries(const glifistore::Manifest& manifest) -> std::vector<glifistore::ManifestSegmentEntry> {
    std::vector<glifistore::ManifestSegmentEntry> sealed;
    for (const auto& entry : manifest.segments) {
        if (entry.owner_worker == worker_zero() && entry.role == glifistore::ManifestSegmentRole::sealed) {
            sealed.push_back(entry);
        }
    }
    return sealed;
}

// Files a transaction from `before` to `after` may change: only the retired sources and the created
// outputs. Everything else, including the sealed Segments after the prefix and the active Segment, must be
// byte-for-byte the same file.
void require_prefix_files(const std::filesystem::path& directory, const glifistore::Manifest& before,
                          const glifistore::Manifest& after, const glifistore::DurableCompactionShape& shape,
                          const std::map<std::uint64_t, FileIdentity>& before_identities) {
    const auto sources = sealed_entries(before);
    GLIFI_REQUIRE(shape.source_count <= sources.size());
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto& source = sources[index];
        if (index < shape.source_count) {
            GLIFI_REQUIRE(!std::filesystem::exists(segment_path(directory, before, source)));
            if (index < shape.replacement_count) {
                auto output = source;
                ++output.generation.value;
                GLIFI_REQUIRE(std::filesystem::exists(segment_path(directory, after, output)));
            }
            continue;
        }
        GLIFI_REQUIRE(file_identity(segment_path(directory, before, source)) ==
                      before_identities.at(source.segment_id.value));
    }
    for (const auto& entry : before.segments) {
        if (entry.role == glifistore::ManifestSegmentRole::active) {
            GLIFI_REQUIRE(file_identity(segment_path(directory, before, entry)) ==
                          before_identities.at(entry.segment_id.value));
        }
    }
}

struct CaseResult {
    glifistore::Manifest before;
    glifistore::Manifest after;
    glifistore::DurableCompactionResult result;
    std::optional<glifistore::DurableCompactionShape> shape;
};

// One transaction under max_copy_bytes with the full set of invariants: the model before, right after and
// after a fresh recovery; one canonical transition; only the prefix's files change; a clean namespace.
auto compact_and_verify(const History& history, const std::uint64_t max_copy_bytes) -> CaseResult {
    RecoveryTemporaryDirectory temporary;
    CaseResult out;
    out.before = write_store(temporary.path(), history);
    const auto sealed_count = sealed_entries(out.before).size();
    std::map<std::uint64_t, FileIdentity> before_identities;
    for (const auto& entry : out.before.segments) {
        before_identities.emplace(entry.segment_id.value,
                                  file_identity(segment_path(temporary.path(), out.before, entry)));
    }

    {
        auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), kOpenNowNs);
        GLIFI_REQUIRE(runtime.has_value());
        require_model(**runtime, model_at(history, kOpenNowNs), kOpenNowNs);
        out.result = (*runtime)->compact_worker(0, kCompactNowNs, max_copy_bytes);
        GLIFI_REQUIRE((*runtime)->healthy());
        out.after = (*runtime)->manifest();
        // Verify the Index before any read at kCompactNowNs: an expired GET queues a deferred reclaim that the
        // next GET drains, so verify_index() would race its own key list.
        require_index_verifies(**runtime);
        require_model(**runtime, model_at(history, kCompactNowNs), kCompactNowNs);
        GLIFI_REQUIRE((*runtime)->namespace_audit().recovery_safe());
    }

    if (out.result.compacted()) {
        const auto shape = glifistore::derive_durable_compaction_shape(out.before, out.after, worker_zero());
        GLIFI_REQUIRE(shape.has_value());
        out.shape = *shape;
        const auto canonical =
            glifistore::canonical_durable_compaction_manifest(out.before, worker_zero(), *shape);
        GLIFI_REQUIRE(canonical.has_value());
        GLIFI_REQUIRE(out.after == *canonical);
        GLIFI_REQUIRE(out.result.stats.source_segments == shape->source_count);
        GLIFI_REQUIRE(out.result.stats.retained_sealed_segments == sealed_count - shape->source_count);
        require_prefix_files(temporary.path(), out.before, out.after, *shape, before_identities);
    } else {
        // A refused attempt changes nothing: not one file, not the Manifest, no intent.
        GLIFI_REQUIRE(out.result.outcome == glifistore::DurableCompactionOutcome::not_beneficial);
        GLIFI_REQUIRE(out.after == out.before);
        for (const auto& entry : out.before.segments) {
            GLIFI_REQUIRE(file_identity(segment_path(temporary.path(), out.before, entry)) ==
                          before_identities.at(entry.segment_id.value));
        }
    }
    require_no_intent(temporary.path());

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), kCompactNowNs);
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->manifest() == out.after);
    GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
    require_index_verifies(**reopened);
    require_model(**reopened, model_at(history, kCompactNowNs), kCompactNowNs);
    return out;
}

// Counts Record payload reads (offset past the Segment header) so a test can prove that an attempt read none.
struct RecordReadCounter {
    std::atomic<std::size_t> payload_reads{};

    static auto read_some_at(void* opaque, const int descriptor, const std::span<std::byte> bytes,
                             const std::uint64_t offset) -> std::ptrdiff_t {
        auto& self = *static_cast<RecordReadCounter*>(opaque);
        if (offset >= glifistore::kSegmentHeaderReservedBytes) {
            self.payload_reads.fetch_add(1, std::memory_order_relaxed);
        }
        return ::pread(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset));
    }
};

} // namespace

GLIFI_TEST("strict-prefix compaction preserves the sequential model across PUT overwrite ERASE and TTL") {
    const auto history = closure_history();
    const auto live = live_bytes_per_segment(history, kOpenNowNs);
    // The first two sealed Segments hold five live Index references; the third adds three more.
    GLIFI_REQUIRE(live[0] > 0 && live[1] > 0 && live[2] > 0);

    const auto out = compact_and_verify(history, prefix_budget(live, 2));
    GLIFI_REQUIRE(out.result.compacted());
    const glifistore::DurableCompactionShape expected_shape{.source_count = 2, .replacement_count = 1};
    GLIFI_REQUIRE(out.shape.has_value() && *out.shape == expected_shape);
    const auto& stats = out.result.stats;
    GLIFI_REQUIRE(stats.source_segments == 2);
    GLIFI_REQUIRE(stats.retained_sealed_segments == 1);
    // Five Index references point into the prefix. The two TTL puts (one masking an older live put) have
    // expired by the compaction time and are dropped; the three others are copied and remapped.
    GLIFI_REQUIRE(stats.source_index_records_verified == 5);
    GLIFI_REQUIRE(stats.expired_records_dropped == 2);
    GLIFI_REQUIRE(stats.records_copied == 3);
    GLIFI_REQUIRE(stats.remapped_records == 3);
    GLIFI_REQUIRE(stats.expired_index_erasures == 2);
    GLIFI_REQUIRE(stats.superseded_copies == 0);
    GLIFI_REQUIRE(stats.manifest_rebases == 0);
    GLIFI_REQUIRE(out.after.manifest_generation == out.before.manifest_generation + 1U);
    GLIFI_REQUIRE(out.after.segments.size() == out.before.segments.size() - 1U);
}

GLIFI_TEST("strict-prefix compaction composes with a later whole-set transaction") {
    const auto history = closure_history();
    const auto live = live_bytes_per_segment(history, kOpenNowNs);
    RecoveryTemporaryDirectory temporary;
    const auto before = write_store(temporary.path(), history);
    auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), kOpenNowNs);
    GLIFI_REQUIRE(runtime.has_value());

    const auto first = (*runtime)->compact_worker(0, kCompactNowNs, prefix_budget(live, 2));
    GLIFI_REQUIRE(first.compacted());
    GLIFI_REQUIRE(first.stats.source_segments == 2);
    const auto middle = (*runtime)->manifest();
    require_model(**runtime, model_at(history, kCompactNowNs), kCompactNowNs);

    // The second transaction sees the first one's output (generation 2) and the retained Segment 3.
    const auto second = (*runtime)->compact_worker(0, kCompactNowNs, 0);
    GLIFI_REQUIRE(second.compacted());
    GLIFI_REQUIRE(second.stats.source_segments == 2);
    GLIFI_REQUIRE(second.stats.retained_sealed_segments == 0);
    const auto last = (*runtime)->manifest();
    const auto shape = glifistore::derive_durable_compaction_shape(middle, last, worker_zero());
    GLIFI_REQUIRE(shape.has_value());
    GLIFI_REQUIRE(shape->source_count == 2 && shape->replacement_count == 1);
    GLIFI_REQUIRE(last.manifest_generation == before.manifest_generation + 2U);
    const auto sealed = sealed_entries(last);
    GLIFI_REQUIRE(sealed.size() == 1);
    GLIFI_REQUIRE(sealed[0].segment_id == glifistore::SegmentId{1});
    GLIFI_REQUIRE(sealed[0].generation == glifistore::GenerationId{3});
    require_model(**runtime, model_at(history, kCompactNowNs), kCompactNowNs);
    require_index_verifies(**runtime);
    runtime->reset();

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), kCompactNowNs);
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->manifest() == last);
    GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
    require_model(**reopened, model_at(history, kCompactNowNs), kCompactNowNs);
}

GLIFI_TEST(
    "strict-prefix selection picks the shortest prefix among equal reclaim and bounds it by the budget") {
    // Segment 1 is entirely superseded or erased, so retiring it needs no output at all.
    History history{3};
    add_put(history, 0, "x", "x1");
    add_put(history, 0, "y", "y1");
    add_put(history, 1, "x", "x2");
    add_erase(history, 1, "y");
    add_put(history, 2, "z", "z1");
    add_put(history, 3, "w", "w1");
    const auto live = live_bytes_per_segment(history, kOpenNowNs);
    GLIFI_REQUIRE(live[0] == 0 && live[1] > 0 && live[2] > 0);

    // k=1 (zero outputs) and k=2 (one output) both reclaim exactly one Segment: the shorter prefix wins.
    const auto tie = compact_and_verify(history, prefix_budget(live, 2));
    GLIFI_REQUIRE(tie.result.compacted());
    GLIFI_REQUIRE(tie.shape.has_value());
    GLIFI_REQUIRE(tie.shape->source_count == 1 && tie.shape->replacement_count == 0);
    GLIFI_REQUIRE(tie.result.stats.source_segments == 1);
    GLIFI_REQUIRE(tie.result.stats.retained_sealed_segments == 2);
    GLIFI_REQUIRE(tie.result.stats.records_copied == 0);
    GLIFI_REQUIRE(tie.result.stats.source_index_records_verified == 0);
    GLIFI_REQUIRE(tie.after.segments.size() == tie.before.segments.size() - 1U);

    // Admitting Segment 3 as well reclaims two Segments, which beats either shorter prefix. With no budget
    // at all the same complete set is chosen.
    for (const auto budget : {prefix_budget(live, 3), std::uint64_t{0}}) {
        const auto whole = compact_and_verify(history, budget);
        GLIFI_REQUIRE(whole.result.compacted());
        GLIFI_REQUIRE(whole.shape.has_value());
        GLIFI_REQUIRE(whole.shape->source_count == 3 && whole.shape->replacement_count == 1);
        GLIFI_REQUIRE(whole.result.stats.retained_sealed_segments == 0);
        GLIFI_REQUIRE(whole.result.stats.records_copied == 2);
    }
}

GLIFI_TEST("strict-prefix selection under a copy budget keeps every prefix length reachable") {
    // Four sealed Segments of one live key each, all the same size: every longer prefix reclaims one more.
    History history{4};
    add_put(history, 0, "a", "a1");
    add_put(history, 1, "b", "b1");
    add_put(history, 2, "c", "c1");
    add_put(history, 3, "d", "d1");
    add_put(history, 4, "e", "e1");
    const auto live = live_bytes_per_segment(history, kOpenNowNs);
    GLIFI_REQUIRE(live[0] > 0 && live[0] == live[1] && live[1] == live[2] && live[2] == live[3]);

    for (const std::size_t prefix_length : {std::size_t{2}, std::size_t{3}, std::size_t{4}}) {
        const auto out = compact_and_verify(history, prefix_budget(live, prefix_length));
        GLIFI_REQUIRE(out.result.compacted());
        GLIFI_REQUIRE(out.shape.has_value());
        GLIFI_REQUIRE(out.shape->source_count == prefix_length && out.shape->replacement_count == 1);
        GLIFI_REQUIRE(out.result.stats.source_segments == prefix_length);
        GLIFI_REQUIRE(out.result.stats.retained_sealed_segments == 4 - prefix_length);
        GLIFI_REQUIRE(out.result.stats.records_copied == prefix_length);
        // The copy never exceeds what the budget admitted.
        GLIFI_REQUIRE(out.result.stats.bytes_copied <= prefix_budget(live, prefix_length));
    }
    const auto unbounded = compact_and_verify(history, 0);
    GLIFI_REQUIRE(unbounded.shape.has_value() && unbounded.shape->source_count == 4);

    // One Segment alone promises no gain: the exact layout runs and reports it. It verified its Record.
    const auto no_gain = compact_and_verify(history, prefix_budget(live, 1));
    GLIFI_REQUIRE(no_gain.result.outcome == glifistore::DurableCompactionOutcome::not_beneficial);
    GLIFI_REQUIRE(no_gain.result.stats.source_segments == 1);
    GLIFI_REQUIRE(no_gain.result.stats.source_index_records_verified == 1);
    GLIFI_REQUIRE(no_gain.result.stats.records_copied == 0);
    GLIFI_REQUIRE(no_gain.result.error.has_value());
    GLIFI_REQUIRE(no_gain.result.error->code == glifistore::ErrorCode::not_found);
}

GLIFI_TEST("an oversized oldest Segment is not_beneficial before any Record read") {
    History history{3};
    add_put(history, 0, "a", "a1");
    add_put(history, 0, "b", "b1");
    add_put(history, 1, "c", "c1");
    add_put(history, 2, "d", "d1");
    add_put(history, 3, "e", "e1");
    const auto live = live_bytes_per_segment(history, kOpenNowNs);
    RecoveryTemporaryDirectory temporary;
    const auto before = write_store(temporary.path(), history);

    RecordReadCounter reads;
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), {.file_io = {.context = &reads, .read_some_at = &RecordReadCounter::read_some_at}});
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), kOpenNowNs,
                                                                  cold_read_runtime_options());
    GLIFI_REQUIRE(runtime.has_value());
    std::map<std::uint64_t, FileIdentity> identities;
    for (const auto& entry : before.segments) {
        identities.emplace(entry.segment_id.value,
                           file_identity(segment_path(temporary.path(), before, entry)));
    }

    // The oldest Segment holds two Records; a budget one byte short of them admits no prefix at all.
    reads.payload_reads = 0;
    const auto refused = (*runtime)->compact_worker(0, kCompactNowNs, live[0] - 1U);
    GLIFI_REQUIRE(refused.outcome == glifistore::DurableCompactionOutcome::not_beneficial);
    GLIFI_REQUIRE(refused.error.has_value());
    GLIFI_REQUIRE(refused.error->code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE(refused.error->message.find("oldest sealed Segment") != std::string::npos);
    GLIFI_REQUIRE(refused.stats.source_index_records_verified == 0);
    GLIFI_REQUIRE(refused.stats.records_copied == 0);
    GLIFI_REQUIRE(refused.stats.source_segments == 0);
    GLIFI_REQUIRE(reads.payload_reads.load() == 0);
    GLIFI_REQUIRE((*runtime)->manifest() == before);
    require_no_intent(temporary.path());
    for (const auto& entry : before.segments) {
        GLIFI_REQUIRE(file_identity(segment_path(temporary.path(), before, entry)) ==
                      identities.at(entry.segment_id.value));
    }
    GLIFI_REQUIRE((*runtime)->healthy());
    const auto refused_metrics = (*runtime)->compaction_stats();
    GLIFI_REQUIRE(refused_metrics.attempts == 1 && refused_metrics.completed == 0);

    // Control: a budget that admits the oldest Segment does read Records, so the counter above is live.
    const auto admitted = (*runtime)->compact_worker(0, kCompactNowNs, live[0] + live[1]);
    GLIFI_REQUIRE(admitted.compacted());
    GLIFI_REQUIRE(admitted.stats.source_segments == 2);
    GLIFI_REQUIRE(reads.payload_reads.load() > 0);
    GLIFI_REQUIRE((*runtime)->healthy());
}

GLIFI_TEST(
    "strict-prefix selection runs the exact layout so expired puts the estimate counts as live are dropped") {
    // Segment 1 holds two puts that have expired by compaction time. The Index carries no expiry, so the
    // estimate sees two live Records and no prefix promises a gain; the exact layout of the longest prefix
    // within the budget drops them and reclaims the Segment with no output.
    History history{3};
    add_put(history, 0, "t1", "v", kExpiresAtNs);
    add_put(history, 0, "t2", "v", kExpiresAtNs);
    add_put(history, 1, "k1", "v");
    add_put(history, 2, "k2", "v");
    add_put(history, 3, "k3", "v");
    const auto live = live_bytes_per_segment(history, kOpenNowNs);

    const auto out = compact_and_verify(history, prefix_budget(live, 1));
    GLIFI_REQUIRE(out.result.compacted());
    GLIFI_REQUIRE(out.shape.has_value());
    GLIFI_REQUIRE(out.shape->source_count == 1 && out.shape->replacement_count == 0);
    GLIFI_REQUIRE(out.result.stats.source_index_records_verified == 2);
    GLIFI_REQUIRE(out.result.stats.expired_records_dropped == 2);
    GLIFI_REQUIRE(out.result.stats.expired_index_erasures == 2);
    GLIFI_REQUIRE(out.result.stats.records_copied == 0);
    GLIFI_REQUIRE(out.result.stats.retained_sealed_segments == 2);
}

namespace {

// key_count controls how quickly the history overwrites itself: a small key space leaves the oldest
// Segments entirely dead, a large one leaves cold live Records at the head of the history.
auto random_history(const std::uint64_t seed, const std::size_t key_count) -> History {
    constexpr std::size_t kSealed = 4;
    std::mt19937_64 random{seed};
    History history{kSealed};
    for (std::size_t segment = 0; segment <= kSealed; ++segment) {
        const std::size_t count = segment == kSealed ? 5U : 8U;
        for (std::size_t index = 0; index < count; ++index) {
            auto key = "key-" + std::to_string(static_cast<std::size_t>(random() % key_count));
            auto value = "value-" + std::to_string(history.operations.size() + 1U);
            switch (random() % 8U) {
            case 0:
            case 1:
                add_erase(history, segment, std::move(key));
                break;
            case 2:
                add_put(history, segment, std::move(key), std::move(value), kExpiresAtNs);
                break;
            case 3:
                add_put(history, segment, std::move(key), std::move(value), kAlreadyExpiredNs);
                break;
            default:
                add_put(history, segment, std::move(key), std::move(value));
                break;
            }
        }
    }
    return history;
}

} // namespace

GLIFI_TEST("randomized histories under every copy budget match their model before and after reopen") {
    struct Family {
        std::uint64_t seed{};
        std::size_t key_count{};
    };

    // Dense histories overwrite themselves into dead heads; sparse ones keep cold live Records at the head.
    constexpr std::array<Family, 6> kFamilies{
        Family{0x5712'1C7A'0F12'2026ULL, 10}, Family{0x5712'1C7A'0F12'2027ULL, 10},
        Family{0xBADC'0FFE'0DDF'00D5ULL, 10}, Family{0x0123'4567'89AB'CDEFULL, 40},
        Family{0xFEED'FACE'CAFE'BEEFULL, 40}, Family{0xD15C'A4D0'5EED'0001ULL, 40}};
    std::size_t strict_prefixes{};
    std::size_t whole_sets{};
    std::size_t zero_outputs{};
    std::size_t refused{};
    for (const auto& family : kFamilies) {
        const auto seed = family.seed;
        const auto history = random_history(seed, family.key_count);
        const auto live = live_bytes_per_segment(history, kOpenNowNs);
        std::vector<std::uint64_t> budgets{0};
        // One byte short of the oldest Segment's live bytes admits no prefix at all.
        if (live[0] > 1) {
            budgets.push_back(live[0] - 1U);
        }
        for (std::size_t prefix_length = 1; prefix_length <= history.sealed_count; ++prefix_length) {
            const auto budget = prefix_budget(live, prefix_length);
            if (budget != 0 && std::ranges::find(budgets, budget) == budgets.end()) {
                budgets.push_back(budget);
            }
        }
        for (const auto budget : budgets) {
            try {
                const auto out = compact_and_verify(history, budget);
                if (!out.result.compacted()) {
                    ++refused;
                    continue;
                }
                GLIFI_REQUIRE(out.shape.has_value());
                const auto sealed_count = history.sealed_count;
                strict_prefixes += out.shape->source_count < sealed_count ? 1U : 0U;
                whole_sets += out.shape->source_count == sealed_count ? 1U : 0U;
                zero_outputs += out.shape->replacement_count == 0 ? 1U : 0U;
                if (budget != 0) {
                    GLIFI_REQUIRE(out.result.stats.bytes_copied <= budget);
                }
            } catch (const std::exception& exception) {
                throw std::runtime_error("seed " + std::to_string(seed) + " budget " +
                                         std::to_string(budget) + " failed: " + exception.what());
            }
        }
    }
    // The sweep must exercise the strict prefix, the complete set, the zero-output transition and a refusal.
    if (strict_prefixes == 0 || whole_sets == 0 || zero_outputs == 0 || refused == 0) {
        throw std::runtime_error("randomized sweep coverage: strict=" + std::to_string(strict_prefixes) +
                                 " whole=" + std::to_string(whole_sets) + " zero=" +
                                 std::to_string(zero_outputs) + " refused=" + std::to_string(refused));
    }
}

GLIFI_TEST("strict-prefix compaction keeps newer foreground state written during the build") {
    History history{3};
    add_put(history, 0, "p1", "p1-old");
    add_put(history, 0, "p2", "p2-old");
    add_put(history, 0, "p3", "p3-old");
    add_put(history, 1, "p4", "p4-old");
    add_put(history, 2, "r1", "r1-old");
    add_put(history, 2, "r2", "r2-old");
    add_put(history, 3, "a1", "a1-old");
    const auto live = live_bytes_per_segment(history, kOpenNowNs);
    RecoveryTemporaryDirectory temporary;
    const auto before = write_store(temporary.path(), history);
    const auto retained_before = file_identity(segment_path(temporary.path(), before, before.segments[2]));

    BlockingRecordRead blocker;
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(),
        {.file_io = {.context = &blocker, .read_some_at = &BlockingRecordRead::read_some_at}});
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), kOpenNowNs);
    GLIFI_REQUIRE(runtime.has_value());

    // The prefix is the oldest two sealed Segments: p1..p4. r1 and r2 live in the retained Segment 3.
    blocker.arm();
    glifistore::DurableCompactionResult compacted;
    std::thread compactor{
        [&] { compacted = (*runtime)->compact_worker(0, kCompactNowNs, prefix_budget(live, 2)); }};
    const bool blocked = blocker.wait_until_blocked();
    bool foreground_committed = blocked;
    if (blocked) {
        // While the build is blocked on its first source Record, acknowledge newer state for every kind of
        // key: overwritten and erased inside the prefix, erased and re-inserted, new, and in the retained
        // Segment (overwritten and erased).
        foreground_committed =
            put_value(**runtime, "p1", "p1-new").committed() && erase_key(**runtime, "p2").committed() &&
            erase_key(**runtime, "p3").committed() && put_value(**runtime, "p3", "p3-again").committed() &&
            put_value(**runtime, "n1", "n1-new").committed() &&
            put_value(**runtime, "r1", "r1-new").committed() && erase_key(**runtime, "r2").committed();
    }
    blocker.release();
    compactor.join();
    GLIFI_REQUIRE(foreground_committed);

    GLIFI_REQUIRE(compacted.compacted());
    const auto& stats = compacted.stats;
    GLIFI_REQUIRE(stats.source_segments == 2);
    GLIFI_REQUIRE(stats.retained_sealed_segments == 1);
    // Four Records were copied. Only p4 kept its snapshot reference; p1, p2 and p3 were superseded during
    // the build and stay as dead output bytes, never in the Index. The retained Segment's keys are not part
    // of the transaction at all.
    GLIFI_REQUIRE(stats.records_copied == 4);
    GLIFI_REQUIRE(stats.remapped_records == 1);
    GLIFI_REQUIRE(stats.superseded_copies == 3);
    GLIFI_REQUIRE(stats.abandoned_copy_bytes == 0);
    GLIFI_REQUIRE(stats.delta_records == 0);
    const auto after = (*runtime)->manifest();
    const auto shape = glifistore::derive_durable_compaction_shape(before, after, worker_zero());
    GLIFI_REQUIRE(shape.has_value());
    GLIFI_REQUIRE(shape->source_count == 2 && shape->replacement_count == 1);
    GLIFI_REQUIRE(file_identity(segment_path(temporary.path(), before, before.segments[2])) ==
                  retained_before);

    const Model expected{{"p1", "p1-new"}, {"p2", std::nullopt}, {"p3", "p3-again"}, {"p4", "p4-old"},
                         {"r1", "r1-new"}, {"r2", std::nullopt}, {"a1", "a1-old"},   {"n1", "n1-new"}};
    require_model(**runtime, expected, kCompactNowNs);
    require_index_verifies(**runtime);
    const auto metrics = (*runtime)->compaction_stats();
    GLIFI_REQUIRE(metrics.completed == 1 && metrics.remapped_records == 1 && metrics.superseded_copies == 3);
    runtime->reset();

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), kCompactNowNs);
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->manifest() == after);
    GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
    require_index_verifies(**reopened);
    require_model(**reopened, expected, kCompactNowNs);
}

GLIFI_TEST("a rotation of the same Worker during the build rebases the prefix onto the newer authority") {
    // At the snapshot the Worker has three sealed Segments, so the unbounded policy selects all three. A
    // rotation of that same Worker during the build appends a fourth sealed Segment, and the identical
    // prefix is recomputed against the new authority: the plan becomes a strict prefix (k=3 of 4).
    History history{3};
    add_put(history, 0, "k1", "v1");
    add_put(history, 1, "k2", "v2");
    add_put(history, 2, "k3", "v3");
    add_put(history, 3, "active_before", "av");
    RecoveryTemporaryDirectory temporary;
    const auto before = write_store(temporary.path(), history);

    BlockingFilesystemOperation blocked_copy{glifistore::FilesystemOperation::write_record};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), glifistore::FilesystemHooks{.context = &blocked_copy,
                                                      .before = &BlockingFilesystemOperation::before});
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), kOpenNowNs);
    GLIFI_REQUIRE(runtime.has_value());

    glifistore::DurableCompactionResult compacted;
    std::thread compactor{[&] { compacted = (*runtime)->compact_worker(0, kCompactNowNs); }};
    const bool copy_blocked = blocked_copy.wait_until_blocked();
    if (!copy_blocked) {
        blocked_copy.release();
        compactor.join();
    }
    GLIFI_REQUIRE(copy_blocked);
    blocked_copy.force_next_record_write_full();

    glifistore::DurableMutationResult rotation;
    std::mutex completion_mutex;
    std::condition_variable completion;
    bool rotation_finished{};
    std::thread writer{[&] {
        rotation = put_value(**runtime, "rotated", "rv");
        {
            const std::lock_guard lock{completion_mutex};
            rotation_finished = true;
        }
        completion.notify_one();
    }};
    bool rotation_completed_during_copy{};
    {
        std::unique_lock lock{completion_mutex};
        rotation_completed_during_copy =
            completion.wait_for(lock, kNativeConcurrencyDeadline, [&] { return rotation_finished; });
    }
    const auto rotated_manifest = (*runtime)->manifest();
    blocked_copy.release();
    writer.join();
    compactor.join();
    GLIFI_REQUIRE(rotation_completed_during_copy);
    GLIFI_REQUIRE(rotation.committed());

    // The rotation sealed the former active Segment 4 and created Segment 5.
    GLIFI_REQUIRE(sealed_entries(rotated_manifest).size() == 4);
    GLIFI_REQUIRE(rotated_manifest.segments.size() == 5);
    GLIFI_REQUIRE(compacted.compacted());
    GLIFI_REQUIRE(compacted.stats.manifest_rebases == 1);
    GLIFI_REQUIRE(compacted.stats.abandoned_copy_bytes == 0);
    GLIFI_REQUIRE(compacted.stats.source_segments == 3);

    const auto after = (*runtime)->manifest();
    const auto shape = glifistore::derive_durable_compaction_shape(rotated_manifest, after, worker_zero());
    GLIFI_REQUIRE(shape.has_value());
    GLIFI_REQUIRE(shape->source_count == 3 && shape->replacement_count == 1);
    const auto canonical =
        glifistore::canonical_durable_compaction_manifest(rotated_manifest, worker_zero(), *shape);
    GLIFI_REQUIRE(canonical.has_value() && after == *canonical);
    // The rebased old authority has the rotated Segment as a retained sealed entry after the prefix.
    GLIFI_REQUIRE(sealed_entries(after).size() == 2);
    GLIFI_REQUIRE(sealed_entries(after)[1] == sealed_entries(rotated_manifest)[3]);
    GLIFI_REQUIRE(compacted.stats.retained_sealed_segments == 1);
    GLIFI_REQUIRE((*runtime)->healthy());
    GLIFI_REQUIRE((*runtime)->namespace_audit().recovery_safe());

    const Model expected{
        {"k1", "v1"}, {"k2", "v2"}, {"k3", "v3"}, {"active_before", "av"}, {"rotated", "rv"}};
    require_model(**runtime, expected, kCompactNowNs);
    require_index_verifies(**runtime);
    runtime->reset();

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path(), kCompactNowNs);
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->manifest() == after);
    GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
    require_model(**reopened, expected, kCompactNowNs);
}

GLIFI_TEST("strict-prefix compaction enumerates every reached filesystem failure point") {
    // Segment 1 keeps a live Record that nothing supersedes, so the oldest Segment alone cannot be
    // reclaimed; the first two sealed Segments together can. The 2-Segment budget selects exactly that.
    History history{3};
    add_put(history, 0, "p", "p1");
    add_put(history, 0, "a", "a1");
    add_put(history, 0, "b", "b1");
    add_put(history, 0, "c", "c1");
    add_put(history, 1, "a", "a2");
    add_erase(history, 1, "b");
    add_put(history, 2, "d", "d1");
    add_erase(history, 2, "c");
    add_put(history, 3, "e", "e1");
    const auto live = live_bytes_per_segment(history, 0);
    const auto budget = prefix_budget(live, 2);
    const auto expected = model_at(history, 0);
    const glifistore::DurableCompactionShape expected_shape{.source_count = 2, .replacement_count = 1};

    constexpr std::size_t kMaximumEnumeratedFailurePoints = 64;
    bool reached_successful_terminal_iteration{};
    std::size_t old_authority_runs{};
    std::size_t next_authority_runs{};
    for (std::size_t failure_index = 1; failure_index <= kMaximumEnumeratedFailurePoints; ++failure_index) {
        RecoveryTemporaryDirectory temporary;
        const auto before = write_store(temporary.path(), history);
        std::map<std::uint64_t, FileIdentity> identities;
        for (const auto& entry : before.segments) {
            identities.emplace(entry.segment_id.value,
                               file_identity(segment_path(temporary.path(), before, entry)));
        }
        const auto next =
            glifistore::canonical_durable_compaction_manifest(before, worker_zero(), expected_shape);
        GLIFI_REQUIRE(next.has_value());

        NthFilesystemFailure failure{.target_occurrence = failure_index};
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(),
            glifistore::FilesystemHooks{.context = &failure, .before = &NthFilesystemFailure::before});
        GLIFI_REQUIRE(directory.has_value());
        auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
        GLIFI_REQUIRE(runtime.has_value());
        const auto result = (*runtime)->compact_worker(0, 0, budget);

        if (!failure.fired_operation) {
            GLIFI_REQUIRE(result.compacted());
            GLIFI_REQUIRE(result.stats.source_segments == 2);
            GLIFI_REQUIRE(failure_index == failure.occurrences + 1U);
            reached_successful_terminal_iteration = true;
            break;
        }

        GLIFI_REQUIRE(failure.occurrences == failure_index);
        GLIFI_REQUIRE(!result.compacted());
        GLIFI_REQUIRE(result.error.has_value());
        GLIFI_REQUIRE(result.error->code == glifistore::ErrorCode::io_error);
        GLIFI_REQUIRE(result.outcome == glifistore::DurableCompactionOutcome::not_compacted ||
                      result.outcome == glifistore::DurableCompactionOutcome::recovery_required);
        GLIFI_REQUIRE((*runtime)->healthy() ==
                      (result.outcome == glifistore::DurableCompactionOutcome::not_compacted));
        runtime->reset();

        // Reopen is the persisted-state oracle: exactly one of the two embedded authorities, a clean
        // namespace, the whole model, and no change to any Segment outside the prefix.
        auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(reopened.has_value());
        GLIFI_REQUIRE((*reopened)->healthy());
        GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
        require_index_verifies(**reopened);
        require_model(**reopened, expected, 0);
        const bool old_authority = (*reopened)->manifest() == before;
        const bool next_authority = (*reopened)->manifest() == *next;
        GLIFI_REQUIRE(old_authority != next_authority);
        old_authority_runs += old_authority ? 1U : 0U;
        next_authority_runs += next_authority ? 1U : 0U;
        require_no_intent(temporary.path());
        const auto sealed = sealed_entries(before);
        for (std::size_t index = 0; index < sealed.size(); ++index) {
            const auto path = segment_path(temporary.path(), before, sealed[index]);
            if (next_authority && index < expected_shape.source_count) {
                GLIFI_REQUIRE(!std::filesystem::exists(path));
            } else {
                GLIFI_REQUIRE(file_identity(path) == identities.at(sealed[index].segment_id.value));
            }
        }
        auto output = sealed[0];
        ++output.generation.value;
        GLIFI_REQUIRE(std::filesystem::exists(segment_path(temporary.path(), *next, output)) ==
                      next_authority);
        GLIFI_REQUIRE(file_identity(segment_path(temporary.path(), before, before.segments.back())) ==
                      identities.at(before.segments.back().segment_id.value));
    }
    GLIFI_REQUIRE(reached_successful_terminal_iteration);
    // Both authorities are reached: failures before the Manifest is durable roll back, later ones commit.
    GLIFI_REQUIRE(old_authority_runs > 0);
    GLIFI_REQUIRE(next_authority_runs > 0);
}
