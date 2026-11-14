#include "glifistore/server/protocol.hpp"
#include "hex_fixture.hpp"
#include "server/reactor_detail.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace {

auto bytes(const std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

auto text(const std::span<const std::byte> value) -> std::string_view {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

} // namespace

GLIFI_TEST("server protocol request round trips and handles partial frames") {
    const glifistore::server::RequestView request{
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 42,
        .expire_at_ns = 900,
        .key = bytes("key"),
        .value = bytes("value"),
    };
    const auto encoded = glifistore::server::encode_request(request);
    GLIFI_REQUIRE(encoded.has_value());

    const auto partial = glifistore::server::decode_request(
        std::span<const std::byte>{encoded->data(), glifistore::server::kRequestHeaderBytes - 1U});
    GLIFI_REQUIRE(partial.has_value());
    GLIFI_REQUIRE(!partial->complete);

    const auto decoded = glifistore::server::decode_request(*encoded);
    GLIFI_REQUIRE(decoded.has_value());
    GLIFI_REQUIRE(decoded->complete);
    GLIFI_REQUIRE(decoded->consumed == encoded->size());
    GLIFI_REQUIRE(decoded->frame.opcode == glifistore::server::RequestOpcode::put);
    GLIFI_REQUIRE(decoded->frame.flags == 0);
    GLIFI_REQUIRE(decoded->frame.request_id == 42);
    GLIFI_REQUIRE(decoded->frame.expire_at_ns == 900);
    GLIFI_REQUIRE(decoded->frame.target_worker == glifistore::server::kNoWorker);
    GLIFI_REQUIRE(text(decoded->frame.key) == "key");
    GLIFI_REQUIRE(text(decoded->frame.value) == "value");
}

GLIFI_TEST("server protocol encodes requests into caller-owned storage") {
    const glifistore::server::RequestView request{
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 42,
        .key = bytes("key"),
        .value = bytes("value"),
    };
    const auto required = glifistore::server::encoded_request_size(request);
    GLIFI_REQUIRE(required.has_value());
    std::vector<std::byte> storage(*required);
    const auto written = glifistore::server::encode_request(storage, request);
    GLIFI_REQUIRE(written.has_value());
    GLIFI_REQUIRE(*written == storage.size());

    const auto owned = glifistore::server::encode_request(request);
    GLIFI_REQUIRE(owned.has_value());
    GLIFI_REQUIRE(storage == *owned);

    storage.pop_back();
    GLIFI_REQUIRE(!glifistore::server::encode_request(storage, request).has_value());
}

GLIFI_TEST("server protocol rejects noncanonical flags and reserved fields") {
    GLIFI_REQUIRE(!glifistore::server::encode_request({
                                                          .opcode = glifistore::server::RequestOpcode::ping,
                                                          .flags = 1,
                                                          .request_id = 1,
                                                      })
                       .has_value());

    auto request = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 2,
    });
    GLIFI_REQUIRE(request.has_value());
    (*request)[36] = std::byte{1};
    GLIFI_REQUIRE(!glifistore::server::decode_request(*request).has_value());

    auto response = glifistore::server::encode_response({
        .status = glifistore::server::ResponseStatus::ok,
        .request_id = 2,
    });
    GLIFI_REQUIRE(response.has_value());
    (*response)[28] = std::byte{1};
    GLIFI_REQUIRE(!glifistore::server::decode_response(*response).has_value());
}

GLIFI_TEST("server protocol rejects inconsistent and oversized request frames") {
    const auto encoded = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 1,
        .value = bytes("ping"),
    });
    GLIFI_REQUIRE(encoded.has_value());

    auto inconsistent = *encoded;
    inconsistent[20] = std::byte{0x7F};
    GLIFI_REQUIRE(!glifistore::server::decode_request(inconsistent).has_value());

    auto oversized = *encoded;
    const auto declared = static_cast<std::uint32_t>(glifistore::server::kMaxFrameBytes + 1U);
    for (std::size_t byte = 0; byte < 4; ++byte) {
        oversized[byte] = static_cast<std::byte>((declared >> (byte * 8U)) & 0xFFU);
    }
    GLIFI_REQUIRE(!glifistore::server::decode_request(oversized).has_value());
}

