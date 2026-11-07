#include "glifistore/abi/glifistore.h"

#include "glifistore/core/error.hpp"
#include "glifistore/store/store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef GLIFISTORE_PRODUCT_VERSION
#define GLIFISTORE_PRODUCT_VERSION "unknown"
#endif
#ifndef GLIFISTORE_PRODUCT_VERSION_MAJOR
#define GLIFISTORE_PRODUCT_VERSION_MAJOR 0
#define GLIFISTORE_PRODUCT_VERSION_MINOR 0
#define GLIFISTORE_PRODUCT_VERSION_PATCH 0
#endif

struct gs_store final {
    std::unique_ptr<glifistore::Store> value;
};

namespace {

using glifistore::Error;
using glifistore::ErrorCode;

static_assert(sizeof(void*) == 8, "C ABI v1 is defined only for supported 64-bit targets");
static_assert(sizeof(gs_bytes_view) == 16);
static_assert(alignof(gs_bytes_view) == alignof(std::uint64_t));
static_assert(offsetof(gs_bytes_view, data) == 0);
static_assert(offsetof(gs_bytes_view, size) == 8);
static_assert(sizeof(gs_status) == 16);
static_assert(alignof(gs_status) == alignof(std::uint32_t));
static_assert(offsetof(gs_status, code) == 0);
static_assert(offsetof(gs_status, category) == 4);
static_assert(offsetof(gs_status, retryability) == 8);
static_assert(offsetof(gs_status, reserved) == 12);
static_assert(sizeof(gs_mutation_result) == 32);
static_assert(alignof(gs_mutation_result) == alignof(std::uint32_t));
static_assert(offsetof(gs_mutation_result, status) == 0);
static_assert(offsetof(gs_mutation_result, outcome) == 16);
static_assert(offsetof(gs_mutation_result, reserved) == 20);
static_assert(sizeof(gs_store_options) == 104);
static_assert(alignof(gs_store_options) == alignof(std::uint64_t));
static_assert(offsetof(gs_store_options, struct_size) == 0);
static_assert(offsetof(gs_store_options, abi_version) == 4);
static_assert(offsetof(gs_store_options, worker_count) == 8);
static_assert(offsetof(gs_store_options, storage_mode) == 12);
static_assert(offsetof(gs_store_options, durable_open_mode) == 16);
static_assert(offsetof(gs_store_options, reserved0) == 20);
static_assert(offsetof(gs_store_options, data_directory) == 24);
static_assert(offsetof(gs_store_options, reserved) == 24 + sizeof(gs_bytes_view));
static_assert(sizeof(gs_store_options) == offsetof(gs_store_options, reserved) + 8 * sizeof(std::uint64_t));
static_assert(sizeof(gs_put_request) == 48);
static_assert(alignof(gs_put_request) == alignof(std::uint64_t));
static_assert(offsetof(gs_put_request, struct_size) == 0);
static_assert(offsetof(gs_put_request, reserved) == 4);
static_assert(offsetof(gs_put_request, key) == 8);
static_assert(offsetof(gs_put_request, value) == 8 + sizeof(gs_bytes_view));
static_assert(offsetof(gs_put_request, expire_at_ns) == 40);
static_assert(sizeof(gs_erase_request) == 24);
static_assert(alignof(gs_erase_request) == alignof(std::uint64_t));
static_assert(offsetof(gs_erase_request, struct_size) == 0);
static_assert(offsetof(gs_erase_request, reserved) == 4);
static_assert(offsetof(gs_erase_request, key) == 8);

constexpr auto ok_status() noexcept -> gs_status {
    return {GS_OK, GS_CATEGORY_NONE, GS_RETRY_NEVER, 0};
}

constexpr auto make_status(const std::uint32_t code, const std::uint32_t category,
                           const std::uint32_t retryability = GS_RETRY_NEVER) noexcept -> gs_status {
    return {code, category, retryability, 0};
}

auto map_error(const Error& error) noexcept -> gs_status {
    switch (error.code) {
    case ErrorCode::invalid_argument:
    case ErrorCode::arithmetic_overflow:
        return make_status(GS_INVALID_ARGUMENT, GS_CATEGORY_INPUT);
    case ErrorCode::record_too_large:
        return make_status(GS_RECORD_TOO_LARGE, GS_CATEGORY_INPUT);
    case ErrorCode::not_found:
        return make_status(GS_NOT_FOUND, GS_CATEGORY_INPUT);
    case ErrorCode::resource_exhausted:
    case ErrorCode::descriptor_exhausted:
        return make_status(GS_RESOURCE_EXHAUSTED, GS_CATEGORY_RESOURCE, GS_RETRY_AFTER_BACKOFF);
    case ErrorCode::storage_exhausted:
    case ErrorCode::file_too_large:
    case ErrorCode::read_only_filesystem:
        return make_status(GS_STORAGE_EXHAUSTED, GS_CATEGORY_STORAGE);
    case ErrorCode::sequence_conflict:
        return make_status(GS_CONFLICT, GS_CATEGORY_STORAGE, GS_RETRY_MAYBE);
    case ErrorCode::checksum_mismatch:
    case ErrorCode::invalid_record:
    case ErrorCode::invalid_reference:
    case ErrorCode::corrupted_data:
        return make_status(GS_CORRUPTED_DATA, GS_CATEGORY_INTEGRITY);
    case ErrorCode::unavailable:
    case ErrorCode::segment_full:
    case ErrorCode::segment_sealed:
        return make_status(GS_UNAVAILABLE, GS_CATEGORY_LIFECYCLE, GS_RETRY_AFTER_BACKOFF);
    case ErrorCode::io_error:
        return make_status(GS_IO_ERROR, GS_CATEGORY_STORAGE, GS_RETRY_MAYBE);
    case ErrorCode::internal_error:
        return make_status(GS_INTERNAL_ERROR, GS_CATEGORY_INTERNAL);
    }
    return make_status(GS_INTERNAL_ERROR, GS_CATEGORY_INTERNAL);
}

auto map_status(const glifistore::Status& status) noexcept -> gs_status {
    return status ? ok_status() : map_error(status.error());
}

constexpr auto mutation_result(const gs_status status, const std::uint32_t outcome) noexcept
    -> gs_mutation_result {
    return {status, outcome, {0, 0, 0}};
}

constexpr auto engine_mutation_result(const gs_status status) noexcept -> gs_mutation_result {
    // The supported C++ Store status intentionally hides internal commit knowledge.
    // A clean success is committed; any engine error is conservatively indeterminate.
    return mutation_result(status, status.code == GS_OK ? GS_MUTATION_COMMITTED : GS_MUTATION_INDETERMINATE);
}

constexpr auto invalid_argument() noexcept -> gs_status {
    return make_status(GS_INVALID_ARGUMENT, GS_CATEGORY_INPUT);
}

constexpr auto incompatible_abi() noexcept -> gs_status {
    return make_status(GS_INCOMPATIBLE_ABI, GS_CATEGORY_INPUT);
}

constexpr auto resource_exhausted() noexcept -> gs_status {
    return make_status(GS_RESOURCE_EXHAUSTED, GS_CATEGORY_RESOURCE, GS_RETRY_AFTER_BACKOFF);
}

constexpr auto internal_error() noexcept -> gs_status {
    return make_status(GS_INTERNAL_ERROR, GS_CATEGORY_INTERNAL);
}

auto valid_view(const gs_bytes_view view) noexcept -> bool {
    return view.size == 0 || view.data != nullptr;
}

auto byte_span(const gs_bytes_view view) noexcept -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(view.data), view.size};
}

