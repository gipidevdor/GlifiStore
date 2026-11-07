/* Embedded C ABI smoke test for an installed GlifiStore package.
 *
 * Opens a Store with the ABI defaults through the installed C ABI, writes and
 * reads back one record, then closes it. Linked through the installed
 * pkg-config file, so a wrong prefix in glifistore-abi.pc fails here rather
 * than silently falling back to a source-tree path.
 */
#include <glifistore/abi/glifistore.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    gs_store_options options;
    if (gs_store_options_init(&options).code != GS_OK) {
        return 2;
    }

    gs_store* store = NULL;
    if (gs_store_open(&options, &store).code != GS_OK || store == NULL) {
        (void)fprintf(stderr, "gs_store_open failed\n");
        return 3;
    }

    const char key[] = "packaged-c-abi-consumer";
    const char value[] = "installed-prefix-only";
    const gs_bytes_view key_view = {(const uint8_t*)key, sizeof(key) - 1};
    const gs_bytes_view value_view = {(const uint8_t*)value, sizeof(value) - 1};

    const gs_mutation_result put = gs_store_put(store, key_view, value_view, 0);
    if (put.status.code != GS_OK || put.outcome != GS_MUTATION_COMMITTED) {
        (void)gs_store_close(store);
        return 4;
    }

    uint8_t observed[sizeof(value) - 1];
    size_t required = 0;
    if (gs_store_get(store, key_view, observed, sizeof(observed), &required).code != GS_OK ||
        required != sizeof(observed) || memcmp(observed, value, sizeof(observed)) != 0) {
        (void)gs_store_close(store);
        return 5;
    }

    if (gs_store_close(store).code != GS_OK) {
        return 6;
    }
    (void)printf("packaged C ABI consumer OK (ABI %u.%u, product %s)\n", gs_abi_major(), gs_abi_minor(),
                 gs_product_version_string());
    return 0;
}
