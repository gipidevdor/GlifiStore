#include "glifistore/abi/glifistore.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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
    _Static_assert(offsetof(gs_mutation_result, outcome) == 16, "outcome moved");
    _Static_assert(sizeof(gs_store_options) == 104, "gs_store_options layout changed");
    _Static_assert(_Alignof(gs_store_options) == 8, "gs_store_options alignment changed");
    _Static_assert(offsetof(gs_store_options, data_directory) == 24, "data_directory moved");
    _Static_assert(offsetof(gs_store_options, reserved) == 40, "reserved moved");
    _Static_assert(sizeof(gs_put_request) == 48, "gs_put_request layout changed");
    _Static_assert(offsetof(gs_put_request, expire_at_ns) == 40, "expire_at_ns moved");
    _Static_assert(sizeof(gs_erase_request) == 24, "gs_erase_request layout changed");
    _Static_assert(offsetof(gs_erase_request, key) == 8, "gs_erase_request.key moved");
    if (gs_abi_major() != GS_ABI_VERSION_MAJOR || gs_abi_minor() < GS_ABI_VERSION_MINOR) {
        return 1;
    }
    printf("C ABI layout probe PASSED major=%u minor=%u\n", gs_abi_major(), gs_abi_minor());
    return 0;
}
