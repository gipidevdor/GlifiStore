#include "glifistore/persistence/bootstrap.hpp"

#include "glifistore/core/types.hpp"
#include "glifistore/core/worker_routing.hpp"
#include "glifistore/persistence/resource_limits.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "system_error.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/random.h>
#else
#include <unistd.h>
#endif

namespace glifistore {
namespace {

auto new_store_id() -> Result<StoreId> {
    StoreId id{};
    if (::getentropy(id.data(), id.size()) != 0) {
        return persistence_system_error("getentropy(Store ID)");
    }
    if (std::ranges::all_of(id, [](const std::byte byte) { return byte == std::byte{0}; })) {
        return fail(ErrorCode::io_error, "entropy source returned an invalid all-zero Store ID");
    }
    return id;
}

auto initial_manifest(const std::size_t worker_count, const WorkerRoutingState routing,
                      const bool enable_blobs) -> Result<Manifest> {
    if (worker_count == 0 || worker_count > kMaximumWorkerCount ||
        worker_count >= std::numeric_limits<std::uint32_t>::max()) {
        return fail(ErrorCode::invalid_argument, "bootstrap Worker count is outside supported bounds");
    }
    if (auto valid = validate_worker_routing_state(routing); !valid) {
        return unexpected(valid.error());
    }
    auto store_id = new_store_id();
    if (!store_id) {
        return unexpected(store_id.error());
    }
    Manifest manifest{
        .store_id = *store_id,
        .manifest_generation = 1,
        .routing_algorithm = routing.algorithm,
        .worker_count = static_cast<std::uint32_t>(worker_count),
        .routing_epoch = 1,
        .worker_hash_seed = routing.seed,
        .next_segment_id = SegmentId{worker_count + 1U},
        .next_segment_generation = GenerationId{1},
    };
    manifest.format_version = enable_blobs ? 2 : 1;
    manifest.segments.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        manifest.segments.push_back({.segment_id = SegmentId{worker + 1U},
                                     .generation = GenerationId{1},
                                     .owner_worker = WorkerId{static_cast<std::uint32_t>(worker)},
                                     .role = ManifestSegmentRole::active});
    }
    return manifest;
}

auto validate_initial_manifest(const Manifest& manifest) -> Status {
    if (manifest.manifest_generation != 1 || manifest.routing_epoch != 1 ||
        manifest.next_segment_generation != GenerationId{1} ||
        manifest.segments.size() != manifest.worker_count ||
        manifest.next_segment_id.value != static_cast<std::uint64_t>(manifest.worker_count) + 1U) {
        return fail(ErrorCode::corrupted_data, "bootstrap intent is not a canonical initial manifest");
    }
    if (auto routing = validate_worker_routing_state(manifest.worker_routing()); !routing) {
        return unexpected(routing.error());
    }
    for (std::size_t index = 0; index < manifest.segments.size(); ++index) {
        const auto& entry = manifest.segments[index];
        if (entry.segment_id.value != index + 1U || entry.generation != GenerationId{1} ||
            entry.owner_worker.value != index || entry.role != ManifestSegmentRole::active) {
            return fail(ErrorCode::corrupted_data, "bootstrap intent Segment catalog is not canonical");
        }
    }
    return {};
}

auto require_worker_count(const Manifest& manifest, const std::optional<std::size_t> required) -> Status {
    if (required && *required != manifest.worker_count) {
        return fail(ErrorCode::invalid_argument,
                    "requested Worker count disagrees with durable Store metadata");
    }
    return {};
}

auto require_worker_routing(const Manifest& manifest, const WorkerRoutingConfig& requested) -> Status {
    if (!requested.seed_explicit) {
        return {};
    }
    if (manifest.worker_routing() != requested.state()) {
        return fail(ErrorCode::invalid_argument,
                    "requested Worker hash seed disagrees with durable Store metadata");
    }
    return {};
}

auto complete_bootstrap(DataDirectory& directory, const Manifest& intent,
                        const std::optional<std::size_t> required_worker_count,
                        const DurableResourceLimits& limits, const WorkerRoutingConfig& requested_routing)
    -> Status {
    if (auto valid = validate_initial_manifest(intent); !valid) {
        return valid;
    }
    if (auto compatible = require_worker_count(intent, required_worker_count); !compatible) {
        return compatible;
    }
    if (auto routing = require_worker_routing(intent, requested_routing); !routing) {
        return routing;
    }
    if (auto resources = validate_durable_bootstrap_resources(intent.worker_count, limits); !resources) {
        return resources;
    }
    auto published = directory.read_manifest(limits.max_manifest_bytes);
    bool publish_required{};
    if (published) {
        if (*published != intent) {
            return fail(ErrorCode::corrupted_data, "published manifest disagrees with bootstrap intent");
        }
    } else if (published.error().code == ErrorCode::not_found) {
        publish_required = true;
    } else {
        return unexpected(published.error());
    }

    std::vector<bool> missing_segments(intent.segments.size());
    std::size_t missing_count{};
    for (std::size_t index = 0; index < intent.segments.size(); ++index) {
        const auto& entry = intent.segments[index];
        const SegmentHeaderIdentity identity{.store_id = intent.store_id,
                                             .segment_id = entry.segment_id,
                                             .generation = entry.generation,
                                             .owner_worker = entry.owner_worker,
                                             .format_version = intent.format_version};
        auto existing = DurableSegmentFile::open(directory, identity, SegmentFileOpenMode::read_only);
        if (existing) {
            const auto& commit = existing->selected_commit().commit;
            if (commit.commit_generation != 1 || commit.committed_end != kSegmentHeaderReservedBytes ||
                commit.state != PersistedSegmentState::active || commit.record_count != 0) {
                return fail(ErrorCode::corrupted_data,
                            "bootstrap Segment is not in its pristine initial state");
            }
            continue;
        }
        if (existing.error().code != ErrorCode::not_found) {
            return unexpected(existing.error());
        }
        missing_segments[index] = true;
        ++missing_count;
    }

    const auto manifest_bytes = durable_manifest_bytes(intent.segments.size());
    if (!manifest_bytes) {
        return unexpected(manifest_bytes.error());
    }
    if (missing_count > std::numeric_limits<std::uint64_t>::max() / kSegmentSizeBytes) {
        return fail(ErrorCode::arithmetic_overflow, "bootstrap remaining byte requirement overflow");
    }
    auto additional = static_cast<std::uint64_t>(missing_count) * kSegmentSizeBytes;
    if (publish_required) {
        if (*manifest_bytes > std::numeric_limits<std::uint64_t>::max() - additional) {
            return fail(ErrorCode::arithmetic_overflow, "bootstrap remaining byte requirement overflow");
        }
        additional += *manifest_bytes;
    }
    if (auto available = require_durable_available_space(directory, additional, limits); !available) {
        return available;
    }

    if (publish_required) {
        const auto result = directory.publish_manifest(intent, limits.max_manifest_bytes);
        if (!result.durable()) {
            return unexpected(
                result.error.value_or(Error{ErrorCode::io_error, "initial manifest publication failed"}));
        }
    }

    for (std::size_t index = 0; index < intent.segments.size(); ++index) {
        if (!missing_segments[index]) {
            continue;
        }
        const auto& entry = intent.segments[index];
        const SegmentHeaderIdentity identity{.store_id = intent.store_id,
                                             .segment_id = entry.segment_id,
                                             .generation = entry.generation,
                                             .owner_worker = entry.owner_worker,
                                             .format_version = intent.format_version};
        auto created = DurableSegmentFile::create(directory, identity);
        if (!created.durable()) {
            return unexpected(
                created.error.value_or(Error{ErrorCode::io_error, "initial Segment creation failed"}));
        }
    }
    return directory.finish_bootstrap();
}

} // namespace