auto key_view(const gs_bytes_view view) noexcept -> std::string_view {
    return view.size == 0 ? std::string_view{}
                          : std::string_view{reinterpret_cast<const char*>(view.data), view.size};
}

auto reserved_bytes_are_zero(const gs_store_options& options) noexcept -> bool {
    if (options.reserved0 != 0) {
        return false;
    }
    const auto reserved_offset = offsetof(gs_store_options, reserved);
    if (options.struct_size <= reserved_offset) {
        return true;
    }
    const auto visible =
        std::min<std::size_t>(options.struct_size - reserved_offset, sizeof(options.reserved));
    const auto* bytes = reinterpret_cast<const unsigned char*>(options.reserved);
    return std::all_of(bytes, bytes + visible, [](const unsigned char value) { return value == 0; });
}

auto validate_options(const gs_store_options* options) noexcept -> gs_status {
    constexpr auto minimum_size = offsetof(gs_store_options, data_directory) + sizeof(gs_bytes_view);
    if (options == nullptr) {
        return invalid_argument();
    }
    if (options->struct_size < minimum_size) {
        return invalid_argument();
    }
    if (options->struct_size > sizeof(gs_store_options) || options->abi_version != GS_ABI_VERSION_1) {
        return incompatible_abi();
    }
    if (options->worker_count == 0 || options->worker_count > glifistore::kMaximumWorkerCount ||
        !valid_view(options->data_directory) || !reserved_bytes_are_zero(*options)) {
        return invalid_argument();
    }
    if (options->storage_mode != GS_STORAGE_VOLATILE && options->storage_mode != GS_STORAGE_DURABLE_SYNC) {
        return invalid_argument();
    }
    if (options->durable_open_mode != GS_OPEN_OR_CREATE && options->durable_open_mode != GS_OPEN_EXISTING &&
        options->durable_open_mode != GS_CREATE_NEW) {
        return invalid_argument();
    }
    if (options->storage_mode == GS_STORAGE_VOLATILE && options->data_directory.size != 0) {
        return invalid_argument();
    }
    if (options->storage_mode == GS_STORAGE_DURABLE_SYNC && options->data_directory.size == 0) {
        return invalid_argument();
    }
    if (options->data_directory.size != 0 &&
        std::memchr(options->data_directory.data, '\0', options->data_directory.size) != nullptr) {
        return invalid_argument();
    }
    return ok_status();
}

