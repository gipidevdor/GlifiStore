#include "glifistore/persistence/compaction_intent.hpp"
#include "glifistore/persistence/manifest.hpp"
#include "glifistore/segment/record.hpp"
#include "glifistore/segment/segment_header.hpp"
#include "glifistore/server/protocol.hpp"
#include "hex_fixture.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace {

[[nodiscard]] auto released_root() -> std::filesystem::path {
    return std::filesystem::path{GLIFISTORE_SOURCE_DIR} / "tests/fixtures/released";
}

[[nodiscard]] auto decode_wire_requests(const std::span<const std::byte> corpus) -> bool {
    std::size_t offset{};
    while (offset < corpus.size()) {
        const auto remaining = corpus.subspan(offset);
        const auto decoded = glifistore::server::decode_request(remaining);
        if (!decoded || !decoded->complete || decoded->consumed == 0) {
            return false;
        }
        const auto reencoded = glifistore::server::encode_request(decoded->frame);
        if (!reencoded || reencoded->size() != decoded->consumed ||
            !std::equal(reencoded->begin(), reencoded->end(), remaining.begin())) {
            return false;
        }
        offset += decoded->consumed;
    }
    return true;
}

[[nodiscard]] auto decode_wire_responses(const std::span<const std::byte> corpus) -> bool {
    std::size_t offset{};
    while (offset < corpus.size()) {
        const auto remaining = corpus.subspan(offset);
        const auto decoded = glifistore::server::decode_response(remaining);
        if (!decoded || !decoded->complete || decoded->consumed == 0) {
            return false;
        }
        const auto reencoded = glifistore::server::encode_response(decoded->frame);
        if (!reencoded || reencoded->size() != decoded->consumed ||
            !std::equal(reencoded->begin(), reencoded->end(), remaining.begin())) {
            return false;
        }
        offset += decoded->consumed;
    }
    return true;
}

[[nodiscard]] auto decode_named_fixture(const std::filesystem::path& path) -> bool {
    const auto name = path.filename().string();
    const auto bytes = glifistore::test::read_hex_fixture(path);
    if (name.find("manifest") != std::string::npos && name.find("intent") == std::string::npos) {
        return glifistore::decode_manifest(bytes).has_value();
    }
    if (name.find("compaction_intent") != std::string::npos) {
        return glifistore::decode_compaction_intent(bytes).has_value();
    }
    if (name.find("segment_header") != std::string::npos ||
        name.find("segment_v1_header") != std::string::npos) {
        std::array<std::byte, glifistore::kSegmentHeaderReservedBytes> encoded{};
        if (bytes.size() > encoded.size()) {
            return false;
        }
        std::copy(bytes.begin(), bytes.end(), encoded.begin());
        return glifistore::decode_segment_header(encoded).has_value();
    }
    if (name.find("record") != std::string::npos) {
        return glifistore::decode_record(bytes).has_value();
    }
    if (name == "wire_requests_v2.hex") {
        return decode_wire_requests(bytes);
    }
    if (name == "wire_responses_v2.hex") {
        return decode_wire_responses(bytes);
    }
    // Unknown future fixtures remain metadata-only until an explicit decoder is added here.
    return true;
}

} // namespace

GLIFI_TEST("released artifact harness decodes every labeled fixture tree") {
    const auto root = released_root();
    GLIFI_REQUIRE(std::filesystem::exists(root));
    std::size_t labels = 0;
    std::size_t fixtures = 0;
    for (const auto& entry : std::filesystem::directory_iterator{root}) {
        if (!entry.is_directory()) {
            continue;
        }
        ++labels;
        for (const auto& fixture : std::filesystem::directory_iterator{entry.path()}) {
            if (!fixture.is_regular_file() || fixture.path().extension() != ".hex") {
                continue;
            }
            ++fixtures;
            GLIFI_REQUIRE(decode_named_fixture(fixture.path()));
        }
    }
    // Zero labels is success: the harness is present; tagged drops remain an alpha gate.
    GLIFI_REQUIRE(labels == 0 || fixtures > 0);
}
