/**
 * @file
 * @brief Wire protocol v2 framing with borrowed decoded payload views.
 */
#pragma once

#include "glifistore/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace glifistore::server {

inline constexpr std::uint16_t kProtocolVersion = 2;
inline constexpr std::size_t kRequestHeaderBytes = 40;
inline constexpr std::size_t kResponseHeaderBytes = 40;
inline constexpr std::size_t kMaxFrameBytes = std::size_t{2} * 1024U * 1024U;
inline constexpr std::uint32_t kNoWorker = 0xFFFF'FFFFU;

/**
 * @brief Wire-v2 operation numbers; numeric values are protocol compatibility data.
 */
enum class RequestOpcode : std::uint8_t {
    init = 1,
    ping = 2,
    get = 3,
    put = 4,
    erase = 5,
    bind_worker = 6,
    health = 7,
    ready = 8,
    stats = 9,
    backup = 10,
};
/**
 * @brief Wire-v2 status numbers; transport failure has no synthetic received status.
 */
enum class ResponseStatus : std::uint16_t {
    ok = 0,
    invalid_request = 1,
    unsupported = 2,
    internal_error = 3,
    not_found = 4,
    overloaded = 5,
    wrong_owner = 6,
    not_bound = 7,
    permission_denied = 8
};

/**
 * @brief Wire request metadata and payload spans borrowing the encode/decode input buffers.
 */
struct RequestView {
    RequestOpcode opcode{RequestOpcode::init};
    std::uint8_t flags{};
    std::uint64_t request_id{};             ///< Caller correlation id echoed by the response.
    std::uint64_t expire_at_ns{};           ///< Absolute Unix nanoseconds; zero disables PUT expiration.
    std::uint32_t target_worker{kNoWorker}; ///< Requested bound owner, or the protocol sentinel.
    std::span<const std::byte> key{};       ///< Binary key borrowed from the frame buffer.
    std::span<const std::byte> value{};     ///< Payload borrowed from the frame buffer.
};

/**
 * @brief Wire response metadata and payload span; it carries no ownership of value bytes.
 */
struct ResponseView {
    ResponseStatus status{ResponseStatus::ok};
    std::uint64_t request_id{}; ///< Caller correlation id echoed by the response.
    std::uint32_t owner_worker{kNoWorker};
    std::uint32_t worker_count{};
    std::uint64_t routing_epoch{};
    std::span<const std::byte> value{}; ///< Payload borrowed from the frame buffer.
};

/**
 * @brief Incremental decode result without payload ownership.
 * @tparam View Borrowed request or response view.
 * @note complete == false is a successful need-more-input result; consumed is then
 * zero and frame must not be used. On completion, consumed identifies one frame;
 * trailing input is left for the next decode. Keep input bytes stable while using frame.
 */
template <typename View> struct DecodedFrame {
    bool complete{};        ///< Whether a complete validated frame was decoded.
    std::size_t consumed{}; ///< Input bytes consumed by that frame; zero while incomplete.
    View frame{};           ///< Borrowed decoded view, usable only when complete.
};

[[nodiscard]] auto request_opcode_name(RequestOpcode opcode) noexcept -> std::string_view;

/**
 * @brief Decode one bounded wire-v2 frame without copying its payload.
 * @param input Stable borrowed bytes, possibly including partial or multiple frames.
 * @param maximum_frame_bytes Inclusive declared frame-size bound.
 * @return An incomplete successful result, one validated frame, or a malformed/oversize error.
 * @note No payload view is valid after its source buffer is modified or destroyed.
 */
[[nodiscard]] auto decode_request(std::span<const std::byte> input,
                                  std::size_t maximum_frame_bytes = kMaxFrameBytes)
    -> Result<DecodedFrame<RequestView>>;
/**
 * @brief Decode one bounded wire-v2 frame without copying its payload.
 * @param input Stable borrowed bytes, possibly including partial or multiple frames.
 * @param maximum_frame_bytes Inclusive declared frame-size bound.
 * @return An incomplete successful result, one validated frame, or a malformed/oversize error.
 * @note No payload view is valid after its source buffer is modified or destroyed.
 */
[[nodiscard]] auto decode_response(std::span<const std::byte> input,
                                   std::size_t maximum_frame_bytes = kMaxFrameBytes)
    -> Result<DecodedFrame<ResponseView>>;

/**
 * @brief Validate encodable fields and compute the complete frame size with checked arithmetic.
 */
[[nodiscard]] auto encoded_request_size(const RequestView& request) -> Result<std::size_t>;
/**
 * @brief Encode into caller-owned storage; return bytes written or a validation/capacity error.
 */
[[nodiscard]] auto encode_request(std::span<std::byte> output, const RequestView& request)
    -> Result<std::size_t>;
/**
 * @brief Allocate an owning encoded frame; input payload spans are borrowed only for the call.
 */
[[nodiscard]] auto encode_request(const RequestView& request) -> Result<std::vector<std::byte>>;
/**
 * @brief Validate encodable fields and compute the complete frame size with checked arithmetic.
 */
[[nodiscard]] auto encoded_response_size(const ResponseView& response) -> Result<std::size_t>;
/// Encodes only the fixed header while declaring the complete frame size. The
/// caller may send response.value separately with scatter/gather I/O.
[[nodiscard]] auto encode_response_header(std::span<std::byte> output, const ResponseView& response)
    -> Result<std::size_t>;
/**
 * @brief Encode into caller-owned storage; return bytes written or a validation/capacity error.
 */
[[nodiscard]] auto encode_response(std::span<std::byte> output, const ResponseView& response)
    -> Result<std::size_t>;
/**
 * @brief Allocate an owning encoded frame; input payload spans are borrowed only for the call.
 */
[[nodiscard]] auto encode_response(const ResponseView& response) -> Result<std::vector<std::byte>>;

} // namespace glifistore::server
