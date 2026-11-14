#include "glifistore/core/key_hash.hpp"
#include "glifistore/store/paired/fail_closed_state.hpp"
#include "glifistore/store/paired/mutation_batch.hpp"
#include "glifistore/store/paired/mutation_execution.hpp"
#include "test.hpp"

#include <array>
#include <atomic>

using glifistore::Error;
using glifistore::ErrorCode;
using glifistore::HashedKey;
using glifistore::store::paired::classify_volatile_mutation_error;
using glifistore::store::paired::durable_subbatch_end;
using glifistore::store::paired::FailClosedLaneWake;
using glifistore::store::paired::FailClosedScope;
using glifistore::store::paired::kMaximumPublicationBatch;
using glifistore::store::paired::rewrite_known_not_committed_wire_error;
using glifistore::store::paired::sync_publication_chunk_cap;

GLIFI_TEST("mutation_batch durable_subbatch_end stops before duplicate keys") {
    const std::array keys{
        HashedKey{.key = "a", .hash = 1},
        HashedKey{.key = "b", .hash = 2},
        HashedKey{.key = "a", .hash = 1},
        HashedKey{.key = "c", .hash = 3},
    };
    const auto end = durable_subbatch_end(0, keys.size(),
                                          [&](const std::size_t i) -> const HashedKey& { return keys[i]; });
    GLIFI_REQUIRE(end == 2);
    const auto next = durable_subbatch_end(2, keys.size(),
                                           [&](const std::size_t i) -> const HashedKey& { return keys[i]; });
    GLIFI_REQUIRE(next == 4);
    GLIFI_REQUIRE(sync_publication_chunk_cap(100) == kMaximumPublicationBatch);
    GLIFI_REQUIRE(sync_publication_chunk_cap(7) == 7);
}

GLIFI_TEST("mutation_execution rewrite preserves reject polarity and maps INTERNAL_ERROR bucket") {
    Error exhausted{ErrorCode::resource_exhausted, "keep"};
    rewrite_known_not_committed_wire_error(exhausted);
    GLIFI_REQUIRE(exhausted.code == ErrorCode::resource_exhausted);

    Error io{ErrorCode::io_error, "map"};
    rewrite_known_not_committed_wire_error(io);
    GLIFI_REQUIRE(io.code == ErrorCode::resource_exhausted);

    bool sticky = false;
    auto classified = classify_volatile_mutation_error(Error{ErrorCode::unavailable, "sticky"}, sticky);
    GLIFI_REQUIRE(sticky);
    GLIFI_REQUIRE(classified.code == ErrorCode::unavailable);

    sticky = false;
    classified = classify_volatile_mutation_error(Error{ErrorCode::corrupted_data, "reject"}, sticky);
    GLIFI_REQUIRE(!sticky);
    GLIFI_REQUIRE(classified.code == ErrorCode::resource_exhausted);
}

GLIFI_TEST("FailClosedScope and lane wake views are distinct") {
    std::atomic_uint64_t signal{0};
    const std::array wakes{FailClosedLaneWake{.signal = &signal}, FailClosedLaneWake{}};
    GLIFI_REQUIRE(wakes[0].signal != nullptr);
    GLIFI_REQUIRE(wakes[1].signal == nullptr);
    static_assert(static_cast<std::uint8_t>(FailClosedScope::pair_only) !=
                  static_cast<std::uint8_t>(FailClosedScope::pair_and_store));
}
