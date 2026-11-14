#include "glifistore/server/protocol.hpp"

#include "glifistore/core/checked_math.hpp"
#include "glifistore/core/little_endian.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace glifistore::server {
namespace {

[[nodiscard]] auto load_u16(const std::span<const std::byte> input, const std::size_t offset) noexcept
    -> std::uint16_t {
    return le::get_u16(input, offset);
}

[[nodiscard]] auto load_u32(const std::span<const std::byte> input, const std::size_t offset) noexcept
    -> std::uint32_t {
    return le::get_u32(input, offset);
}

[[nodiscard]] auto load_u64(const std::span<const std::byte> input, const std::size_t offset) noexcept
    -> std::uint64_t {
    return le::get_u64(input, offset);
}

void store_u16(const std::span<std::byte> output, const std::size_t offset,
               const std::uint16_t value) noexcept {
    le::put_u16(output, offset, value);
}

void store_u32(const std::span<std::byte> output, const std::size_t offset,
               const std::uint32_t value) noexcept {
    le::put_u32(output, offset, value);
}

void store_u64(const std::span<std::byte> output, const std::size_t offset,
               const std::uint64_t value) noexcept {
    le::put_u64(output, offset, value);
}

[[nodiscard]] auto request_opcode(const std::uint8_t encoded) -> Result<RequestOpcode> {
    switch (encoded) {
    case static_cast<std::uint8_t>(RequestOpcode::init):
        return RequestOpcode::init;
    case static_cast<std::uint8_t>(RequestOpcode::ping):
        return RequestOpcode::ping;
    case static_cast<std::uint8_t>(RequestOpcode::get):
        return RequestOpcode::get;
    case static_cast<std::uint8_t>(RequestOpcode::put):
        return RequestOpcode::put;
    case static_cast<std::uint8_t>(RequestOpcode::erase):
        return RequestOpcode::erase;
    case static_cast<std::uint8_t>(RequestOpcode::bind_worker):
        return RequestOpcode::bind_worker;
    case static_cast<std::uint8_t>(RequestOpcode::health):
        return RequestOpcode::health;
    case static_cast<std::uint8_t>(RequestOpcode::ready):
        return RequestOpcode::ready;
    case static_cast<std::uint8_t>(RequestOpcode::stats):
        return RequestOpcode::stats;
    case static_cast<std::uint8_t>(RequestOpcode::backup):
        return RequestOpcode::backup;
    default:
        return fail(ErrorCode::invalid_record, "request contains an unknown opcode");
    }
}

[[nodiscard]] auto response_status(const std::uint16_t encoded) -> Result<ResponseStatus> {
    switch (encoded) {
    case static_cast<std::uint16_t>(ResponseStatus::ok):
        return ResponseStatus::ok;
    case static_cast<std::uint16_t>(ResponseStatus::invalid_request):
        return ResponseStatus::invalid_request;
    case static_cast<std::uint16_t>(ResponseStatus::unsupported):
        return ResponseStatus::unsupported;
    case static_cast<std::uint16_t>(ResponseStatus::internal_error):
        return ResponseStatus::internal_error;
    case static_cast<std::uint16_t>(ResponseStatus::not_found):
        return ResponseStatus::not_found;
    case static_cast<std::uint16_t>(ResponseStatus::overloaded):
        return ResponseStatus::overloaded;
    case static_cast<std::uint16_t>(ResponseStatus::wrong_owner):
        return ResponseStatus::wrong_owner;
    case static_cast<std::uint16_t>(ResponseStatus::not_bound):
        return ResponseStatus::not_bound;
    case static_cast<std::uint16_t>(ResponseStatus::permission_denied):
        return ResponseStatus::permission_denied;
    default:
        return fail(ErrorCode::invalid_record, "response contains an unknown status");
    }
}

[[nodiscard]] auto checked_frame_size(const std::size_t header_size, const std::size_t first_size,
                                      const std::size_t second_size) -> Result<std::size_t> {
    const auto with_first = checked_add(header_size, first_size);
    if (!with_first) {
        return unexpected(with_first.error());
    }
    return checked_add(*with_first, second_size);
}

[[nodiscard]] auto checked_u32(const std::size_t value, const char* field) -> Result<std::uint32_t> {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return fail(ErrorCode::record_too_large, std::string{field} + " exceeds protocol limits");
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] auto validate_request_fields(const RequestView& request) -> Status {
    const bool has_key = !request.key.empty();
    const bool has_value = !request.value.empty();
    const bool has_expiry = request.expire_at_ns != 0;
    const bool has_target = request.target_worker != kNoWorker;
    switch (request.opcode) {
    case RequestOpcode::init:
    case RequestOpcode::health:
    case RequestOpcode::ready:
    case RequestOpcode::stats:
        if (has_key || has_value || has_expiry || has_target) {
            return fail(ErrorCode::invalid_argument,
                        "lifecycle probe/INIT cannot carry key, value, expiry, or target_worker");
        }
        return {};
    case RequestOpcode::ping:
        if (has_key || has_expiry || has_target) {
            return fail(ErrorCode::invalid_argument,
                        "PING request cannot carry key, expiry, or target_worker");
        }
        return {};
    case RequestOpcode::backup:
        // Key carries the destination data-directory path (UTF-8). No value/expiry/target.
        if (!has_key || has_value || has_expiry || has_target) {
            return fail(ErrorCode::invalid_argument,
                        "BACKUP requires a destination path key and no value, expiry, or target_worker");
        }
        return {};
    case RequestOpcode::get:
        if (!has_key || has_value || has_expiry || has_target) {
            return fail(ErrorCode::invalid_argument,
                        "GET request requires a key and cannot carry value, expiry, or target_worker");
        }
        return {};
    case RequestOpcode::put:
        if (!has_key || has_target) {
            return fail(ErrorCode::invalid_argument,
                        "PUT request requires a key and cannot carry target_worker");
        }
        return {};
    case RequestOpcode::erase:
        if (!has_key || has_value || has_expiry || has_target) {
            return fail(ErrorCode::invalid_argument,
                        "ERASE request requires a key and cannot carry value, expiry, or target_worker");
        }
        return {};
    case RequestOpcode::bind_worker:
        if (has_key || has_value || has_expiry) {
            return fail(ErrorCode::invalid_argument,
                        "BIND_WORKER request cannot carry key, value, or expiry");
        }
        if (!has_target) {
            return fail(ErrorCode::invalid_argument,
                        "BIND_WORKER request requires an explicit target_worker");
        }
        return {};
    }
    return fail(ErrorCode::invalid_argument, "request contains an unknown opcode");
}

} // namespace

