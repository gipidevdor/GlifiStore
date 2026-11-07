#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/blob.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "glifistore/persistence/store_backup.hpp"
#include "glifistore/segment/crc32c.hpp"
#include "glifistore/store/store.hpp"
#include "test.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace glifistore;

struct Temp {
    std::filesystem::path path;

    Temp() {
        static std::atomic_uint64_t counter{};
        path = std::filesystem::temp_directory_path() /
               ("glifi-blob-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::remove_all(path);
    }

    ~Temp() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

auto config(const std::filesystem::path& path) -> StoreConfig {
    StoreConfig cfg;
    cfg.data_directory = path;
    cfg.enable_blobs = true;
    cfg.storage_mode = StorageMode::durable_sync;
    cfg.worker_config.explicit_count = 1;
    cfg.durable_limits.reserved_free_bytes = 0;
    cfg.maintenance.mode = MaintenanceMode::disabled;
    return cfg;
}

template <class T> auto take(Result<T> value) -> T {
    if (!value)
        throw std::runtime_error(value.error().message);
    return std::move(*value);
}

void success(Status status) {
    if (!status)
        throw std::runtime_error(status.error().message);
}

auto data(std::string_view value) -> std::span<const std::byte> {
    return std::as_bytes(std::span{value.data(), value.size()});
}

auto hex(const BlobDigest& digest) -> std::string {
    std::string result;
    constexpr auto digits = "0123456789abcdef";
    for (auto b : digest) {
        auto v = std::to_integer<unsigned>(b);
        result += digits[v >> 4U];
        result += digits[v & 15U];
    }
    return result;
}

auto file_count(const std::filesystem::path& path) -> std::size_t {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator{path / "blobs"},
                                                  std::filesystem::directory_iterator{}));
}
} // namespace

