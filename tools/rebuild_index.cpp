#include "cli/arguments.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <string_view>

namespace {

enum OptionId : std::size_t { help, version };

constexpr std::array kOptionSpecs{
    glifistore::cli::OptionSpec{
        help, "help", 'h', glifistore::cli::OptionArity::none, {}, "Show this help message and exit"},
    glifistore::cli::OptionSpec{
        version, "version", 'V', glifistore::cli::OptionArity::none, {}, "Show version information and exit"},
};

void print_help(const std::string_view program) {
    glifistore::cli::write_help(
        std::cout, program,
        "Offline Index rebuild is not supported for durable persistence v1.\n"
        "Durable Indexes are rebuilt from committed Segments during Store recovery.\n"
        "Operator path: open or restart the Store on the data directory, or use\n"
        "glifistore_repair_store for offline catalog repair into an explicit workspace.",
        "[OPTIONS] <SEGMENT-FILE>...", kOptionSpecs);
}

} // namespace

int main(const int argc, char** argv) try {
    const auto program = glifistore::cli::executable_name(argc > 0 ? argv[0] : "glifistore_rebuild_index");
    auto parsed = glifistore::cli::parse_arguments(argc, argv, kOptionSpecs);
    if (!parsed) {
        std::cerr << program << ": error: " << parsed.error().message << "\nTry '" << program
                  << " --help' for more information.\n";
        return 2;
    }
    if (parsed->has(help)) {
        print_help(program);
        return 0;
    }
    if (parsed->has(version)) {
        std::cout << program << ' ' << GLIFISTORE_VERSION << '\n';
        return 0;
    }
    if (parsed->positionals.empty()) {
        std::cerr << program << ": error: expected one or more segment files\nTry '" << program
                  << " --help' for more information.\n";
        return 2;
    }
    std::cerr << program << ": error: durable v1 does not persist a separate Index artifact\n"
              << program
              << ": note: reopen the Store data directory to rebuild Indexes from "
                 "committed Segments\n"
              << program
              << ": note: for offline catalog repair use glifistore_repair_store with an "
                 "explicit empty workspace\n";
    return 1;
} catch (const std::exception& exception) {

    const auto program = glifistore::cli::executable_name(argc > 0 ? argv[0] : "glifistore_rebuild_index");
    std::cerr << program << ": fatal: " << exception.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "glifistore_rebuild_index: fatal: unknown non-standard exception\n";
    return 1;
}