auto status_text(const std::uint32_t code) noexcept -> const char* {
    switch (code) {
    case GS_OK:
        return "ok";
    case GS_NOT_FOUND:
        return "not found";
    case GS_INVALID_ARGUMENT:
        return "invalid argument";
    case GS_RESOURCE_EXHAUSTED:
        return "resource exhausted";
    case GS_BUFFER_TOO_SMALL:
        return "buffer too small";
    case GS_UNAVAILABLE:
        return "unavailable";
    case GS_IO_ERROR:
        return "I/O error";
    case GS_CORRUPTED_DATA:
        return "corrupted data";
    case GS_RECORD_TOO_LARGE:
        return "record too large";
    case GS_STORAGE_EXHAUSTED:
        return "storage exhausted";
    case GS_CONFLICT:
        return "conflict";
    case GS_INTERNAL_ERROR:
        return "internal error";
    case GS_INCOMPATIBLE_ABI:
        return "incompatible ABI";
    default:
        return "unknown status";
    }
}

} // namespace

extern "C" GS_API uint32_t gs_abi_major(void) noexcept {
    return GS_ABI_VERSION_MAJOR;
}

extern "C" GS_API uint32_t gs_abi_minor(void) noexcept {
    return GS_ABI_VERSION_MINOR;
}

extern "C" GS_API uint32_t gs_product_version_major(void) noexcept {
    return GLIFISTORE_PRODUCT_VERSION_MAJOR;
}

extern "C" GS_API uint32_t gs_product_version_minor(void) noexcept {
    return GLIFISTORE_PRODUCT_VERSION_MINOR;
}

extern "C" GS_API uint32_t gs_product_version_patch(void) noexcept {
    return GLIFISTORE_PRODUCT_VERSION_PATCH;
}

extern "C" GS_API const char* gs_product_version_string(void) noexcept {
    return GLIFISTORE_PRODUCT_VERSION;
}

extern "C" GS_API size_t gs_status_message(const gs_status status, char* const buffer,
                                           const size_t capacity) noexcept {
    const char* const message = status_text(status.code);
    const auto required = std::strlen(message) + 1U;
    if (buffer != nullptr && capacity != 0) {
        const auto copied = std::min(required - 1U, capacity - 1U);
        std::memcpy(buffer, message, copied);
        buffer[copied] = '\0';
    }
    return required;
}

