// Protocol-v2 driver for the packaging lifecycle checks.
//
// Built from outside the GlifiStore checkout against an installed package and
// run against the packaged glifistored. Every packaging backend uses this one
// binary for health, PUT, exact GET, ERASE, NOT_FOUND and BACKUP so that the
// checks do not quietly fall back to the in-tree SDKs.
//
// Each invocation performs exactly one operation and reports the outcome through
// the exit status, which keeps the retained logs readable as evidence.

#include <cstdint>
#include <glifistore/client/client.hpp>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr int exit_usage = 2;
constexpr int exit_connect = 3;
constexpr int exit_operation = 4;
constexpr int exit_mismatch = 5;

void usage() {
    std::cerr << "usage: glifistore_daemon_smoke --port N [--host H] [--timeout-ms N]\n"
                 "                               --command "
                 "{health|put|get|erase|expect-not-found|backup}\n"
                 "                               [--key K] [--value V] [--expect V] "
                 "[--destination DIR]\n";
}

auto option(std::string_view name, int& index, int argc, char** argv, std::string& into) -> bool {
    if (name != argv[index]) {
        return false;
    }
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string{name} + " requires a value");
    }
    into = argv[++index];
    return true;
}

auto describe(const glifistore::Error& error) -> std::string {
    std::string text = error.message;
    if (!error.category.empty()) {
        text += " [" + error.category + "]";
    }
    if (error.wire_status) {
        text += " wire_status=" + std::to_string(*error.wire_status);
    }
    return text;
}

} // namespace

int main(int argc, char** argv) try {
    std::string host = "127.0.0.1";
    std::string port;
    std::string timeout_ms = "5000";
    std::string command;
    std::string key;
    std::string value;
    std::string expect;
    std::string destination;

    for (int index = 1; index < argc; ++index) {
        if (option("--host", index, argc, argv, host) || option("--port", index, argc, argv, port) ||
            option("--timeout-ms", index, argc, argv, timeout_ms) ||
            option("--command", index, argc, argv, command) || option("--key", index, argc, argv, key) ||
            option("--value", index, argc, argv, value) || option("--expect", index, argc, argv, expect) ||
            option("--destination", index, argc, argv, destination)) {
            continue;
        }
        usage();
        return exit_usage;
    }
    if (port.empty() || command.empty()) {
        usage();
        return exit_usage;
    }

    const auto parsed_port = std::stoul(port);
    const auto parsed_timeout = std::stoul(timeout_ms);
    if (parsed_port == 0 || parsed_port > 65535) {
        std::cerr << "port out of range: " << port << '\n';
        return exit_usage;
    }

    glifistore::client::ClientConfig config{};
    config.host = host;
    config.port = static_cast<std::uint16_t>(parsed_port);
    config.connect_timeout_ms = static_cast<std::uint32_t>(parsed_timeout);
    config.request_timeout_ms = static_cast<std::uint32_t>(parsed_timeout);

    auto connected = glifistore::client::Client::connect(config);
    if (!connected) {
        std::cerr << "connect failed: " << describe(connected.error()) << '\n';
        return exit_connect;
    }
    auto& client = connected.value();

    if (command == "health") {
        auto pong = client.ping();
        if (!pong) {
            std::cerr << "health failed: " << describe(pong.error()) << '\n';
            return exit_operation;
        }
        if (!client.healthy()) {
            std::cerr << "client reports an unhealthy connection after a successful PING\n";
            return exit_operation;
        }
        std::cout << "health OK workers=" << client.worker_count()
                  << " routing_epoch=" << client.routing_epoch() << '\n';
        return 0;
    }

    if (command == "backup") {
        if (destination.empty()) {
            usage();
            return exit_usage;
        }
        auto done = client.backup(destination);
        if (!done) {
            std::cerr << "backup failed: " << describe(done.error()) << '\n';
            return exit_operation;
        }
        std::cout << "backup OK destination=" << destination << '\n';
        return 0;
    }

    if (key.empty()) {
        usage();
        return exit_usage;
    }

    if (command == "put") {
        const auto result = client.put(std::string_view{key}, std::string_view{value});
        if (!result.committed()) {
            std::cerr << "put not committed: "
                      << (result.error ? describe(*result.error) : "no error reported") << '\n';
            return exit_operation;
        }
        std::cout << "put OK key=" << key << " bytes=" << value.size() << '\n';
        return 0;
    }

    if (command == "get") {
        auto got = client.get(std::string_view{key});
        if (!got) {
            std::cerr << "get failed: " << describe(got.error()) << '\n';
            return exit_operation;
        }
        const auto& bytes = got.value();
        const std::string observed{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        if (!expect.empty() && observed != expect) {
            std::cerr << "get mismatch: expected " << expect.size() << " bytes, observed " << observed.size()
                      << " bytes\n";
            return exit_mismatch;
        }
        std::cout << "get OK key=" << key << " bytes=" << observed.size() << '\n';
        return 0;
    }

    if (command == "erase") {
        const auto result = client.erase(std::string_view{key});
        if (!result.committed()) {
            std::cerr << "erase not committed: "
                      << (result.error ? describe(*result.error) : "no error reported") << '\n';
            return exit_operation;
        }
        std::cout << "erase OK key=" << key << '\n';
        return 0;
    }

    if (command == "expect-not-found") {
        auto got = client.get(std::string_view{key});
        if (got) {
            std::cerr << "expected NOT_FOUND but the key still resolves\n";
            return exit_mismatch;
        }
        if (got.error().code != glifistore::ErrorCode::not_found) {
            std::cerr << "expected NOT_FOUND, observed: " << describe(got.error()) << '\n';
            return exit_mismatch;
        }
        std::cout << "expect-not-found OK key=" << key << '\n';
        return 0;
    }

    usage();
    return exit_usage;
} catch (const std::exception& error) {

    std::cerr << "glifistore_daemon_smoke failed: " << error.what() << '\n';
    return exit_usage;
}
