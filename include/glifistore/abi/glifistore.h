/**
 * @file
 * @brief Versioned C ABI with opaque Store handles and caller-owned byte buffers.
 */
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

/**
 * @brief Opaque owning Store handle returned by gs_store_open().
 * @note Operations may run concurrently while the handle is alive. The caller must
 * externally serialize gs_store_close() with every use of the handle.
 */
typedef struct gs_store gs_store;

/**
 * @brief Non-owning binary byte range. data may be NULL only when size is zero.
 */
typedef struct gs_bytes_view {
    const uint8_t* data; ///< Borrowed bytes; binary data is not NUL-terminated.
    size_t size;         ///< Byte length of the borrowed range.
} gs_bytes_view;

/**
 * @brief Fixed-layout status; code is authoritative and reserved remains zero.
 */
typedef struct gs_status {
    uint32_t code;         ///< One of the GS_* status-code constants.
    uint32_t category;     ///< One of the GS_CATEGORY_* constants.
    uint32_t retryability; ///< One of the GS_RETRY_* hints; not a guarantee of safe mutation replay.
    uint32_t reserved;     ///< Must be zero.
} gs_status;

/**
 * @brief Mutation status plus explicit commit knowledge; failure is not proof of rollback.
 */
typedef struct gs_mutation_result {
    gs_status status;     ///< Programmatic status accompanying the outcome.
    uint32_t outcome;     ///< One of the GS_MUTATION_* outcome constants.
    uint32_t reserved[3]; ///< Reserved for ABI evolution; zero.
} gs_mutation_result;

/**
 * @brief ABI-v1 open options. Initialize with gs_store_options_init(); reserved fields must remain zero.
 */
typedef struct gs_store_options {
    uint32_t struct_size;         ///< Exact sizeof of the containing ABI-v1 structure.
    uint32_t abi_version;         ///< GS_ABI_VERSION_1 for this options layout.
    uint32_t worker_count;        ///< Explicit fixed Worker count; valid range is 1 through 256.
    uint32_t storage_mode;        ///< GS_STORAGE_VOLATILE or GS_STORAGE_DURABLE_SYNC.
    uint32_t durable_open_mode;   ///< One of the GS_OPEN_* modes.
    uint32_t reserved0;           ///< Must be zero.
    gs_bytes_view data_directory; ///< Borrowed path bytes for durable mode, without a trailing NUL.
    uint64_t reserved[8];         ///< Reserved for ABI evolution; all elements must be zero.
} gs_store_options;

/**
 * @brief Borrowed PUT item; initialize struct_size and zero reserved before submission.
 */
typedef struct gs_put_request {
    uint32_t struct_size;  ///< Exact sizeof of the containing ABI-v1 structure.
    uint32_t reserved;     ///< Must be zero.
    gs_bytes_view key;     ///< Binary key borrowed until the call returns.
    gs_bytes_view value;   ///< Value bytes borrowed until the call returns.
    uint64_t expire_at_ns; ///< Absolute Unix nanoseconds; zero disables expiration.
} gs_put_request;

/**
 * @brief Borrowed ERASE item; initialize struct_size and zero reserved before submission.
 */
typedef struct gs_erase_request {
    uint32_t struct_size; ///< Exact sizeof of the containing ABI-v1 structure.
    uint32_t reserved;    ///< Must be zero.
    gs_bytes_view key;    ///< Binary key borrowed until the call returns.
} gs_erase_request;

/**
 * @brief Return the independently versioned C ABI major version.
 */
GS_API uint32_t gs_abi_major(void) GS_NOEXCEPT;
/**
 * @brief Return the additive C ABI minor version.
 */
GS_API uint32_t gs_abi_minor(void) GS_NOEXCEPT;
/**
 * @brief Return the product major version, independent of ABI/disk/wire versions.
 */
GS_API uint32_t gs_product_version_major(void) GS_NOEXCEPT;
/**
 * @brief Return the product minor version.
 */
GS_API uint32_t gs_product_version_minor(void) GS_NOEXCEPT;
/**
 * @brief Return the product patch version.
 */
GS_API uint32_t gs_product_version_patch(void) GS_NOEXCEPT;
/**
 * @brief Borrow a static NUL-terminated version string; never free or modify it.
 */
GS_API const char* gs_product_version_string(void) GS_NOEXCEPT;

/**
 * @brief Format diagnostic text without allocation.
 * @param status Status whose code selects the diagnostic message.
 * @param[out] buffer Optional destination; NULL performs a size query.
 * @param capacity Destination capacity in bytes; zero performs no write.
 * @return Required bytes including NUL, even when the output was truncated.
 * @note A non-NULL buffer with positive capacity is always NUL-terminated.
 */
