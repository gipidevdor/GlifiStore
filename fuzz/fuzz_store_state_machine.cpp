#include "support/stateful_store_model.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    glifistore::test::run_stateful_store_model({data, size});
    return 0;
}
