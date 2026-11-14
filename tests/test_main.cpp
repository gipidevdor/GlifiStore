#include "test.hpp"

#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

struct TestShard {
    std::size_t count{1};
    std::size_t index{0};
};

auto parse_shard_value(std::string_view value, std::string_view variable) -> std::size_t {
    std::size_t parsed = 0;
    const char* const begin = value.data();
    const char* const end = begin + value.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, parsed);
    if (value.empty() || error != std::errc{} || parsed_end != end) {
        throw std::invalid_argument{"invalid " + std::string{variable} +
                                    ": expected an unsigned decimal integer"};
    }
    return parsed;
}

auto read_test_shard(std::size_t test_count) -> TestShard {
    const char* const count_env = std::getenv("GLIFISTORE_TEST_SHARD_COUNT");
    const char* const index_env = std::getenv("GLIFISTORE_TEST_SHARD_INDEX");
    if ((count_env == nullptr) != (index_env == nullptr)) {
        throw std::invalid_argument{
            "GLIFISTORE_TEST_SHARD_COUNT and GLIFISTORE_TEST_SHARD_INDEX must be set together"};
    }
    if (count_env == nullptr) {
        return {};
    }

    const std::size_t count = parse_shard_value(count_env, "GLIFISTORE_TEST_SHARD_COUNT");
    const std::size_t index = parse_shard_value(index_env, "GLIFISTORE_TEST_SHARD_INDEX");
    if (count == 0) {
        throw std::invalid_argument{"GLIFISTORE_TEST_SHARD_COUNT must be greater than zero"};
    }
    if (count > test_count) {
        throw std::invalid_argument{"GLIFISTORE_TEST_SHARD_COUNT must not exceed the registered test count"};
    }
    if (index >= count) {
        throw std::invalid_argument{
            "GLIFISTORE_TEST_SHARD_INDEX must be less than GLIFISTORE_TEST_SHARD_COUNT"};
    }
    return {.count = count, .index = index};
}

auto shard_index_for(std::string_view test_name, std::size_t shard_count) -> std::size_t {
    constexpr std::uint64_t fnv_offset_basis = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t fnv_prime = 1'099'511'628'211ULL;
    std::uint64_t hash = fnv_offset_basis;
    for (const char character : test_name) {
        hash ^= static_cast<unsigned char>(character);
        hash *= fnv_prime;
    }
    return static_cast<std::size_t>(hash % static_cast<std::uint64_t>(shard_count));
}

} // namespace

int main(int argc, char** argv) {
    // Network/TLS tests may write to a peer that already closed (EPIPE). On
    // OpenBSD/BSD this raises SIGPIPE by default and aborts the whole suite.
    // Ignore process-wide; sockets also set SO_NOSIGPIPE where available.
    std::signal(SIGPIPE, SIG_IGN);

    const char* filter_env = std::getenv("GLIFISTORE_TEST_FILTER");
    const std::string_view filter =
        argc > 1 ? std::string_view{argv[1]}
                 : (filter_env != nullptr ? std::string_view{filter_env} : std::string_view{});
    const auto& tests = glifistore::test::registry();
    TestShard shard;
    try {
        shard = read_test_shard(tests.size());
    } catch (const std::invalid_argument& exception) {
        std::cerr << "invalid test shard configuration: " << exception.what() << '\n';
        return 2;
    }
    if (shard.count > 1) {
        std::cout << "test shard " << (shard.index + 1) << '/' << shard.count << '\n';
    }

    int failures = 0;
    int ran = 0;
    for (const auto& test : tests) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) {
            continue;
        }
        if (shard_index_for(test.name, shard.count) != shard.index) {
            continue;
        }
        ++ran;
        std::cout << "[RUN] " << test.name << '\n' << std::flush;
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }
    std::cout << ran << " tests, " << failures << " failures\n";
    if (ran == 0) {
        if (!filter.empty()) {
            std::cerr << "no tests matched filter and shard: " << filter << '\n';
        } else {
            std::cerr << "no tests selected for shard " << (shard.index + 1) << '/' << shard.count << '\n';
        }
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