extern "C" GS_API gs_status gs_store_options_init(gs_store_options* const options) noexcept {
    if (options == nullptr) {
        return invalid_argument();
    }
    std::memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
    options->abi_version = GS_ABI_VERSION_1;
    options->worker_count = 1;
    options->storage_mode = GS_STORAGE_VOLATILE;
    options->durable_open_mode = GS_OPEN_OR_CREATE;
    return ok_status();
}

extern "C" GS_API gs_status gs_store_open(const gs_store_options* const options,
                                          gs_store** const out_store) noexcept try {
    if (out_store == nullptr) {
        return invalid_argument();
    }
    *out_store = nullptr;
    if (const auto valid = validate_options(options); valid.code != GS_OK) {
        return valid;
    }

    glifistore::StoreConfig config;
    config.worker_config.explicit_count = options->worker_count;
    config.concurrency = glifistore::StoreConcurrencyMode::paired;
    config.maintenance.mode = glifistore::MaintenanceMode::cooperative;
    if (options->storage_mode == GS_STORAGE_DURABLE_SYNC) {
        config.storage_mode = glifistore::StorageMode::durable_sync;
        config.data_directory = std::filesystem::path{std::string{
            reinterpret_cast<const char*>(options->data_directory.data), options->data_directory.size}};
        switch (options->durable_open_mode) {
        case GS_OPEN_OR_CREATE:
            config.durable_open_mode = glifistore::DurableOpenMode::open_or_create;
            break;
        case GS_OPEN_EXISTING:
            config.durable_open_mode = glifistore::DurableOpenMode::open_existing;
            break;
        case GS_CREATE_NEW:
            config.durable_open_mode = glifistore::DurableOpenMode::create_new;
            break;
        default:
            return invalid_argument();
        }
    }
    auto opened = glifistore::Store::open(config);
    if (!opened) {
        return map_error(opened.error());
    }
    auto handle = std::make_unique<gs_store>();
    handle->value = std::move(opened.value());
    *out_store = handle.release();
    return ok_status();
} catch (const std::bad_alloc&) {

    return resource_exhausted();
} catch (...) {
    return internal_error();
}

extern "C" GS_API gs_status gs_store_close(gs_store* const store) noexcept try {
    if (store == nullptr) {
        return invalid_argument();
    }
    std::unique_ptr<gs_store> owner{store};
    const auto status = map_status(owner->value->close());
    return status;
} catch (...) {

    return internal_error();
}

extern "C" GS_API gs_status gs_store_get(gs_store* const store, const gs_bytes_view key,
                                         uint8_t* const output, const size_t output_capacity,
                                         size_t* const output_required) noexcept try {
    if (output_required == nullptr) {
        return invalid_argument();
    }
    *output_required = 0;
    if (store == nullptr || !valid_view(key) || (output == nullptr && output_capacity != 0)) {
        return invalid_argument();
    }
    auto value = store->value->get(key_view(key));
    if (!value) {
        return map_error(value.error());
    }
    *output_required = value->bytes.size();
    if (value->bytes.size() > output_capacity) {
        return make_status(GS_BUFFER_TOO_SMALL, GS_CATEGORY_RESOURCE);
    }
    if (value->bytes.size() != 0) {
        std::memcpy(output, value->bytes.data(), value->bytes.size());
    }
    return ok_status();
} catch (const std::bad_alloc&) {

    return resource_exhausted();
} catch (...) {
    return internal_error();
}

extern "C" GS_API gs_mutation_result gs_store_put(gs_store* const store, const gs_bytes_view key,
                                                  const gs_bytes_view value,
                                                  const uint64_t expire_at_ns) noexcept try {
    if (store == nullptr || !valid_view(key) || !valid_view(value)) {
        return mutation_result(invalid_argument(), GS_MUTATION_REJECTED);
    }
    return engine_mutation_result(
        map_status(store->value->put(key_view(key), byte_span(value), expire_at_ns)));
} catch (const std::bad_alloc&) {

    return mutation_result(resource_exhausted(), GS_MUTATION_INDETERMINATE);
} catch (...) {
    return mutation_result(internal_error(), GS_MUTATION_INDETERMINATE);
}

