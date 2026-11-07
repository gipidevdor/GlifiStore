#include "glifistore/abi/glifistore.h"

#include <string.h>

int main(void) {
    gs_store_options options;
    if (gs_store_options_init(&options).code != GS_OK) {
        return 1;
    }
    gs_store* store = NULL;
    if (gs_store_open(&options, &store).code != GS_OK || store == NULL) {
        return 2;
    }
    const char key[] = "installed-c-consumer";
    const char value[] = "value";
    const gs_bytes_view key_view = {(const uint8_t*)key, sizeof(key) - 1};
    const gs_bytes_view value_view = {(const uint8_t*)value, sizeof(value) - 1};
    const gs_mutation_result put = gs_store_put(store, key_view, value_view, 0);
    if (put.status.code != GS_OK || put.outcome != GS_MUTATION_COMMITTED) {
        (void)gs_store_close(store);
        return 3;
    }
    uint8_t output[sizeof(value) - 1];
    size_t required = 0;
    if (gs_store_get(store, key_view, output, sizeof(output), &required).code != GS_OK ||
        required != sizeof(output) || memcmp(output, value, sizeof(output)) != 0) {
        (void)gs_store_close(store);
        return 4;
    }
    return gs_store_close(store).code == GS_OK ? 0 : 5;
}