auto decode_request(const std::span<const std::byte> input, const std::size_t maximum_frame_bytes)
    -> Result<DecodedFrame<RequestView>> {
    if (input.size() < sizeof(std::uint32_t)) {
        return DecodedFrame<RequestView>{};
    }
    const auto frame_size = static_cast<std::size_t>(load_u32(input, 0));
    if (frame_size < kRequestHeaderBytes || frame_size > maximum_frame_bytes) {
        return fail(ErrorCode::invalid_record, "request frame size is outside protocol limits");
    }
    if (input.size() < frame_size) {
        return DecodedFrame<RequestView>{};
    }
    if (load_u16(input, 4) != kProtocolVersion) {
        return fail(ErrorCode::invalid_record, "request protocol version is unsupported");
    }
    if (input[7] != std::byte{0} || load_u32(input, 36) != 0) {
        return fail(ErrorCode::invalid_record, "request flags or reserved field are not canonical");
    }
    auto opcode = request_opcode(std::to_integer<std::uint8_t>(input[6]));
    if (!opcode) {
        return unexpected(opcode.error());
    }
    const auto key_size = static_cast<std::size_t>(load_u32(input, 16));
    const auto value_size = static_cast<std::size_t>(load_u32(input, 20));
    const auto expected_size = checked_frame_size(kRequestHeaderBytes, key_size, value_size);
    if (!expected_size) {
        return unexpected(expected_size.error());
    }
    if (*expected_size != frame_size) {
        return fail(ErrorCode::invalid_record, "request payload sizes do not match frame size");
    }
    const auto key = input.subspan(kRequestHeaderBytes, key_size);
    const auto value = input.subspan(kRequestHeaderBytes + key_size, value_size);
    RequestView frame{.opcode = *opcode,
                      .flags = std::to_integer<std::uint8_t>(input[7]),
                      .request_id = load_u64(input, 8),
                      .expire_at_ns = load_u64(input, 24),
                      .target_worker = load_u32(input, 32),
                      .key = key,
                      .value = value};
    if (auto valid = validate_request_fields(frame); !valid) {
        return unexpected(valid.error());
    }
    return DecodedFrame<RequestView>{.complete = true, .consumed = frame_size, .frame = frame};
}

