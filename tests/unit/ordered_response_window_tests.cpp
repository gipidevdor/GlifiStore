#include "glifistore/server/ordered_response_window.hpp"
#include "test.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

using glifistore::OwnedValue;
using glifistore::server::HeldResponse;
using glifistore::server::kMaximumColdReadWindow;
using glifistore::server::OrderedResponseWindow;
using glifistore::server::ResponseStatus;

namespace {

auto owned(const std::string_view text) -> OwnedValue {
    return OwnedValue::from_bytes(std::as_bytes(std::span<const char>{text.data(), text.size()}), 0, 0);
}

auto text_of(const HeldResponse& response) -> std::string_view {
    const auto bytes = response.value.view();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace

GLIFI_TEST("ordered response window releases responses in request order") {
    OrderedResponseWindow window;
    GLIFI_REQUIRE(window.empty());
    // A decided response can only wait behind an unfinished read.
    GLIFI_REQUIRE(!window.hold(HeldResponse{.request_id = 1}, 40));
    GLIFI_REQUIRE(!window.admit_read(1, 100));
    GLIFI_REQUIRE(window.prepare_storage());

    GLIFI_REQUIRE(window.admit_read(1, 100));
    GLIFI_REQUIRE(
        window.hold(HeldResponse{.status = ResponseStatus::ok, .request_id = 2, .value = owned("two")}, 43));
    GLIFI_REQUIRE(window.admit_read(3, 100));
    GLIFI_REQUIRE(window.hold(HeldResponse{.status = ResponseStatus::not_found, .request_id = 4}, 40));
    GLIFI_REQUIRE(window.size() == 4);
    GLIFI_REQUIRE(window.pending_reads() == 2);
    GLIFI_REQUIRE(window.reserved_bytes() == 283);

    // Nothing leaves while the oldest read is unfinished.
    GLIFI_REQUIRE(!window.pop_decided().has_value());
    // Completions follow submission order: the second read cannot finish first.
    GLIFI_REQUIRE(!window.complete_read(3, HeldResponse{.request_id = 3}));
    GLIFI_REQUIRE(window.complete_read(1, HeldResponse{.request_id = 1, .value = owned("one")}));

    auto first = window.pop_decided();
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE(first->response.request_id == 1);
    GLIFI_REQUIRE(text_of(first->response) == "one");
    GLIFI_REQUIRE(first->reserved_bytes == 100);
    auto second = window.pop_decided();
    GLIFI_REQUIRE(second.has_value());
    GLIFI_REQUIRE(second->response.request_id == 2);
    GLIFI_REQUIRE(text_of(second->response) == "two");
    GLIFI_REQUIRE(!window.pop_decided().has_value());
    GLIFI_REQUIRE(window.reserved_bytes() == 140);

    GLIFI_REQUIRE(
        window.complete_read(3, HeldResponse{.status = ResponseStatus::internal_error, .request_id = 3}));
    auto third = window.pop_decided();
    auto fourth = window.pop_decided();
    GLIFI_REQUIRE(third.has_value());
    GLIFI_REQUIRE(fourth.has_value());
    GLIFI_REQUIRE(third->response.status == ResponseStatus::internal_error);
    GLIFI_REQUIRE(fourth->response.request_id == 4);
    GLIFI_REQUIRE(fourth->response.status == ResponseStatus::not_found);
    GLIFI_REQUIRE(window.empty());
    GLIFI_REQUIRE(window.pending_reads() == 0);
    GLIFI_REQUIRE(window.reserved_bytes() == 0);
    GLIFI_REQUIRE(!window.complete_read(5, HeldResponse{.request_id = 5}));
}

GLIFI_TEST("ordered response window is bounded and clears in-flight reads") {
    OrderedResponseWindow window;
    GLIFI_REQUIRE(window.prepare_storage());
    for (std::uint64_t request = 0; request < kMaximumColdReadWindow; ++request) {
        GLIFI_REQUIRE(window.admit_read(request, 64));
    }
    GLIFI_REQUIRE(window.full());
    GLIFI_REQUIRE(!window.admit_read(kMaximumColdReadWindow, 64));
    GLIFI_REQUIRE(!window.hold(HeldResponse{.request_id = kMaximumColdReadWindow}, 40));

    // Release half, then wrap the ring around its storage.
    for (std::uint64_t request = 0; request < kMaximumColdReadWindow / 2U; ++request) {
        GLIFI_REQUIRE(window.complete_read(request, HeldResponse{.request_id = request}));
        auto released = window.pop_decided();
        GLIFI_REQUIRE(released.has_value());
        GLIFI_REQUIRE(released->response.request_id == request);
    }
    for (std::uint64_t request = kMaximumColdReadWindow; request < kMaximumColdReadWindow * 3U / 2U;
         ++request) {
        GLIFI_REQUIRE(window.hold(HeldResponse{.request_id = request}, 40));
    }
    GLIFI_REQUIRE(window.full());
    GLIFI_REQUIRE(window.pending_reads() == kMaximumColdReadWindow / 2U);

    // Reservations that would overflow are refused before any state changes.
    window.clear();
    GLIFI_REQUIRE(window.empty());
    GLIFI_REQUIRE(window.admit_read(1, static_cast<std::size_t>(-1)));
    GLIFI_REQUIRE(!window.admit_read(2, 1));
    GLIFI_REQUIRE(window.size() == 1);

    // Closing a connection drops unfinished reads; a late completion then has no slot.
    window.clear();
    GLIFI_REQUIRE(window.empty());
    GLIFI_REQUIRE(window.pending_reads() == 0);
    GLIFI_REQUIRE(window.reserved_bytes() == 0);
    GLIFI_REQUIRE(!window.complete_read(1, HeldResponse{.request_id = 1}));
}