GLIFI_TEST("server protocol response round trips") {
    const auto encoded = glifistore::server::encode_response({
        .status = glifistore::server::ResponseStatus::ok,
        .request_id = 77,
        .owner_worker = 2,
        .worker_count = 4,
        .routing_epoch = 9,
        .value = bytes("pong"),
    });
    GLIFI_REQUIRE(encoded.has_value());
    const auto decoded = glifistore::server::decode_response(*encoded);
    GLIFI_REQUIRE(decoded.has_value());
    GLIFI_REQUIRE(decoded->complete);
    GLIFI_REQUIRE(decoded->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(decoded->frame.request_id == 77);
    GLIFI_REQUIRE(decoded->frame.owner_worker == 2);
    GLIFI_REQUIRE(decoded->frame.worker_count == 4);
    GLIFI_REQUIRE(decoded->frame.routing_epoch == 9);
    GLIFI_REQUIRE(text(decoded->frame.value) == "pong");
}

GLIFI_TEST("server protocol scatter header matches contiguous response encoding") {
    const glifistore::server::ResponseView response{.status = glifistore::server::ResponseStatus::ok,
                                                    .request_id = 91,
                                                    .owner_worker = 0,
                                                    .worker_count = 1,
                                                    .routing_epoch = 3,
                                                    .value = bytes("scatter-value")};
    std::array<std::byte, glifistore::server::kResponseHeaderBytes> header{};
    const auto declared = glifistore::server::encode_response_header(header, response);
    GLIFI_REQUIRE(declared.has_value());
    GLIFI_REQUIRE(*declared == header.size() + response.value.size());
    std::vector<std::byte> gathered;
    gathered.insert(gathered.end(), header.begin(), header.end());
    gathered.insert(gathered.end(), response.value.begin(), response.value.end());
    const auto contiguous = glifistore::server::encode_response(response);
    GLIFI_REQUIRE(contiguous.has_value());
    GLIFI_REQUIRE(gathered == *contiguous);
}

GLIFI_TEST("server protocol rejects noncanonical opcode-specific fields") {
    GLIFI_REQUIRE(!glifistore::server::encode_request({
                                                          .opcode = glifistore::server::RequestOpcode::get,
                                                          .request_id = 1,
                                                          .key = bytes("k"),
                                                          .value = bytes("x"),
                                                      })
                       .has_value());
    GLIFI_REQUIRE(!glifistore::server::encode_request({
                                                          .opcode = glifistore::server::RequestOpcode::put,
                                                          .request_id = 1,
                                                          .target_worker = 1,
                                                          .key = bytes("k"),
                                                          .value = bytes("v"),
                                                      })
                       .has_value());
    GLIFI_REQUIRE(!glifistore::server::encode_request({
                                                          .opcode = glifistore::server::RequestOpcode::health,
                                                          .request_id = 1,
                                                          .key = bytes("k"),
                                                      })
                       .has_value());
    GLIFI_REQUIRE(
        !glifistore::server::encode_request({
                                                .opcode = glifistore::server::RequestOpcode::bind_worker,
                                                .request_id = 1,
                                            })
             .has_value());
    GLIFI_REQUIRE(!glifistore::server::encode_request({
                                                          .opcode = glifistore::server::RequestOpcode::get,
                                                          .request_id = 1,
                                                      })
                       .has_value());

    auto ping = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 9,
        .value = bytes("ok"),
    });
    GLIFI_REQUIRE(ping.has_value());
    // Force a non-canonical target_worker while keeping payload sizes intact.
    (*ping)[32] = std::byte{1};
    (*ping)[33] = std::byte{0};
    (*ping)[34] = std::byte{0};
    (*ping)[35] = std::byte{0};
    GLIFI_REQUIRE(!glifistore::server::decode_request(*ping).has_value());
}

