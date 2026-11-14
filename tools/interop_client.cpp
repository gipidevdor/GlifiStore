#include "glifistore/client/client.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] auto hex_nibble(const char ch) -> int {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] auto parse_hex(const std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(text.size() / 2);
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() && (text[index] == ' ' || text[index] == '\n' || text[index] == '\t')) {
            ++index;
        }
        if (index >= text.size()) {
            break;
        }
        if (index + 1 >= text.size()) {
            throw std::runtime_error("odd hex length");
        }
        const int high = hex_nibble(text[index]);
        const int low = hex_nibble(text[index + 1]);
        if (high < 0 || low < 0) {
            throw std::runtime_error("invalid hex");
        }
        out.push_back(static_cast<std::byte>((high << 4) | low));
        index += 2;
    }
    return out;
}

[[nodiscard]] auto to_hex(const std::span<const std::byte> bytes) -> std::string {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value = static_cast<unsigned>(bytes[index]);
        out[index * 2] = kDigits[(value >> 4) & 0xf];
        out[index * 2 + 1] = kDigits[value & 0xf];
    }
    return out;
}

void usage(const char* program) {
    std::cerr << "Usage:\n  " << program
              << " --port PORT [--host HOST] [--tls] [--tls-ca PATH] [--tls-cert PATH] [--tls-key PATH] "
                 "[--server-name NAME] [--insecure-skip-verify]\n"
              << "    put --key-hex HEX --value-hex HEX [--expire-at-ns N]\n  " << program
              << " ... get --key-hex HEX\n  " << program << " ... erase --key-hex HEX\n  " << program
              << " ... pipeline-put-get --key-hex HEX --value-hex HEX\n  " << program
              << " ... worker-pipelines-put-get --key-hex PREFIX_HEX\n  " << program
              << " ... health|ready|stats\n  " << program << " ... backup --dest PATH\n  " << program
              << " ... expect-not-found --key-hex HEX\n  " << program
              << " ... expect-permission-denied --key-hex HEX --value-hex HEX\n  " << program
              << " ... burst-expect-overloaded --key-hex HEX --value-hex HEX [--burst N]\n  " << program
              << " ... expect-frame-limit\n";
}

[[nodiscard]] auto require_flag(const int argc, char** argv, int& index, const char* name)
    -> std::string_view {
    if (index + 1 >= argc) {
        throw std::runtime_error(std::string("missing value for ") + name);
    }
    return argv[++index];
}

} // namespace