extern "C" GS_API gs_mutation_result gs_store_erase(gs_store* const store, const gs_bytes_view key) noexcept
    try {
    if (store == nullptr || !valid_view(key)) {
        return mutation_result(invalid_argument(), GS_MUTATION_REJECTED);
    }
    return engine_mutation_result(map_status(store->value->erase(key_view(key))));
} catch (const std::bad_alloc&) {

    return mutation_result(resource_exhausted(), GS_MUTATION_INDETERMINATE);
} catch (...) {
    return mutation_result(internal_error(), GS_MUTATION_INDETERMINATE);
}

extern "C" GS_API gs_status gs_store_put_batch(gs_store* const store, const gs_put_request* const requests,
                                               const size_t request_count,
                                               gs_mutation_result* const results) noexcept try {
    if (store == nullptr || request_count > GS_MAX_BATCH_ITEMS ||
        (request_count != 0 && (requests == nullptr || results == nullptr))) {
        return invalid_argument();
    }
    for (size_t index = 0; index < request_count; ++index) {
        const auto& request = requests[index];
        if (request.struct_size != sizeof(gs_put_request) || request.reserved != 0 ||
            !valid_view(request.key) || !valid_view(request.value)) {
            return invalid_argument();
        }
    }

    std::vector<glifistore::Store::PutItem> items;
    items.reserve(request_count);
    for (size_t index = 0; index < request_count; ++index) {
        items.push_back({.key = key_view(requests[index].key),
                         .value = byte_span(requests[index].value),
                         .expire_at_ns = requests[index].expire_at_ns});
    }
    auto statuses = store->value->put_batch(items);
    if (statuses.size() != request_count) {
        for (size_t index = 0; index < request_count; ++index) {
            results[index] = mutation_result(internal_error(), GS_MUTATION_INDETERMINATE);
        }
        return internal_error();
    }
    for (size_t index = 0; index < request_count; ++index) {
        results[index] = engine_mutation_result(map_status(statuses[index]));
    }
    return ok_status();
} catch (const std::bad_alloc&) {

    if (results != nullptr) {
        for (size_t index = 0; index < request_count; ++index) {
            results[index] = mutation_result(resource_exhausted(), GS_MUTATION_INDETERMINATE);
        }
    }
    return resource_exhausted();
} catch (...) {
    if (results != nullptr) {
        for (size_t index = 0; index < request_count; ++index) {
            results[index] = mutation_result(internal_error(), GS_MUTATION_INDETERMINATE);
        }
    }
    return internal_error();
}

extern "C" GS_API gs_status gs_store_erase_batch(gs_store* const store,
                                                 const gs_erase_request* const requests,
                                                 const size_t request_count,
                                                 gs_mutation_result* const results) noexcept try {
    if (store == nullptr || request_count > GS_MAX_BATCH_ITEMS ||
        (request_count != 0 && (requests == nullptr || results == nullptr))) {
        return invalid_argument();
    }
    for (size_t index = 0; index < request_count; ++index) {
        const auto& request = requests[index];
        if (request.struct_size != sizeof(gs_erase_request) || request.reserved != 0 ||
            !valid_view(request.key)) {
            return invalid_argument();
        }
    }

    std::vector<glifistore::Store::EraseItem> items;
    items.reserve(request_count);
    for (size_t index = 0; index < request_count; ++index) {
        items.push_back({.key = key_view(requests[index].key)});
    }
    auto statuses = store->value->erase_batch(items);
    if (statuses.size() != request_count) {
        for (size_t index = 0; index < request_count; ++index) {
            results[index] = mutation_result(internal_error(), GS_MUTATION_INDETERMINATE);
        }
        return internal_error();
    }
    for (size_t index = 0; index < request_count; ++index) {
        results[index] = engine_mutation_result(map_status(statuses[index]));
    }
    return ok_status();
} catch (const std::bad_alloc&) {

    if (results != nullptr) {
        for (size_t index = 0; index < request_count; ++index) {
            results[index] = mutation_result(resource_exhausted(), GS_MUTATION_INDETERMINATE);
        }
    }
    return resource_exhausted();
} catch (...) {
    if (results != nullptr) {
        for (size_t index = 0; index < request_count; ++index) {
            results[index] = mutation_result(internal_error(), GS_MUTATION_INDETERMINATE);
        }
    }
    return internal_error();
}