GLIFI_TEST("wire protocol v2 matches independent canonical request fixtures") {
    const auto corpus = glifistore::test::read_hex_fixture(std::filesystem::path{GLIFISTORE_SOURCE_DIR} /
                                                           "tests/fixtures/wire_requests_v2.hex");
    std::size_t offset{};
    std::uint8_t expected_opcode{1};
    while (offset < corpus.size()) {
        const auto remaining = std::span<const std::byte>{corpus}.subspan(offset);
        const auto decoded = glifistore::server::decode_request(remaining);
        GLIFI_REQUIRE(decoded.has_value());
        GLIFI_REQUIRE(decoded->complete);
        GLIFI_REQUIRE(static_cast<std::uint8_t>(decoded->frame.opcode) == expected_opcode);
        const auto reencoded = glifistore::server::encode_request(decoded->frame);
        GLIFI_REQUIRE(reencoded.has_value());
        GLIFI_REQUIRE(reencoded->size() == decoded->consumed);
        GLIFI_REQUIRE(std::equal(reencoded->begin(), reencoded->end(), remaining.begin()));
        offset += decoded->consumed;
        ++expected_opcode;
    }
    GLIFI_REQUIRE(expected_opcode == 11);
}

GLIFI_TEST("wire protocol v2 matches independent canonical response fixtures") {
    const auto corpus = glifistore::test::read_hex_fixture(std::filesystem::path{GLIFISTORE_SOURCE_DIR} /
                                                           "tests/fixtures/wire_responses_v2.hex");
    std::size_t offset{};
    std::uint16_t expected_status{};
    while (offset < corpus.size()) {
        const auto remaining = std::span<const std::byte>{corpus}.subspan(offset);
        const auto decoded = glifistore::server::decode_response(remaining);
        GLIFI_REQUIRE(decoded.has_value());
        GLIFI_REQUIRE(decoded->complete);
        GLIFI_REQUIRE(static_cast<std::uint16_t>(decoded->frame.status) == expected_status);
        const auto reencoded = glifistore::server::encode_response(decoded->frame);
        GLIFI_REQUIRE(reencoded.has_value());
        GLIFI_REQUIRE(reencoded->size() == decoded->consumed);
        GLIFI_REQUIRE(std::equal(reencoded->begin(), reencoded->end(), remaining.begin()));
        offset += decoded->consumed;
        ++expected_status;
    }
    GLIFI_REQUIRE(expected_status == 9);
}

GLIFI_TEST("reactor maps unavailable to INTERNAL_ERROR not OVERLOADED") {
    // Post-commit / fail-closed Store errors use ErrorCode::unavailable. Wire
    // OVERLOADED is known-not-committed; INTERNAL_ERROR is reconcile_first.
    using glifistore::Error;
    using glifistore::ErrorCode;
    using glifistore::server::ResponseStatus;
    using glifistore::server::reactor_detail::response_status;

    GLIFI_REQUIRE(response_status(Error{ErrorCode::unavailable, {}}) == ResponseStatus::internal_error);
    GLIFI_REQUIRE(response_status(Error{ErrorCode::resource_exhausted, {}}) == ResponseStatus::overloaded);
    GLIFI_REQUIRE(response_status(Error{ErrorCode::storage_exhausted, {}}) == ResponseStatus::overloaded);
    GLIFI_REQUIRE(response_status(Error{ErrorCode::sequence_conflict, {}}) == ResponseStatus::overloaded);
    GLIFI_REQUIRE(response_status(Error{ErrorCode::segment_full, {}}) == ResponseStatus::overloaded);
    GLIFI_REQUIRE(response_status(Error{ErrorCode::segment_sealed, {}}) == ResponseStatus::overloaded);
    GLIFI_REQUIRE(response_status(Error{ErrorCode::arithmetic_overflow, {}}) == ResponseStatus::overloaded);
    GLIFI_REQUIRE(response_status(Error{ErrorCode::internal_error, {}}) == ResponseStatus::internal_error);
    GLIFI_REQUIRE(response_status(Error{ErrorCode::corrupted_data, {}}) == ResponseStatus::internal_error);
    GLIFI_REQUIRE(response_status(Error{ErrorCode::io_error, {}}) == ResponseStatus::internal_error);
}