auto prepare_durable_store(DataDirectory& directory, const DurableOpenMode mode,
                           const std::size_t creation_worker_count,
                           const std::optional<std::size_t> required_worker_count,
                           const DurableResourceLimits& limits, const WorkerRoutingConfig& creation_routing,
                           const bool enable_blobs) -> Status {
    if (auto valid = validate_durable_resource_limits(limits); !valid) {
        return valid;
    }
    if (auto routing = validate_worker_routing_state(creation_routing.state()); !routing) {
        return routing;
    }
    auto intent = directory.read_bootstrap_intent(limits.max_manifest_bytes);
    if (intent) {
        if ((intent->format_version == 2) != enable_blobs) {
            return fail(ErrorCode::invalid_argument, "Store format requires matching enable_blobs");
        }
        return complete_bootstrap(directory, *intent, required_worker_count, limits, creation_routing);
    }
    if (intent.error().code != ErrorCode::not_found) {
        return unexpected(intent.error());
    }

    auto manifest = directory.read_manifest(limits.max_manifest_bytes);
    if (manifest) {
        if ((manifest->format_version == 2) != enable_blobs) {
            return fail(ErrorCode::invalid_argument, "Store format requires matching enable_blobs");
        }
        if (auto resources = validate_durable_manifest_resources(*manifest, limits); !resources) {
            return resources;
        }
        if (auto compatible = require_worker_count(*manifest, required_worker_count); !compatible) {
            return compatible;
        }
        if (auto routing = require_worker_routing(*manifest, creation_routing); !routing) {
            return routing;
        }
        return validate_worker_routing_state(manifest->worker_routing());
    }
    if (manifest.error().code != ErrorCode::not_found) {
        return unexpected(manifest.error());
    }
    if (mode == DurableOpenMode::open_existing) {
        return fail(ErrorCode::not_found, "durable Store manifest does not exist");
    }
    auto pristine = directory.pristine_for_bootstrap();
    if (!pristine) {
        return unexpected(pristine.error());
    }
    if (!*pristine) {
        return fail(ErrorCode::invalid_argument, "refusing to initialize a non-empty data directory");
    }
    auto created = initial_manifest(creation_worker_count, creation_routing.state(), enable_blobs);
    if (!created) {
        return unexpected(created.error());
    }
    if (auto resources = validate_durable_bootstrap_resources(creation_worker_count, limits); !resources) {
        return resources;
    }
    const auto manifest_bytes = durable_manifest_bytes(creation_worker_count);
    if (!manifest_bytes) {
        return unexpected(manifest_bytes.error());
    }
    if (creation_worker_count > std::numeric_limits<std::uint64_t>::max() / kSegmentSizeBytes) {
        return fail(ErrorCode::arithmetic_overflow, "bootstrap byte requirement overflow");
    }
    auto required = static_cast<std::uint64_t>(creation_worker_count) * kSegmentSizeBytes;
    if (*manifest_bytes > (std::numeric_limits<std::uint64_t>::max() - required) / 2U) {
        return fail(ErrorCode::arithmetic_overflow, "bootstrap byte requirement overflow");
    }
    required += *manifest_bytes * 2U;
    if (auto available = require_durable_available_space(directory, required, limits); !available) {
        return available;
    }
    if (auto published = directory.publish_bootstrap_intent(*created, limits.max_manifest_bytes);
        !published) {
        return published;
    }
    return complete_bootstrap(directory, *created, required_worker_count, limits, creation_routing);
}

} // namespace glifistore