auto decode_response(const std::span<const std::byte> input, const std::size_t maximum_frame_bytes)
    -> Result<DecodedFrame<ResponseView>> {
    if (input.size() < sizeof(std::uint32_t)) {
        return DecodedFrame<ResponseView>{};
    }
    const auto frame_size = static_cast<std::size_t>(load_u32(input, 0));
    if (frame_size < kResponseHeaderBytes || frame_size > maximum_frame_bytes) {
        return fail(ErrorCode::invalid_record, "response frame size is outside protocol limits");
    }
    if (input.size() < frame_size) {
        return DecodedFrame<ResponseView>{};
    }
    if (load_u16(input, 4) != kProtocolVersion) {
        return fail(ErrorCode::invalid_record, "response protocol version is unsupported");
    }
    if (load_u32(input, 28) != 0) {
        return fail(ErrorCode::invalid_record, "response reserved field is not canonical");
    }
    auto status = response_status(load_u16(input, 6));
    if (!status) {
        return unexpected(status.error());
    }
    const auto value_size = static_cast<std::size_t>(load_u32(input, 16));
    const auto expected_size = checked_frame_size(kResponseHeaderBytes, value_size, 0);
    if (!expected_size || *expected_size != frame_size) {
        return fail(ErrorCode::invalid_record, "response payload size does not match frame size");
    }
    return DecodedFrame<ResponseView>{
        .complete = true,
        .consumed = frame_size,
        .frame = ResponseView{.status = *status,
                              .request_id = load_u64(input, 8),
                              .owner_worker = load_u32(input, 20),
                              .worker_count = load_u32(input, 24),
                              .routing_epoch = load_u64(input, 32),
                              .value = input.subspan(kResponseHeaderBytes, value_size)}};
}

auto encoded_request_size(const RequestView& request) -> Result<std::size_t> {
    if (request.flags != 0) {
        return fail(ErrorCode::invalid_argument, "request flags are not defined in protocol v2");
    }
    if (auto valid = validate_request_fields(request); !valid) {
        return unexpected(valid.error());
    }
    const auto frame_size = checked_frame_size(kRequestHeaderBytes, request.key.size(), request.value.size());
    if (!frame_size || *frame_size > kMaxFrameBytes) {
        return fail(ErrorCode::record_too_large, "request exceeds protocol frame limits");
    }
    return *frame_size;
}

auto encode_request(const std::span<std::byte> output, const RequestView& request) -> Result<std::size_t> {
    const auto frame_size = encoded_request_size(request);
    if (!frame_size) {
        return unexpected(frame_size.error());
    }
    if (output.size() < *frame_size) {
        return fail(ErrorCode::invalid_argument, "request destination is smaller than encoded frame");
    }
    auto encoded_frame_size = checked_u32(*frame_size, "request frame");
    auto encoded_key_size = checked_u32(request.key.size(), "request key");
    auto encoded_value_size = checked_u32(request.value.size(), "request value");
    if (!encoded_frame_size || !encoded_key_size || !encoded_value_size) {
        return fail(ErrorCode::record_too_large, "request fields exceed protocol limits");
    }
    const auto bytes = output.first(*frame_size);
    store_u32(bytes, 0, *encoded_frame_size);
    store_u16(bytes, 4, kProtocolVersion);
    bytes[6] = static_cast<std::byte>(request.opcode);
    bytes[7] = static_cast<std::byte>(request.flags);
    store_u64(bytes, 8, request.request_id);
    store_u32(bytes, 16, *encoded_key_size);
    store_u32(bytes, 20, *encoded_value_size);
    store_u64(bytes, 24, request.expire_at_ns);
    store_u32(bytes, 32, request.target_worker);
    store_u32(bytes, 36, 0);
    std::ranges::copy(request.key, bytes.begin() + static_cast<std::ptrdiff_t>(kRequestHeaderBytes));
    std::ranges::copy(request.value,
                      bytes.begin() + static_cast<std::ptrdiff_t>(kRequestHeaderBytes + request.key.size()));
    return *frame_size;
}

