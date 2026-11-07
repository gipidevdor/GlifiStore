#ifndef GLIFISTORE_ABI_GLIFISTORE_H
#define GLIFISTORE_ABI_GLIFISTORE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(GLIFISTORE_ABI_BUILD)
#define GS_API __declspec(dllexport)
#else
#define GS_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define GS_API __attribute__((visibility("default")))
#else
#define GS_API
#endif

#ifdef __cplusplus
#define GS_NOEXCEPT noexcept
extern "C" {
#else
#define GS_NOEXCEPT
#endif

#define GS_ABI_VERSION_MAJOR 1u
#define GS_ABI_VERSION_MINOR 1u
#define GS_ABI_VERSION_1 1u

typedef uint32_t gs_bool;
#define GS_FALSE 0u
#define GS_TRUE 1u

typedef uint32_t gs_status_code;
#define GS_OK 0u
#define GS_NOT_FOUND 1u
#define GS_INVALID_ARGUMENT 2u
#define GS_RESOURCE_EXHAUSTED 3u
#define GS_BUFFER_TOO_SMALL 4u
#define GS_UNAVAILABLE 5u
#define GS_IO_ERROR 6u
#define GS_CORRUPTED_DATA 7u
#define GS_RECORD_TOO_LARGE 8u
#define GS_STORAGE_EXHAUSTED 9u
#define GS_CONFLICT 10u
#define GS_INTERNAL_ERROR 11u
#define GS_INCOMPATIBLE_ABI 12u

typedef uint32_t gs_status_category;
#define GS_CATEGORY_NONE 0u
#define GS_CATEGORY_INPUT 1u
#define GS_CATEGORY_RESOURCE 2u
#define GS_CATEGORY_LIFECYCLE 3u
#define GS_CATEGORY_STORAGE 4u
#define GS_CATEGORY_INTEGRITY 5u
#define GS_CATEGORY_INTERNAL 6u

typedef uint32_t gs_retryability;
#define GS_RETRY_NEVER 0u
#define GS_RETRY_MAYBE 1u
#define GS_RETRY_AFTER_BACKOFF 2u

typedef uint32_t gs_mutation_outcome;
#define GS_MUTATION_COMMITTED 1u
#define GS_MUTATION_REJECTED 2u
#define GS_MUTATION_INDETERMINATE 3u

typedef uint32_t gs_storage_mode;
#define GS_STORAGE_VOLATILE 1u
#define GS_STORAGE_DURABLE_SYNC 2u

typedef uint32_t gs_durable_open_mode;
#define GS_OPEN_OR_CREATE 1u
#define GS_OPEN_EXISTING 2u
#define GS_CREATE_NEW 3u

#define GS_MAX_BATCH_ITEMS 65536u

typedef struct gs_store gs_store;

typedef struct gs_bytes_view {
    const uint8_t* data;
    size_t size;
} gs_bytes_view;

typedef struct gs_status {
    uint32_t code;
    uint32_t category;
    uint32_t retryability;
    uint32_t reserved;
} gs_status;

typedef struct gs_mutation_result {
    gs_status status;
    uint32_t outcome;
    uint32_t reserved[3];
} gs_mutation_result;

typedef struct gs_store_options {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t worker_count;
    uint32_t storage_mode;
    uint32_t durable_open_mode;
    uint32_t reserved0;
    gs_bytes_view data_directory;
    uint64_t reserved[8];
} gs_store_options;

typedef struct gs_put_request {
    uint32_t struct_size;
    uint32_t reserved;
    gs_bytes_view key;
    gs_bytes_view value;
    uint64_t expire_at_ns;
} gs_put_request;

typedef struct gs_erase_request {
    uint32_t struct_size;
    uint32_t reserved;
    gs_bytes_view key;
} gs_erase_request;

GS_API uint32_t gs_abi_major(void) GS_NOEXCEPT;
GS_API uint32_t gs_abi_minor(void) GS_NOEXCEPT;
GS_API uint32_t gs_product_version_major(void) GS_NOEXCEPT;
GS_API uint32_t gs_product_version_minor(void) GS_NOEXCEPT;
GS_API uint32_t gs_product_version_patch(void) GS_NOEXCEPT;
GS_API const char* gs_product_version_string(void) GS_NOEXCEPT;

/* Returns bytes required including the trailing NUL. */
GS_API size_t gs_status_message(gs_status status, char* buffer, size_t capacity) GS_NOEXCEPT;

GS_API gs_status gs_store_options_init(gs_store_options* options) GS_NOEXCEPT;
GS_API gs_status gs_store_open(const gs_store_options* options, gs_store** out_store) GS_NOEXCEPT;
/* Consumes the handle. The caller must externally serialize close with all other calls. */
GS_API gs_status gs_store_close(gs_store* store) GS_NOEXCEPT;

GS_API gs_status gs_store_get(gs_store* store, gs_bytes_view key, uint8_t* output, size_t output_capacity,
                              size_t* output_required) GS_NOEXCEPT;
GS_API gs_mutation_result gs_store_put(gs_store* store, gs_bytes_view key, gs_bytes_view value,
                                       uint64_t expire_at_ns) GS_NOEXCEPT;
GS_API gs_mutation_result gs_store_erase(gs_store* store, gs_bytes_view key) GS_NOEXCEPT;

/* Batch items linearize independently; this is not a transaction. */
GS_API gs_status gs_store_put_batch(gs_store* store, const gs_put_request* requests, size_t request_count,
                                    gs_mutation_result* results) GS_NOEXCEPT;
GS_API gs_status gs_store_erase_batch(gs_store* store, const gs_erase_request* requests, size_t request_count,
                                      gs_mutation_result* results) GS_NOEXCEPT;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GLIFISTORE_ABI_GLIFISTORE_H */
