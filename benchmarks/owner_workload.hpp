#pragma once

#include "glifistore/core/error.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace glifistore::bench {

/// Client bindings and exact key-operation quotas for a finite owner-bound workload.
struct OwnerWorkload {
    std::vector<std::size_t> client_owners;
    std::vector<std::size_t> operations_by_owner;
};

/// Keep every sibling represented when concentrating clients on one owner.
/// Quotas use quotient/remainder arithmetic, including an uneven last client round.
[[nodiscard]] inline auto
owner_workload(const std::size_t workers, const std::size_t clients, const std::size_t operations,
               const std::optional<std::size_t> hot_clients = std::nullopt, const std::size_t hot_worker = 0)
    -> glifistore::Result<OwnerWorkload> {
    if (workers == 0 || clients == 0 || operations == 0) {
        return fail(ErrorCode::invalid_argument, "owner workload needs Workers, clients and operations");
    }
    if (hot_clients && (workers < 2 || hot_worker >= workers || *hot_clients == 0 ||
                        *hot_clients >= clients || clients - *hot_clients < workers - 1U)) {
        return fail(ErrorCode::invalid_argument,
                    "hot Worker clients must leave at least one client for every sibling");
    }
    if (!hot_clients && hot_worker != 0) {
        return fail(ErrorCode::invalid_argument, "--hot-worker requires --hot-worker-clients");
    }
    OwnerWorkload result;
    result.client_owners.reserve(clients);
    result.operations_by_owner.resize(workers);
    const auto per_client = operations / clients;
    const auto remainder = operations % clients;
    for (std::size_t client = 0; client < clients; ++client) {
        auto owner = client % workers;
        if (hot_clients) {
            owner = hot_worker;
            if (client >= *hot_clients) {
                owner = (client - *hot_clients) % (workers - 1U);
                if (owner >= hot_worker) {
                    ++owner;
                }
            }
        }
        result.client_owners.push_back(owner);
        result.operations_by_owner[owner] += per_client + (client < remainder ? 1U : 0U);
    }
    return result;
}

} // namespace glifistore::bench