auto encode_request(const RequestView& request) -> Result<std::vector<std::byte>> {
    const auto frame_size = encoded_request_size(request);
    if (!frame_size) {
        return unexpected(frame_size.error());
    }
    std::vector<std::byte> output(*frame_size);
    if (auto encoded = encode_request(output, request); !encoded) {
        return unexpected(encoded.error());
    }
    return output;
}

auto encoded_response_size(const ResponseView& response) -> Result<std::size_t> {
    const auto frame_size = checked_frame_size(kResponseHeaderBytes, response.value.size(), 0);
    if (!frame_size || *frame_size > kMaxFrameBytes) {
        return fail(ErrorCode::record_too_large, "response exceeds protocol frame limits");
    }
    return *frame_size;
}

auto encode_response_header(const std::span<std::byte> output, const ResponseView& response)
    -> Result<std::size_t> {
    const auto frame_size = encoded_response_size(response);
    if (!frame_size) {
        return unexpected(frame_size.error());
    }
    if (output.size() < kResponseHeaderBytes) {
        return fail(ErrorCode::invalid_argument, "response destination is smaller than header");
    }
    auto encoded_frame_size = checked_u32(*frame_size, "response frame");
    auto encoded_value_size = checked_u32(response.value.size(), "response value");
    if (!encoded_frame_size || !encoded_value_size) {
        return fail(ErrorCode::record_too_large, "response fields exceed protocol limits");
    }
    const auto bytes = output.first(kResponseHeaderBytes);
    store_u32(bytes, 0, *encoded_frame_size);
    store_u16(bytes, 4, kProtocolVersion);
    store_u16(bytes, 6, static_cast<std::uint16_t>(response.status));
    store_u64(bytes, 8, response.request_id);
    store_u32(bytes, 16, *encoded_value_size);
    store_u32(bytes, 20, response.owner_worker);
    store_u32(bytes, 24, response.worker_count);
    store_u32(bytes, 28, 0);
    store_u64(bytes, 32, response.routing_epoch);
    return *frame_size;
}

auto encode_response(const std::span<std::byte> output, const ResponseView& response) -> Result<std::size_t> {
    const auto frame_size = encode_response_header(output, response);
    if (!frame_size) {
        return unexpected(frame_size.error());
    }
    if (output.size() < *frame_size) {
        return fail(ErrorCode::invalid_argument, "response destination is smaller than encoded frame");
    }
    std::ranges::copy(response.value, output.begin() + static_cast<std::ptrdiff_t>(kResponseHeaderBytes));
    return *frame_size;
}

auto encode_response(const ResponseView& response) -> Result<std::vector<std::byte>> {
    const auto frame_size = encoded_response_size(response);
    if (!frame_size) {
        return unexpected(frame_size.error());
    }
    std::vector<std::byte> output(*frame_size);
    if (auto encoded = encode_response(output, response); !encoded) {
        return unexpected(encoded.error());
    }
    return output;
}

auto request_opcode_name(const RequestOpcode opcode) noexcept -> std::string_view {
    switch (opcode) {
    case RequestOpcode::init:
        return "init";
    case RequestOpcode::ping:
        return "ping";
    case RequestOpcode::get:
        return "get";
    case RequestOpcode::put:
        return "put";
    case RequestOpcode::erase:
        return "erase";
    case RequestOpcode::bind_worker:
        return "bind_worker";
    case RequestOpcode::health:
        return "health";
    case RequestOpcode::ready:
        return "ready";
    case RequestOpcode::stats:
        return "stats";
    case RequestOpcode::backup:
        return "backup";
    }
    return "unknown";
}

} // namespace glifistore::server
