#include "cli/arguments.hpp"
#include "glifistore/persistence/store_repair.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

namespace {

enum OptionId : std::size_t { help, version, json, no_scan };

constexpr std::array kOptionSpecs{
    glifistore::cli::OptionSpec{
        help, "help", 'h', glifistore::cli::OptionArity::none, {}, "Show this help message and exit"},
    glifistore::cli::OptionSpec{
        version, "version", 'V', glifistore::cli::OptionArity::none, {}, "Show version information and exit"},
    glifistore::cli::OptionSpec{
        json, "json", '\0', glifistore::cli::OptionArity::none, {}, "Emit a stable JSON report on stdout"},
    glifistore::cli::OptionSpec{no_scan,
                                "no-scan",
                                '\0',
                                glifistore::cli::OptionArity::none,
                                {},
                                "Skip committed Record scans during catalog verification"},
};

void print_help(const std::string_view program) {
    glifistore::cli::write_help(
        std::cout, program,
        "Offline repair of a GlifiStore durable data directory into an explicit empty workspace.\n"
        "Never mutates the source. Creates <workspace>/store (clean catalog) and\n"
        "<workspace>/quarantine (non-catalog anomalies + audit.txt).",
        "[OPTIONS] <SOURCE-DATA-DIR> <WORKSPACE-DIR>", kOptionSpecs);
}

[[nodiscard]] auto error_code_name(const glifistore::ErrorCode code) -> std::string_view {
    switch (code) {
    case glifistore::ErrorCode::invalid_argument:
        return "invalid_argument";
    case glifistore::ErrorCode::arithmetic_overflow:
        return "arithmetic_overflow";
    case glifistore::ErrorCode::record_too_large:
        return "record_too_large";
    case glifistore::ErrorCode::segment_full:
        return "segment_full";
    case glifistore::ErrorCode::segment_sealed:
        return "segment_sealed";
    case glifistore::ErrorCode::invalid_record:
        return "invalid_record";
    case glifistore::ErrorCode::checksum_mismatch:
        return "checksum_mismatch";
    case glifistore::ErrorCode::invalid_reference:
        return "invalid_reference";
    case glifistore::ErrorCode::sequence_conflict:
        return "sequence_conflict";
    case glifistore::ErrorCode::corrupted_data:
        return "corrupted_data";
    case glifistore::ErrorCode::not_found:
        return "not_found";
    case glifistore::ErrorCode::resource_exhausted:
        return "resource_exhausted";
    case glifistore::ErrorCode::storage_exhausted:
        return "storage_exhausted";
    case glifistore::ErrorCode::file_too_large:
        return "file_too_large";
    case glifistore::ErrorCode::descriptor_exhausted:
        return "descriptor_exhausted";
    case glifistore::ErrorCode::read_only_filesystem:
        return "read_only_filesystem";
    case glifistore::ErrorCode::internal_error:
        return "internal_error";
    case glifistore::ErrorCode::unavailable:
        return "unavailable";
    case glifistore::ErrorCode::io_error:
        return "io_error";
    }
    return "unknown";
}

[[nodiscard]] auto json_escape(const std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20U) {
                constexpr char kDigits[] = "0123456789abcdef";
                out += "\\u00";
                out += kDigits[(static_cast<unsigned char>(ch) >> 4) & 0xf];
                out += kDigits[static_cast<unsigned char>(ch) & 0xf];
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

void write_text_ok(std::ostream& out, const glifistore::DurableStoreRepairReport& report) {
    out << "status=ok\n"
        << "source=" << report.source.string() << '\n'
        << "workspace=" << report.workspace.string() << '\n'
        << "repaired_store=" << report.repaired_store.string() << '\n'
        << "quarantine=" << report.quarantine_directory.string() << '\n'
        << "catalog_files_copied=" << report.catalog_files_copied << '\n'
        << "catalog_bytes_copied=" << report.catalog_bytes_copied << '\n'
        << "quarantined_files=" << report.quarantined.size() << '\n'
        << "repaired_segments=" << report.repaired_verification.segments.size() << '\n';
}

void write_json_ok(std::ostream& out, const glifistore::DurableStoreRepairReport& report) {
    out << '{' << "\"status\":\"ok\","
        << "\"source\":\"" << json_escape(report.source.string()) << "\","
        << "\"workspace\":\"" << json_escape(report.workspace.string()) << "\","
        << "\"repaired_store\":\"" << json_escape(report.repaired_store.string()) << "\","
        << "\"quarantine\":\"" << json_escape(report.quarantine_directory.string()) << "\","
        << "\"catalog_files_copied\":" << report.catalog_files_copied << ','
        << "\"catalog_bytes_copied\":" << report.catalog_bytes_copied << ','
        << "\"quarantined_files\":" << report.quarantined.size() << ','
        << "\"repaired_segments\":" << report.repaired_verification.segments.size() << "}\n";
}

void write_json_error(std::ostream& out, const std::string_view source, const std::string_view workspace,
                      const glifistore::Error& error) {
    out << '{' << "\"status\":\"error\","
        << "\"source\":\"" << json_escape(source) << "\","
        << "\"workspace\":\"" << json_escape(workspace) << "\","
        << "\"error\":{"
        << "\"code\":\"" << error_code_name(error.code) << "\","
        << "\"message\":\"" << json_escape(error.message) << "\""
        << "}}\n";
}

} // namespace

int main(const int argc, char** argv) try {
    const auto program = glifistore::cli::executable_name(argc > 0 ? argv[0] : "glifistore_repair_store");
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
    if (parsed->positionals.size() != 2) {
        std::cerr << program << ": error: expected source data directory and empty workspace\nTry '"
                  << program << " --help' for more information.\n";
        return 2;
    }

    const auto source = std::string{parsed->positionals[0]};
    const auto workspace = std::string{parsed->positionals[1]};
    const bool emit_json = parsed->has(json);
    const bool scan = !parsed->has(no_scan);
    auto report = glifistore::repair_durable_store(source, workspace, scan);
    if (!report) {
        if (emit_json) {
            write_json_error(std::cout, source, workspace, report.error());
        } else {
            std::cerr << program << ": error: " << error_code_name(report.error().code) << ": "
                      << report.error().message << '\n';
        }
        return 1;
    }
    if (emit_json) {
        write_json_ok(std::cout, *report);
    } else {
        write_text_ok(std::cout, *report);
    }
    return 0;
} catch (const std::exception& exception) {

    const auto program = glifistore::cli::executable_name(argc > 0 ? argv[0] : "glifistore_repair_store");
    std::cerr << program << ": fatal: " << exception.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "glifistore_repair_store: fatal: unknown non-standard exception\n";
    return 1;
}
