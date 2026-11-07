#include "glifistore/abi/glifistore.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(expr)                                                                                        \
    do {                                                                                                     \
        if (!(expr)) {                                                                                       \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #expr);                 \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)

static gs_bytes_view view(const void* data, size_t size) {
    gs_bytes_view result;
    result.data = (const uint8_t*)data;
    result.size = size;
    return result;
}

int main(void) {
    _Static_assert(sizeof(void*) == 8, "C ABI v1 requires a 64-bit target");
    _Static_assert(sizeof(gs_bytes_view) == 16, "gs_bytes_view layout changed");
    _Static_assert(_Alignof(gs_bytes_view) == 8, "gs_bytes_view alignment changed");
    _Static_assert(offsetof(gs_bytes_view, data) == 0, "gs_bytes_view.data moved");
    _Static_assert(offsetof(gs_bytes_view, size) == 8, "gs_bytes_view.size moved");
    _Static_assert(sizeof(gs_status) == 16, "gs_status layout changed");
    _Static_assert(_Alignof(gs_status) == 4, "gs_status alignment changed");
    _Static_assert(offsetof(gs_status, reserved) == 12, "gs_status.reserved moved");
    _Static_assert(sizeof(gs_mutation_result) == 32, "gs_mutation_result layout changed");
    _Static_assert(offsetof(gs_mutation_result, outcome) == 16, "gs_mutation_result.outcome moved");
    _Static_assert(sizeof(gs_store_options) == 104, "gs_store_options layout changed");
    _Static_assert(_Alignof(gs_store_options) == 8, "gs_store_options alignment changed");
    _Static_assert(offsetof(gs_store_options, data_directory) == 24, "gs_store_options.data_directory moved");
    _Static_assert(offsetof(gs_store_options, reserved) == 40, "gs_store_options.reserved moved");
    _Static_assert(sizeof(gs_put_request) == 48, "gs_put_request layout changed");
    _Static_assert(offsetof(gs_put_request, expire_at_ns) == 40, "gs_put_request.expire_at_ns moved");
    _Static_assert(sizeof(gs_erase_request) == 24, "gs_erase_request layout changed");
    _Static_assert(offsetof(gs_erase_request, key) == 8, "gs_erase_request.key moved");
    REQUIRE(gs_abi_major() == GS_ABI_VERSION_MAJOR);
    REQUIRE(gs_abi_minor() == GS_ABI_VERSION_MINOR);
    REQUIRE(gs_product_version_major() == 0u);
    REQUIRE(gs_product_version_minor() == 1u);
    REQUIRE(gs_product_version_patch() == 0u);
    REQUIRE(gs_product_version_string() != NULL);
    REQUIRE(gs_product_version_string()[0] != '\0');

    gs_store_options options;
    REQUIRE(gs_store_options_init(&options).code == GS_OK);
    REQUIRE(options.struct_size == sizeof(options));
    REQUIRE(options.abi_version == GS_ABI_VERSION_1);

    gs_store* store = NULL;
    REQUIRE(gs_store_open(&options, &store).code == GS_OK);
    REQUIRE(store != NULL);

    const uint8_t binary_key[] = {0x00, 0x42, 0xff};
    const uint8_t value[] = {1, 2, 3, 4};
    gs_mutation_result put =
        gs_store_put(store, view(binary_key, sizeof(binary_key)), view(value, sizeof(value)), 0);
    REQUIRE(put.status.code == GS_OK);
    REQUIRE(put.outcome == GS_MUTATION_COMMITTED);
    const gs_mutation_result invalid = gs_store_put(store, view(NULL, 1), view(value, sizeof(value)), 0);
    REQUIRE(invalid.status.code == GS_INVALID_ARGUMENT);
    REQUIRE(invalid.outcome == GS_MUTATION_REJECTED);

    size_t required = 999;
    REQUIRE(gs_store_get(store, view(binary_key, sizeof(binary_key)), NULL, 0, &required).code ==
            GS_BUFFER_TOO_SMALL);
    REQUIRE(required == sizeof(value));
    uint8_t output[sizeof(value)] = {0};
    REQUIRE(
        gs_store_get(store, view(binary_key, sizeof(binary_key)), output, sizeof(output), &required).code ==
        GS_OK);
    REQUIRE(memcmp(output, value, sizeof(value)) == 0);

    const char key_a[] = "batch-a";
    const char key_b[] = "batch-b";
    const char value_a[] = "alpha";
    const char value_b[] = "beta";
    gs_put_request requests[2];
    memset(requests, 0, sizeof(requests));
    requests[0].struct_size = sizeof(gs_put_request);
    requests[0].key = view(key_a, sizeof(key_a) - 1);
    requests[0].value = view(value_a, sizeof(value_a) - 1);
    requests[1].struct_size = sizeof(gs_put_request);
    requests[1].key = view(key_b, sizeof(key_b) - 1);
    requests[1].value = view(value_b, sizeof(value_b) - 1);
    gs_mutation_result results[2];
    REQUIRE(gs_store_put_batch(store, requests, 2, results).code == GS_OK);
    REQUIRE(results[0].outcome == GS_MUTATION_COMMITTED);
    REQUIRE(results[1].outcome == GS_MUTATION_COMMITTED);

    gs_erase_request erase_requests[2];
    memset(erase_requests, 0, sizeof(erase_requests));
    erase_requests[0].struct_size = sizeof(gs_erase_request);
    erase_requests[0].key = view(key_a, sizeof(key_a) - 1);
    erase_requests[1].struct_size = sizeof(gs_erase_request);
    erase_requests[1].key = view(key_b, sizeof(key_b) - 1);
    gs_mutation_result erase_results[2];
    REQUIRE(gs_store_erase_batch(store, erase_requests, 2, erase_results).code == GS_OK);
    REQUIRE(erase_results[0].outcome == GS_MUTATION_COMMITTED);
    REQUIRE(erase_results[1].outcome == GS_MUTATION_COMMITTED);
    REQUIRE(gs_store_get(store, view(key_a, sizeof(key_a) - 1), output, sizeof(output), &required).code ==
            GS_NOT_FOUND);

    // Same-key FIFO: seed then erase twice; the second observes the first (not_found).
    REQUIRE(
        gs_store_put(store, view(key_a, sizeof(key_a) - 1), view(value_a, sizeof(value_a) - 1), 0).outcome ==
        GS_MUTATION_COMMITTED);
    gs_erase_request same_key_erases[2];
    memset(same_key_erases, 0, sizeof(same_key_erases));
    same_key_erases[0].struct_size = sizeof(gs_erase_request);
    same_key_erases[0].key = view(key_a, sizeof(key_a) - 1);
    same_key_erases[1].struct_size = sizeof(gs_erase_request);
    same_key_erases[1].key = view(key_a, sizeof(key_a) - 1);
    gs_mutation_result same_key_results[2];
    REQUIRE(gs_store_erase_batch(store, same_key_erases, 2, same_key_results).code == GS_OK);
    REQUIRE(same_key_results[0].status.code == GS_OK);
    REQUIRE(same_key_results[0].outcome == GS_MUTATION_COMMITTED);
    REQUIRE(same_key_results[1].status.code == GS_NOT_FOUND);
    // Engine-mapped failures stay indeterminate; REJECTED is reserved for known pre-engine rejects.
    REQUIRE(same_key_results[1].outcome == GS_MUTATION_INDETERMINATE);
    REQUIRE(gs_store_get(store, view(key_a, sizeof(key_a) - 1), output, sizeof(output), &required).code ==
            GS_NOT_FOUND);

    gs_mutation_result erased = gs_store_erase(store, view(binary_key, sizeof(binary_key)));
    REQUIRE(erased.status.code == GS_OK);
    REQUIRE(erased.outcome == GS_MUTATION_COMMITTED);
    REQUIRE(
        gs_store_get(store, view(binary_key, sizeof(binary_key)), output, sizeof(output), &required).code ==
        GS_NOT_FOUND);

    char message[8];
    gs_status too_small = {GS_BUFFER_TOO_SMALL, GS_CATEGORY_RESOURCE, GS_RETRY_NEVER, 0};
    REQUIRE(gs_status_message(too_small, NULL, 0) == strlen("buffer too small") + 1);
    REQUIRE(gs_status_message(too_small, message, sizeof(message)) == strlen("buffer too small") + 1);
    REQUIRE(message[sizeof(message) - 1] == '\0');

    REQUIRE(gs_store_close(store).code == GS_OK);

    options.reserved[0] = 1;
    REQUIRE(gs_store_open(&options, &store).code == GS_INVALID_ARGUMENT);
    REQUIRE(store == NULL);
    return 0;
}
