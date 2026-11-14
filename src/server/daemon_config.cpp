#include "glifistore/server/daemon_config.hpp"

#include "cli/arguments.hpp"
#include "daemon_config_detail.hpp"

#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace glifistore::server {

using daemon_config_detail::apply_layer;
using daemon_config_detail::ascii_lower;
using daemon_config_detail::config;
using daemon_config_detail::default_getenv;
using daemon_config_detail::deployment_profile_settings;
using daemon_config_detail::dump_config;
using daemon_config_detail::help;
using daemon_config_detail::ingest_setting;
using daemon_config_detail::kOptionSpecs;
using daemon_config_detail::materialize_from_settings;
using daemon_config_detail::profile;
using daemon_config_detail::SettingMap;
using daemon_config_detail::settings_from_parsed;
using daemon_config_detail::take_profile_name;
using daemon_config_detail::trim;
using daemon_config_detail::version;

auto daemon_option_specs() noexcept -> std::span<const cli::OptionSpec> {
    return kOptionSpecs;
}

auto storage_mode_name(const StorageMode mode) noexcept -> std::string_view {
    switch (mode) {
    case StorageMode::volatile_memory:
        return "volatile";
    case StorageMode::durable_sync:
        return "durable-sync";
    case StorageMode::durable_periodic:
        return "durable-periodic";
    case StorageMode::durable_group:
        return "durable-group";
    }
    return "unknown";
}

auto environment_name_for_option(const std::string_view long_name) -> std::string {
    std::string name = "GLIFISTORE_";
    name.reserve(name.size() + long_name.size());
    for (const char character : long_name) {
        name.push_back(
            character == '-' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    }
    return name;
}

auto load_daemon_config_file(const std::filesystem::path& path)
    -> Result<std::map<std::string, std::string, std::less<>>> {
    std::ifstream input{path};
    if (!input) {
        return fail(ErrorCode::io_error, "cannot open config file: " + path.string());
    }
    SettingMap settings;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        const auto separator = trimmed.find('=');
        if (separator == std::string_view::npos) {
            return fail(ErrorCode::invalid_argument, "config " + path.string() + " line " +
                                                         std::to_string(line_number) +
                                                         ": expected key = value");
        }
        const auto key = trim(trimmed.substr(0, separator));
        auto value_view = trim(trimmed.substr(separator + 1));
        std::string value{value_view};
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        const auto where = "config " + path.string() + " line " + std::to_string(line_number);
        if (auto status = ingest_setting(settings, key, std::move(value), where); !status) {
            return unexpected(status.error());
        }
    }
    if (settings.contains("reuse-port") && settings.at("reuse-port") == "true" &&
        settings.contains("no-reuse-port") && settings.at("no-reuse-port") == "true") {
        return fail(ErrorCode::invalid_argument,
                    "config " + path.string() + ": reuse-port and no-reuse-port are mutually exclusive");
    }
    return settings;
}

auto load_daemon_environment(const DaemonEnvironmentLookup& getenv_fn)
    -> Result<std::map<std::string, std::string, std::less<>>> {
    const DaemonEnvironmentLookup& lookup = getenv_fn ? getenv_fn : DaemonEnvironmentLookup{default_getenv};
    SettingMap settings;
    for (const auto& spec : kOptionSpecs) {
        if (spec.id == help || spec.id == version || spec.id == config || spec.id == profile ||
            spec.id == dump_config) {
            continue;
        }
        const auto env_name = environment_name_for_option(spec.long_name);
        const auto value = lookup(env_name);
        if (!value) {
            continue;
        }
        if (auto status = ingest_setting(settings, spec.long_name, *value, env_name); !status) {
            return unexpected(status.error());
        }
    }
    if (settings.contains("reuse-port") && settings.at("reuse-port") == "true" &&
        settings.contains("no-reuse-port") && settings.at("no-reuse-port") == "true") {
        return fail(ErrorCode::invalid_argument,
                    "GLIFISTORE_REUSE_PORT and GLIFISTORE_NO_REUSE_PORT are mutually exclusive");
    }
    return settings;
}

auto parse_daemon_options(const int argc, char* const argv[], DaemonEnvironmentLookup getenv_fn)
    -> Result<DaemonOptions> {
    auto parsed_cli = cli::parse_arguments(argc, argv, kOptionSpecs);
    if (!parsed_cli) {
        return unexpected(parsed_cli.error());
    }
    if (!parsed_cli->positionals.empty()) {
        return fail(ErrorCode::invalid_argument,
                    "unexpected positional argument: " + std::string{parsed_cli->positionals.front()});
    }

    const bool show_help = parsed_cli->has(help);
    const bool show_version = parsed_cli->has(version);
    if (show_help || show_version) {
        DaemonOptions options;
        options.show_help = show_help;
        options.show_version = show_version;
        return options;
    }

    const DaemonEnvironmentLookup& lookup = getenv_fn ? getenv_fn : DaemonEnvironmentLookup{default_getenv};
    std::optional<std::filesystem::path> config_path;
    if (const auto path = parsed_cli->value(config)) {
        if (path->empty()) {
            return fail(ErrorCode::invalid_argument, "--config must not be empty");
        }
        config_path = std::filesystem::path{*path};
    } else if (const auto from_env = lookup("GLIFISTORE_CONFIG")) {
        if (from_env->empty()) {
            return fail(ErrorCode::invalid_argument, "GLIFISTORE_CONFIG must not be empty");
        }
        config_path = std::filesystem::path{*from_env};
    }

    SettingMap file_settings;
    if (config_path) {
        auto loaded = load_daemon_config_file(*config_path);
        if (!loaded) {
            return unexpected(loaded.error());
        }
        file_settings = std::move(*loaded);
    }
    auto env_settings = load_daemon_environment(lookup);
    if (!env_settings) {
        return unexpected(env_settings.error());
    }
    auto cli_settings = settings_from_parsed(*parsed_cli);
    if (!cli_settings) {
        return unexpected(cli_settings.error());
    }

    std::optional<std::string> resolved_profile;
    if (auto from_file = take_profile_name(file_settings)) {
        resolved_profile = std::move(*from_file);
    }
    if (const auto from_env = lookup("GLIFISTORE_PROFILE")) {
        if (from_env->empty()) {
            return fail(ErrorCode::invalid_argument, "GLIFISTORE_PROFILE must not be empty");
        }
        resolved_profile = *from_env;
    }
    if (auto from_cli = take_profile_name(*cli_settings)) {
        resolved_profile = std::move(*from_cli);
    }

    SettingMap merged;
    if (resolved_profile) {
        auto profile_settings = deployment_profile_settings(*resolved_profile);
        if (!profile_settings) {
            return unexpected(profile_settings.error());
        }
        apply_layer(merged, *profile_settings);
        *resolved_profile = ascii_lower(std::move(*resolved_profile));
    }
    apply_layer(merged, file_settings);
    apply_layer(merged, *env_settings);
    apply_layer(merged, *cli_settings);

    auto options =
        materialize_from_settings(std::move(merged), false, false, resolved_profile.value_or(std::string{}));
    if (!options) {
        return unexpected(options.error());
    }
    options->show_dump_config = parsed_cli->has(dump_config);
    return options;
}

} // namespace glifistore::server
