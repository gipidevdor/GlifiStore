#include "glifistore/server/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    const auto bytes = std::span<const std::byte>{reinterpret_cast<const std::byte*>(data), size};
    const auto request = glifistore::server::decode_request(bytes);
    if (request && request->complete) {
        static_cast<void>(glifistore::server::encode_request(request->frame));
    }
    const auto response = glifistore::server::decode_response(bytes);
    if (response && response->complete) {
        static_cast<void>(glifistore::server::encode_response(response->frame));
    }
    return 0;
}