int main(int argc, char** argv) try {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    std::string command;
    std::string key_hex;
    std::string value_hex;
    std::string dest;
    std::uint64_t expire_at_ns = 0;
    std::uint64_t burst = 32;
    glifistore::client::TlsOptions tls{};

    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        }
        if (arg == "--host") {
            host = std::string{require_flag(argc, argv, index, "--host")};
        } else if (arg == "--port") {
            port = static_cast<std::uint16_t>(
                std::stoul(std::string{require_flag(argc, argv, index, "--port")}));
        } else if (arg == "--key-hex") {
            key_hex = std::string{require_flag(argc, argv, index, "--key-hex")};
        } else if (arg == "--value-hex") {
            value_hex = std::string{require_flag(argc, argv, index, "--value-hex")};
        } else if (arg == "--dest") {
            dest = std::string{require_flag(argc, argv, index, "--dest")};
        } else if (arg == "--expire-at-ns") {
            expire_at_ns = std::stoull(std::string{require_flag(argc, argv, index, "--expire-at-ns")});
        } else if (arg == "--burst") {
            burst = std::stoull(std::string{require_flag(argc, argv, index, "--burst")});
        } else if (arg == "--tls") {
            tls.enable = true;
        } else if (arg == "--tls-ca") {
            tls.ca_file = std::string{require_flag(argc, argv, index, "--tls-ca")};
        } else if (arg == "--tls-cert") {
            tls.cert_file = std::string{require_flag(argc, argv, index, "--tls-cert")};
        } else if (arg == "--tls-key") {
            tls.key_file = std::string{require_flag(argc, argv, index, "--tls-key")};
        } else if (arg == "--server-name") {
            tls.server_name = std::string{require_flag(argc, argv, index, "--server-name")};
        } else if (arg == "--insecure-skip-verify") {
            tls.insecure_skip_verify = true;
        } else if (arg == "put" || arg == "get" || arg == "erase" || arg == "pipeline-put-get" ||
                   arg == "worker-pipelines-put-get" || arg == "health" || arg == "ready" || arg == "stats" ||
                   arg == "backup" || arg == "expect-not-found" || arg == "expect-permission-denied" ||
                   arg == "burst-expect-overloaded" || arg == "expect-frame-limit") {
            command = std::string{arg};
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            usage(argv[0]);
            return 2;
        }
    }

    if (port == 0 || command.empty()) {
        usage(argv[0]);
        return 2;
    }
    if (burst == 0 || burst > 10'000) {
        throw std::runtime_error("--burst must be between 1 and 10000");
    }

    glifistore::client::ClientConfig config{.host = host, .port = port, .tls = std::move(tls)};
    const auto maximum_frame_bytes = config.maximum_frame_bytes;
    auto client = glifistore::client::Client::connect(std::move(config));
    if (!client) {
        std::cerr << "connect failed: " << client.error().message << '\n';
        return 1;
    }

    const auto key = parse_hex(key_hex);
    if (command == "put") {
        const auto value = parse_hex(value_hex);
        const auto result =
            client->put(key, value, glifistore::client::PutOptions{.expire_at_ns = expire_at_ns});
        if (!result.committed()) {
            std::cerr << "put not committed\n";
            return 1;
        }
        return 0;
    }
    if (command == "get") {
        auto got = client->get(key);
        if (!got) {
            std::cerr << "get failed: " << got.error().message << '\n';
            return 1;
        }
        std::cout << to_hex(*got) << '\n';
        return 0;
    }
    if (command == "erase") {
        const auto result = client->erase(key);
        if (!result.committed()) {
            std::cerr << "erase not committed\n";
            return 1;
        }
        return 0;
    }
    if (command == "pipeline-put-get") {
        const auto value = parse_hex(value_hex);
        const glifistore::client::PipelineRequest requests[] = {
            {.opcode = glifistore::client::PipelineOpcode::put,
             .key = std::span<const std::byte>{key},
             .value = std::span<const std::byte>{value}},
            {.opcode = glifistore::client::PipelineOpcode::get, .key = std::span<const std::byte>{key}},
        };
        auto responses = client->execute_pipeline(requests);
        if (!responses) {
            std::cerr << "pipeline failed: " << responses.error().message << '\n';
            return 1;
        }
        if (responses->size() != 2 || !(*responses)[0].succeeded() || !(*responses)[1].succeeded()) {
            std::cerr << "pipeline outcomes failed\n";
            return 1;
        }
        if ((*responses)[1].value != value) {
            std::cerr << "pipeline value mismatch\n";
            return 1;
        }
        std::cout << to_hex((*responses)[1].value) << '\n';
        return 0;
    }
    if (command == "worker-pipelines-put-get") {
        // --key-hex is a label prefix; discover one owned key per Worker, then fan out.
        const auto worker_count = client->worker_count();
        if (worker_count == 0) {
            std::cerr << "worker pipelines require a positive Worker count\n";
            return 1;
        }
        std::vector<std::vector<std::byte>> owned_keys(worker_count);
        std::vector<bool> found(worker_count, false);
        std::size_t remaining = worker_count;
        for (std::uint64_t candidate = 0; remaining > 0 && candidate < 1'000'000; ++candidate) {
            auto probe = key;
            const auto suffix = "-" + std::to_string(candidate);
            for (const char ch : suffix) {
                probe.push_back(static_cast<std::byte>(ch));
            }
            const auto owner = client->worker_for(probe);
            if (owner >= worker_count || found[owner]) {
                continue;
            }
            owned_keys[owner] = std::move(probe);
            found[owner] = true;
            --remaining;
        }
        if (remaining != 0) {
            std::cerr << "failed to discover a key for every Worker\n";
            return 1;
        }
        std::vector<std::vector<std::byte>> values(worker_count);
        std::vector<std::vector<glifistore::client::PipelineRequest>> batches(worker_count);
        for (std::uint32_t worker = 0; worker < worker_count; ++worker) {
            values[worker] = parse_hex(value_hex);
            if (values[worker].empty()) {
                values[worker] = {static_cast<std::byte>('v'), static_cast<std::byte>('0' + (worker % 10))};
            }
            batches[worker] = {
                {.opcode = glifistore::client::PipelineOpcode::put,
                 .key = std::span<const std::byte>{owned_keys[worker]},
                 .value = std::span<const std::byte>{values[worker]}},
                {.opcode = glifistore::client::PipelineOpcode::get,
                 .key = std::span<const std::byte>{owned_keys[worker]}},
            };
        }
        auto results = client->execute_worker_pipelines(batches);
        if (!results) {
            std::cerr << "worker pipelines failed: " << results.error().message << '\n';
            return 1;
        }
        if (results->size() != worker_count) {
            std::cerr << "worker pipelines returned unexpected slot count\n";
            return 1;
        }
        for (std::uint32_t worker = 0; worker < worker_count; ++worker) {
            const auto& slot = (*results)[worker];
            if (slot.size() != 2 || !slot[0].succeeded() || !slot[1].succeeded() ||
                slot[1].value != values[worker]) {
                std::cerr << "worker " << worker << " pipeline outcomes failed\n";
                return 1;
            }
        }
        std::cout << "ok\n";
        return 0;
    }
    if (command == "health" || command == "ready" || command == "stats") {
        auto payload = command == "health"  ? client->health()
                       : command == "ready" ? client->ready()
                                            : client->stats();
        if (!payload) {
            std::cerr << command << " failed: " << payload.error().message << '\n';
            return 1;
        }
        std::cout.write(reinterpret_cast<const char*>(payload->data()),
                        static_cast<std::streamsize>(payload->size()));
        return 0;
    }
    if (command == "backup") {
        if (dest.empty()) {
            throw std::runtime_error("backup requires --dest PATH");
        }
        auto report = client->backup(dest);
        if (!report) {
            std::cerr << "backup failed: " << report.error().message << '\n';
            return 1;
        }
        std::cout.write(reinterpret_cast<const char*>(report->data()),
                        static_cast<std::streamsize>(report->size()));
        return 0;
    }
    if (command == "expect-not-found") {
        const auto missing = client->get(key);
        if (missing || missing.error().code != glifistore::ErrorCode::not_found ||
            missing.error().category != "not_found" || missing.error().retryability != "new_attempt") {
            std::cerr << "GET did not produce the expected structured not_found error\n";
            return 1;
        }
        return 0;
    }
    if (command == "expect-permission-denied") {
        const auto value = parse_hex(value_hex);
        const auto denied =
            client->put(key, value, glifistore::client::PutOptions{.expire_at_ns = expire_at_ns});
        if (denied.outcome != glifistore::client::MutationOutcome::rejected || !denied.error.has_value() ||
            denied.error->category != "permission_denied" || denied.error->retryability != "never") {
            std::cerr << "PUT did not produce the expected permission_denied rejection\n";
            return 1;
        }
        return 0;
    }
    if (command == "burst-expect-overloaded") {
        const auto value = parse_hex(value_hex);
        for (std::uint64_t index = 0; index < burst; ++index) {
            const auto result =
                client->put(key, value, glifistore::client::PutOptions{.expire_at_ns = expire_at_ns});
            if (result.outcome == glifistore::client::MutationOutcome::rejected && result.error.has_value() &&
                result.error->category == "overloaded" && result.error->retryability == "never") {
                return 0;
            }
            if (!result.committed()) {
                std::cerr << "PUT produced an unexpected result before OVERLOADED\n";
                return 1;
            }
        }
        std::cerr << "PUT burst did not produce the expected overloaded rejection\n";
        return 1;
    }
    if (command == "expect-frame-limit") {
        const std::vector<std::byte> oversized(maximum_frame_bytes, std::byte{0xA5});
        const auto limit_key = parse_hex("6c696d6974");
        const auto rejected = client->put(limit_key, oversized);
        if (rejected.outcome != glifistore::client::MutationOutcome::rejected ||
            !rejected.error.has_value() || rejected.error->category != "invalid_argument" ||
            rejected.error->bytes_sent != 0 || rejected.error->retryability != "never") {
            std::cerr << "oversized PUT did not produce the expected local frame-limit rejection\n";
            return 1;
        }
        return 0;
    }

    usage(argv[0]);
    return 2;
} catch (const std::exception& exception) {

    std::cerr << "error: " << exception.what() << '\n';
    return 1;
}