GS_API size_t gs_status_message(gs_status status, char* buffer, size_t capacity) GS_NOEXCEPT;

/**
 * @brief Write defaults, struct size, ABI version, and zeroed reserved fields.
 * @param[out] options Writable full-size options object.
 * @return GS_OK, or GS_INVALID_ARGUMENT for NULL.
 * @note Defaults select one Worker and volatile storage.
 */
GS_API gs_status gs_store_options_init(gs_store_options* options) GS_NOEXCEPT;
/**
 * @brief Open an owning Store handle after validation and any required recovery.
 * @param options Initialized options borrowed for this call.
 * @param[out] out_store Receives ownership on success and NULL on failure; must not be NULL.
 * @return GS_OK or a validation, compatibility, allocation, or storage error.
 * @note Release a successful handle exactly once with gs_store_close().
 */
GS_API gs_status gs_store_open(const gs_store_options* options, gs_store** out_store) GS_NOEXCEPT;
/**
 * @brief Drain/close the Store and consume its handle even when shutdown reports an error.
 * @param store Live handle; NULL is invalid.
 * @return Shutdown status; the handle is no longer usable after return.
 * @pre The caller serializes close with all reads, mutations, and other close calls.
 */
GS_API gs_status gs_store_close(gs_store* store) GS_NOEXCEPT;

/**
 * @brief Copy one visible value into caller-owned storage.
 * @param store Live Store handle.
 * @param key Binary key borrowed until return.
 * @param[out] output Destination; NULL is permitted only with zero output_capacity.
 * @param output_capacity Writable byte capacity.
 * @param[out] output_required Required value bytes on a successful lookup; must not be NULL.
 * @return GS_BUFFER_TOO_SMALL without copying when capacity is insufficient; GS_OK
 * for a complete copy, GS_NOT_FOUND for absence/expiry, or another operation error.
 * @note A NULL/zero size query still performs a lookup. Retrying with a larger buffer
 * performs a fresh read and may observe a different value under concurrent mutation.
 */
GS_API gs_status gs_store_get(gs_store* store, gs_bytes_view key, uint8_t* output, size_t output_capacity,
                              size_t* output_required) GS_NOEXCEPT;
/**
 * @brief Publish a value and return explicit mutation knowledge.
 * @param store Live Store handle.
 * @param key Binary key borrowed until return.
 * @param value Bytes borrowed until return.
 * @param expire_at_ns Absolute Unix nanoseconds; zero disables expiration.
 * @return Status and committed/rejected/indeterminate outcome; do not infer rollback from status alone.
 */
GS_API gs_mutation_result gs_store_put(gs_store* store, gs_bytes_view key, gs_bytes_view value,
                                       uint64_t expire_at_ns) GS_NOEXCEPT;
/**
 * @brief Erase key visibility with explicit mutation knowledge.
 * @param store Live Store handle.
 * @param key Binary key borrowed until return.
 * @return Status and outcome; absent/expired keys report GS_NOT_FOUND.
 */
GS_API gs_mutation_result gs_store_erase(gs_store* store, gs_bytes_view key) GS_NOEXCEPT;

/**
 * @brief Apply independent batch items with positional mutation outcomes.
 * @param store Live Store handle.
 * @param requests Array of initialized requests, borrowed until return; may be NULL for zero count.
 * @param request_count Number of items, at most GS_MAX_BATCH_ITEMS.
 * @param[out] results Caller-owned array of request_count entries; may be NULL for zero count.
 * @return Batch-level status. GS_OK means results were produced, not that every item succeeded.
 * @note Structural validation precedes execution. On validation failure results is not
 * populated; callers must inspect each result after successful dispatch. The batch
 * is not a transaction; successful items are not rolled back by other failures.
 */
GS_API gs_status gs_store_put_batch(gs_store* store, const gs_put_request* requests, size_t request_count,
                                    gs_mutation_result* results) GS_NOEXCEPT;
/**
 * @brief Apply independent batch items with positional mutation outcomes.
 * @param store Live Store handle.
 * @param requests Array of initialized requests, borrowed until return; may be NULL for zero count.
 * @param request_count Number of items, at most GS_MAX_BATCH_ITEMS.
 * @param[out] results Caller-owned array of request_count entries; may be NULL for zero count.
 * @return Batch-level status. GS_OK means results were produced, not that every item succeeded.
 * @note Structural validation precedes execution. On validation failure results is not
 * populated; callers must inspect each result after successful dispatch. The batch
 * is not a transaction; successful items are not rolled back by other failures.
 */
GS_API gs_status gs_store_erase_batch(gs_store* store, const gs_erase_request* requests, size_t request_count,
                                      gs_mutation_result* results) GS_NOEXCEPT;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GLIFISTORE_ABI_GLIFISTORE_H */
