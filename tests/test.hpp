#pragma once

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace glifistore::test {

using TestFunction = void (*)();

struct Case {
    std::string name;
    TestFunction function;
};

inline auto registry() -> std::vector<Case>& {
    static std::vector<Case> cases;
    return cases;
}

struct Register {
    Register(std::string name, TestFunction function) {
        registry().push_back({std::move(name), function});
    }
};

inline void require(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        throw std::runtime_error(std::string{file} + ':' + std::to_string(line) +
                                 " requirement failed: " + expression);
    }
}

} // namespace glifistore::test

#define GLIFI_TEST_JOIN_IMPL(a, b) a##b
#define GLIFI_TEST_JOIN(a, b) GLIFI_TEST_JOIN_IMPL(a, b)
#define GLIFI_TEST(name)                                                                                     \
    static void GLIFI_TEST_JOIN(test_function_, __LINE__)();                                                 \
    static ::glifistore::test::Register GLIFI_TEST_JOIN(test_registration_, __LINE__){                       \
        name, &GLIFI_TEST_JOIN(test_function_, __LINE__)};                                                   \
    static void GLIFI_TEST_JOIN(test_function_, __LINE__)()
#define GLIFI_REQUIRE(expression) ::glifistore::test::require((expression), #expression, __FILE__, __LINE__)
