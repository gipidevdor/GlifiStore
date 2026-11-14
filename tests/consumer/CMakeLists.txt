cmake_minimum_required(VERSION 3.25)

project(GlifiStoreConsumer LANGUAGES C CXX)

find_package(GlifiStore 0.1 CONFIG REQUIRED)

add_executable(glifistore_consumer_smoke main.cpp)
target_link_libraries(glifistore_consumer_smoke PRIVATE GlifiStore::core)
target_compile_features(glifistore_consumer_smoke PRIVATE cxx_std_23)

enable_testing()
add_test(NAME glifistore_consumer_smoke COMMAND glifistore_consumer_smoke)

add_executable(glifistore_client_consumer_smoke client.cpp)
target_link_libraries(glifistore_client_consumer_smoke PRIVATE GlifiStore::client)
target_compile_features(glifistore_client_consumer_smoke PRIVATE cxx_std_23)
add_test(NAME glifistore_client_consumer_smoke COMMAND glifistore_client_consumer_smoke)

add_executable(glifistore_abi_consumer_smoke abi.c)
target_link_libraries(glifistore_abi_consumer_smoke PRIVATE GlifiStore::abi)
set_target_properties(glifistore_abi_consumer_smoke PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
add_test(NAME glifistore_abi_consumer_smoke COMMAND glifistore_abi_consumer_smoke)