GLIFI_TEST("blob SHA256 matches independent empty abc and million-a vectors") {
    BlobHasher hash;
    GLIFI_REQUIRE(hex(hash.finish()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    hash.update(data("a"));
    hash.update(data("bc"));
    GLIFI_REQUIRE(hex(hash.finish()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    BlobHasher million;
    std::array<std::byte, 1000> block{};
    block.fill(std::byte{'a'});
    for (unsigned i = 0; i < 1000; ++i)
        million.update(block);
    GLIFI_REQUIRE(hex(million.finish()) ==
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

GLIFI_TEST("blob codec bounds and metadata corruption reject before payload allocation") {
    BlobRef ref{.id = {std::byte{1}}, .logical_size = kMaximumBlobValueBytes};
    auto encoded = take(encode_blob_ref(ref));
    GLIFI_REQUIRE(take(decode_blob_ref(encoded)) == ref);
    GLIFI_REQUIRE(blob_chunk_count(ref.logical_size) == 64);
    GLIFI_REQUIRE(blob_file_bytes(ref.logical_size) == 67'110'080);
    encoded[36] = std::byte{65};
    GLIFI_REQUIRE(!decode_blob_ref(encoded));
    ref.logical_size = kMaximumBlobValueBytes + 1;
    GLIFI_REQUIRE(!encode_blob_ref(ref));
    ref.logical_size = 0;
    GLIFI_REQUIRE(!encode_blob_ref(ref));
    ref.logical_size = kBlobChunkBytes + 1;
    BlobFileMetadata meta{.reference = ref, .store_id = {std::byte{2}}};
    meta.chunks[0] = {.offset = 160, .length = kBlobChunkBytes, .checksum = 17};
    meta.chunks[1] = {.offset = 160 + kBlobChunkBytes, .length = 1, .checksum = 19};
    auto header = take(encode_blob_metadata(meta));
    auto decoded = take(decode_blob_metadata(std::span{header}.first(160)));
    GLIFI_REQUIRE(decoded.reference.logical_size == ref.logical_size);
    header[128] ^= std::byte{1};
    GLIFI_REQUIRE(!decode_blob_metadata(std::span{header}.first(160)));
    auto footer = take(encode_blob_footer(ref));
    success(validate_blob_footer(footer, ref));
    footer[16] ^= std::byte{1};
    GLIFI_REQUIRE(!validate_blob_footer(footer, ref));
}

GLIFI_TEST("blob and v2 codecs match independent Python golden fixtures") {
    const auto fixture = [](std::string_view name) {
        const auto path = std::filesystem::path{__FILE__}.parent_path().parent_path() / "fixtures" / name;
        std::ifstream input(path);
        if (!input)
            throw std::runtime_error("missing blob golden fixture");
        std::vector<std::byte> bytes;
        std::string line;
        while (std::getline(input, line)) {
            for (std::size_t i = 0; i < line.size(); i += 2) {
                bytes.push_back(static_cast<std::byte>(std::stoul(line.substr(i, 2), nullptr, 16)));
            }
        }
        return bytes;
    };
    auto ref = take(decode_blob_ref(fixture("blob_ref_v1.hex")));
    std::vector<std::byte> payload(kBlobChunkBytes, std::byte{'A'});
    BlobHasher hash;
    hash.update(payload);
    hash.update(data("xyz"));
    GLIFI_REQUIRE(hash.finish() == ref.digest);
    GLIFI_REQUIRE(ref.logical_size == kBlobChunkBytes + 3);
    GLIFI_REQUIRE(std::ranges::equal(take(encode_blob_ref(ref)), fixture("blob_ref_v1.hex")));
    BlobFileMetadata meta{.reference = ref};
    for (std::size_t i = 0; i < meta.store_id.size(); ++i) {
        meta.store_id[i] = static_cast<std::byte>(i);
    }
    meta.chunks[0] = {.offset = 160, .length = kBlobChunkBytes, .checksum = crc32c(payload)};
    meta.chunks[1] = {.offset = 160 + kBlobChunkBytes, .length = 3, .checksum = crc32c(data("xyz"))};
    auto metadata = take(encode_blob_metadata(meta));
    GLIFI_REQUIRE(std::ranges::equal(std::span{metadata}.first(160), fixture("blob_file_v1_metadata.hex")));
    GLIFI_REQUIRE(std::ranges::equal(take(encode_blob_footer(ref)), fixture("blob_file_v1_footer.hex")));
    const auto descriptor = take(encode_blob_ref(ref));
    RecordInput record{.sequence = SequenceNumber{7},
                       .type = ValueType::blob_ref,
                       .key_hash = hash_key("blob-key"),
                       .key = data("blob-key"),
                       .value = descriptor,
                       .format_version = 2};
    GLIFI_REQUIRE(std::ranges::equal(take(encode_record(record)), fixture("record_v2_blob_ref.hex")));
    Manifest manifest{.store_id = meta.store_id,
                      .manifest_generation = 3,
                      .worker_count = 1,
                      .routing_epoch = 1,
                      .next_segment_id = SegmentId{2},
                      .next_segment_generation = GenerationId{1}};
    manifest.segments.push_back(
        {.segment_id = SegmentId{1}, .generation = GenerationId{1}, .role = ManifestSegmentRole::active});
    manifest.format_version = 2;
    manifest.next_blob_id[0] = std::byte{2};
    GLIFI_REQUIRE(std::ranges::equal(take(encode_manifest(manifest)), fixture("manifest_v2_blob.hex")));
    GLIFI_REQUIRE(take(decode_manifest(fixture("manifest_v2_blob.hex"))) == manifest);
    SegmentHeader header{.identity = {.store_id = meta.store_id,
                                      .segment_id = SegmentId{1},
                                      .generation = GenerationId{1},
                                      .format_version = 2},
                         .commits = {}};
    header.commits[0] = SegmentCommit{.commit_generation = 1, .committed_end = 4096};
    std::array<std::byte, kSegmentHeaderReservedBytes> encoded{};
    success(encode_segment_header(encoded, header));
    GLIFI_REQUIRE(std::ranges::equal(std::span{encoded}.first(128), fixture("segment_header_v2_prefix.hex")));
}

GLIFI_TEST("blob logical boundary sizes use typed placement and survive reopen") {
    Temp temp;
    auto cfg = config(temp.path);
    auto store = take(Store::open(cfg));
    const std::string key = "key";
    const auto limit = kMaxNormalRecordSize - kEncodedRecordHeaderSize - key.size();
    for (const auto size : {std::size_t{0}, std::size_t{1}, limit - 1, limit, limit + 1,
                            std::size_t{kBlobChunkBytes}, std::size_t{kBlobChunkBytes + 1}}) {
        std::vector<std::byte> value(size, std::byte{0x53});
        success(store->put(key, value));
        auto got = take(store->get(key));
        GLIFI_REQUIRE(got.view().size() == size);
        GLIFI_REQUIRE(std::ranges::equal(got.view(), value));
    }
    auto opaque = take(encode_blob_ref(BlobRef{.id = {std::byte{1}}, .logical_size = 1}));
    success(store->put("opaque", opaque));
    GLIFI_REQUIRE(std::ranges::equal(take(store->get("opaque")).view(), opaque));
    success(store->close());
    store.reset();
    auto legacy = cfg;
    legacy.enable_blobs = false;
    GLIFI_REQUIRE(!Store::open(legacy));
    store = take(Store::open(cfg));
    auto got = take(store->get(key));
    GLIFI_REQUIRE(got.bytes.size() == kBlobChunkBytes + 1);
    GLIFI_REQUIRE(got.bytes.front() == std::byte{0x53});
}

GLIFI_TEST("blob streaming stores and reads exactly 64 MiB with bounded chunks") {
    Temp temp;
    auto cfg = config(temp.path);
    auto store = take(Store::open(cfg));
    const std::string key(kMaximumBlobKeyBytes, 'k');
    auto writer = take(store->begin_blob_put(key, {.expected_size = kMaximumBlobValueBytes}));
    std::vector<std::byte> chunk(kBlobChunkBytes);
    for (std::size_t i = 0; i < chunk.size(); ++i)
        chunk[i] = static_cast<std::byte>((i * 31U + 7U) & 255U);
    for (unsigned i = 0; i < 64; ++i)
        success(writer->write(chunk));
    success(writer->commit());
    success(writer->commit());
    GLIFI_REQUIRE(!writer->write(data("x")));
    writer.reset();
    auto reader = take(store->open_blob(key));
    GLIFI_REQUIRE(reader->size() == kMaximumBlobValueBytes);
    std::array<std::byte, 17'123> output{};
    std::uint64_t position{};
    for (;;) {
        const auto read = take(reader->read(output));
        if (read == 0)
            break;
        for (std::size_t i = 0; i < read; ++i)
            GLIFI_REQUIRE(output[i] == chunk[static_cast<std::size_t>((position + i) % chunk.size())]);
        position += read;
    }
    GLIFI_REQUIRE(position == kMaximumBlobValueBytes);
    reader.reset();
    success(store->close());
    store.reset();
    store = take(Store::open(cfg));
    auto owning = take(store->get(key));
    GLIFI_REQUIRE(owning.bytes.size() == kMaximumBlobValueBytes);
    GLIFI_REQUIRE(owning.bytes.front() == chunk.front());
    GLIFI_REQUIRE(owning.bytes.back() == chunk.back());
    GLIFI_REQUIRE(!store->begin_blob_put("oversize", {.expected_size = kMaximumBlobValueBytes + 1}));
}

GLIFI_TEST("blob stream pins old version through overwrite erase and collection") {
    Temp temp;
    auto store = take(Store::open(config(temp.path)));
    std::vector<std::byte> first(kBlobChunkBytes + 13, std::byte{0x41}),
        second(first.size(), std::byte{0x42});
    success(store->put("key", first));
    auto old = take(store->open_blob("key"));
    success(store->put("key", second));
    success(store->erase("key"));
    GLIFI_REQUIRE(!store->get("key"));
    auto gc = take(store->collect_blobs());
    GLIFI_REQUIRE(gc.files_retained == 2);
    GLIFI_REQUIRE(gc.files_removed == 0);
    std::array<std::byte, 8192> chunk{};
    std::size_t size{};
    for (;;) {
        const auto n = take(old->read(chunk));
        if (!n)
            break;
        size += n;
        GLIFI_REQUIRE(
            std::ranges::all_of(std::span{chunk}.first(n), [](auto b) { return b == std::byte{0x41}; }));
    }
    GLIFI_REQUIRE(size == first.size());
    success(store->close());
    GLIFI_REQUIRE(!old->read(chunk));
}

GLIFI_TEST("blob transfer quotas abort and terminal commit return credits") {
    Temp temp;
    auto store = take(Store::open(config(temp.path)));
    auto first = take(store->begin_blob_put("a", {.expected_size = 4}));
    auto second = take(store->begin_blob_put("b"));
    GLIFI_REQUIRE(!store->begin_blob_put("c"));
    second->abort();
    auto third = take(store->begin_blob_put("c"));
    third->abort();
    success(first->write(data("abc")));
    GLIFI_REQUIRE(!first->commit());
    GLIFI_REQUIRE(!first->commit());
    auto next = take(store->begin_blob_put("d", {.expected_size = 0}));
    success(next->commit());
    GLIFI_REQUIRE(take(store->get("d")).bytes.empty());
    auto unknown = take(store->begin_blob_put("unknown"));
    std::vector<std::byte> part(kBlobChunkBytes, std::byte{0x71});
    success(unknown->write(std::span{part}.first(17)));
    success(unknown->write(part));
    success(unknown->commit());
    GLIFI_REQUIRE(take(store->get("unknown")).bytes.size() == kBlobChunkBytes + 17);
}

GLIFI_TEST("blob install failure before descriptor commit leaves reclaimable orphan") {
    Temp temp;
    auto cfg = config(temp.path);
    std::atomic_bool fail{};
    cfg.filesystem_hooks.context = &fail;
    cfg.filesystem_hooks.before = [](void* p, FilesystemOperation op) -> Status {
        if (op == FilesystemOperation::write_record && static_cast<std::atomic_bool*>(p)->exchange(false))
            return glifistore::fail(ErrorCode::resource_exhausted, "injected descriptor rejection");
        return {};
    };
    auto store = take(Store::open(cfg));
    success(store->put("key", data("old")));
    fail = true;
    std::vector<std::byte> value(kBlobChunkBytes + 1, std::byte{1});
    GLIFI_REQUIRE(!store->put("key", value));
    GLIFI_REQUIRE(take(store->get("key")).bytes.size() == 3);
    GLIFI_REQUIRE(file_count(temp.path) == 1);
    auto gc = take(store->collect_blobs());
    GLIFI_REQUIRE(gc.files_removed == 1);
    GLIFI_REQUIRE(file_count(temp.path) == 0);
}

GLIFI_TEST("blob corruption in overwritten committed history fails closed on recovery") {
    Temp temp;
    auto cfg = config(temp.path);
    auto store = take(Store::open(cfg));
    std::vector<std::byte> value(kBlobChunkBytes + 1, std::byte{7});
    success(store->put("key", value));
    const auto old = std::filesystem::directory_iterator(temp.path / "blobs")->path();
    success(store->put("key", data("new inline")));
    success(store->close());
    store.reset();
    std::fstream file(old, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(160);
    const char bad = 'x';
    file.write(&bad, 1);
    file.close();
    GLIFI_REQUIRE(!Store::open(cfg));
}

GLIFI_TEST("blob fenced backup retains historical descriptor closure and restores values") {
    Temp temp, backup;
    auto cfg = config(temp.path);
    auto store = take(Store::open(cfg));
    std::vector<std::byte> value(kBlobChunkBytes + 11, std::byte{0x63});
    success(store->put("key", value));
    success(store->put("key", data("inline")));
    success(store->put("blob", value));
    auto report = take(store->backup_to(backup.path));
    GLIFI_REQUIRE(report.destination_verification.manifest.format_version == 2);
    GLIFI_REQUIRE(file_count(backup.path) == 2);
    auto restored = take(Store::open(config(backup.path)));
    GLIFI_REQUIRE(std::ranges::equal(take(restored->get("blob")).view(), value));
    GLIFI_REQUIRE(take(restored->get("key")).bytes.size() == 6);
}

GLIFI_TEST("blob v2 descriptor compaction reclaims without a Segment-count gain") {
    Temp temp;
    auto cfg = config(temp.path);
    auto store = take(Store::open(cfg));
    std::vector<std::byte> value(kBlobChunkBytes + 1, std::byte{0x61});
    success(store->put("keep", value));
    success(store->put("hot", value));
    auto reader = take(store->open_blob("hot"));
    for (unsigned i = 0; i < 32; ++i) {
        value.back() = static_cast<std::byte>(i);
        success(store->put("hot", value));
    }
    auto compacted = take(store->compact());
    GLIFI_REQUIRE(compacted.compacted);
    auto gc = take(store->collect_blobs());
    GLIFI_REQUIRE(gc.files_removed > 0);
    std::array<std::byte, 128> buffer{};
    GLIFI_REQUIRE(take(reader->read(buffer)) == buffer.size());
    GLIFI_REQUIRE(buffer.front() == std::byte{0x61});
    reader.reset();
    GLIFI_REQUIRE(take(store->collect_blobs()).files_removed == 1);
    GLIFI_REQUIRE(take(store->get("keep")).bytes.back() == std::byte{0x61});
    success(store->close());
    store.reset();
    store = take(Store::open(cfg));
    GLIFI_REQUIRE(take(store->get("hot")).bytes.back() == std::byte{31});
}

GLIFI_TEST("blob strict group and periodic commits retain typed values after flush") {
    for (const auto policy : {StorageMode::durable_group, StorageMode::durable_periodic}) {
        Temp temp;
        auto cfg = config(temp.path);
        cfg.storage_mode = policy;
        auto store = take(Store::open(cfg));
        std::vector<std::byte> value(kBlobChunkBytes + 1, std::byte{0x39});
        auto first = take(store->begin_blob_put("one", {.expected_size = value.size()}));
        auto second = take(store->begin_blob_put("two", {.expected_size = value.size()}));
        success(first->write(std::span{value}.first(kBlobChunkBytes)));
        success(first->write(std::span{value}.last(1)));
        success(second->write(std::span{value}.first(kBlobChunkBytes)));
        success(second->write(std::span{value}.last(1)));
        Status a, b;
        std::thread one{[&] { a = first->commit(); }};
        std::thread two{[&] { b = second->commit(); }};
        one.join();
        two.join();
        success(a);
        success(b);
        success(store->flush());
        GLIFI_REQUIRE(std::ranges::equal(take(store->get("one")).view(), value));
        success(store->close());
        store.reset();
        store = take(Store::open(cfg));
        GLIFI_REQUIRE(std::ranges::equal(take(store->get("two")).view(), value));
    }
}

GLIFI_TEST("blob recovery budget rejects insufficient metadata capacity before creation") {
    Temp temp;
    auto cfg = config(temp.path);
    cfg.durable_limits.max_recovery_memory_bytes = 1024 * 1024;
    GLIFI_REQUIRE(!Store::open(cfg));
    GLIFI_REQUIRE(!std::filesystem::exists(temp.path));
}

GLIFI_TEST("blob streaming read keeps its selected value after TTL expiration") {
    struct Clock final : StoreClock {
        std::atomic_uint64_t now{100};

        auto now_ns() const noexcept -> std::uint64_t override {
            return now.load();
        }
    };

    Temp temp;
    auto cfg = config(temp.path);
    auto clock = std::make_shared<Clock>();
    cfg.clock = clock;
    auto store = take(Store::open(cfg));
    std::vector<std::byte> value(kBlobChunkBytes + 1, std::byte{0x67});
    success(store->put("ttl", value, 150));
    auto reader = take(store->open_blob("ttl"));
    clock->now = 200;
    GLIFI_REQUIRE(!store->get("ttl"));
    GLIFI_REQUIRE(take(store->collect_blobs()).files_retained == 1);
    std::array<std::byte, 8192> buffer{};
    std::size_t total{};
    for (;;) {
        const auto count = take(reader->read(buffer));
        if (!count)
            break;
        total += count;
    }
    GLIFI_REQUIRE(total == value.size());
}
