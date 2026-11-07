#include "glifistore/core/key_hash.hpp"
#include "glifistore/store/store.hpp"
#include "integration/persistence_recovery_test_support.hpp"
#include "store/store_internal.hpp"
#include "test.hpp"

#include <span>
#include <string_view>
#include <utility>

GLIFI_TEST("PreparedColdRead transfers live pins through move construction and replacement") {
    using namespace glifistore;
    persistence_recovery_test_support::RecoveryTemporaryDirectory directory;
    auto opened = Store::open({.worker_config = {.explicit_count = 1},
                               .storage_mode = StorageMode::durable_sync,
                               .data_directory = directory.path() / "store",
                               .durable_open_mode = DurableOpenMode::create_new});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    const std::string_view first_value{"first-value"};
    const std::string_view second_value{"second-value"};
    GLIFI_REQUIRE(store.put("first", std::as_bytes(std::span{first_value})).has_value());
    GLIFI_REQUIRE(store.put("second", std::as_bytes(std::span{second_value})).has_value());
    const HashedKey first_key{"first", hash_key("first")};
    const HashedKey second_key{"second", hash_key("second")};
    auto first = detail::StoreAccess::prepare_get_owned(store, 0, first_key, 0);
    auto second = detail::StoreAccess::prepare_get_owned(store, 0, second_key, 0);
    GLIFI_REQUIRE(first.has_value() && first->cold.has_value());
    GLIFI_REQUIRE(second.has_value() && second->cold.has_value());

    auto moved = std::move(*first->cold);
    auto replaced = std::move(*second->cold);
    replaced = std::move(moved);
    // Reconstruct into the storage whose previous State has just been destroyed.
    moved = std::move(replaced);
    auto value = detail::StoreAccess::complete_get_owned(store, 0, std::move(moved));
    GLIFI_REQUIRE(value.has_value());
    GLIFI_REQUIRE(value->bytes.as_vector() ==
                  std::vector<std::byte>(std::as_bytes(std::span{first_value}).begin(),
                                         std::as_bytes(std::span{first_value}).end()));
    GLIFI_REQUIRE(store.close().has_value());
}
