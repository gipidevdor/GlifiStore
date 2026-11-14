#include "glifistore/persistence/store_repair.hpp"
#include "glifistore/store/store.hpp"
#include "test.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

class RepairTemporaryDirectory final {
  public:
    RepairTemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-repair-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        path_ = created;
    }

    ~RepairTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

auto bytes(std::string_view value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

auto value_string(const glifistore::OwnedValue& value) -> std::string_view {
    return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

void write_private_file(const std::filesystem::path& path, const std::string_view contents) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    GLIFI_REQUIRE(static_cast<bool>(out));
    out << contents;
    GLIFI_REQUIRE(static_cast<bool>(out));
    GLIFI_REQUIRE(::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0);
}

} // namespace

GLIFI_TEST("repair_durable_store quarantines unlisted Segment and opens a clean store") {
    RepairTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto workspace = root.path() / "workspace";

    {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("keep", bytes("value")).has_value());
        GLIFI_REQUIRE((*opened)->close().has_value());
    }

    write_private_file(source / "segment-00000000000000ff-0000000a.glifi", "orphan-bytes");
    write_private_file(source / "operator-note.txt", "do-not-adopt");

    const auto repaired = glifistore::repair_durable_store(source, workspace);
    GLIFI_REQUIRE(repaired.has_value());
    GLIFI_REQUIRE(repaired->quarantined.size() == 2);
    GLIFI_REQUIRE(std::filesystem::exists(repaired->quarantine_directory / "audit.txt"));
    GLIFI_REQUIRE(
        std::filesystem::exists(repaired->quarantine_directory / "segment-00000000000000ff-0000000a.glifi"));
    GLIFI_REQUIRE(std::filesystem::exists(source / "operator-note.txt"));
    GLIFI_REQUIRE(std::filesystem::exists(source / "segment-00000000000000ff-0000000a.glifi"));

    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = repaired->repaired_store,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(value_string(*(*reopened)->get("keep")) == "value");
    GLIFI_REQUIRE((*reopened)->close().has_value());
}

GLIFI_TEST("repair_durable_store refuses unsafe symlink entries") {
    RepairTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto workspace = root.path() / "workspace";
    {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("keep", bytes("value")).has_value());
        GLIFI_REQUIRE((*opened)->close().has_value());
    }
    std::filesystem::create_symlink(glifistore::kManifestFilename, source / "evil-link");
    const auto repaired = glifistore::repair_durable_store(source, workspace);
    GLIFI_REQUIRE(!repaired.has_value());
    GLIFI_REQUIRE(repaired.error().code == glifistore::ErrorCode::corrupted_data);
    GLIFI_REQUIRE(!std::filesystem::exists(workspace / "store"));
}

GLIFI_TEST("repair_durable_store refuses a non-empty workspace") {
    RepairTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto workspace = root.path() / "workspace";
    std::filesystem::create_directories(workspace);
    write_private_file(workspace / "stale", "x");
    {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->close().has_value());
    }
    const auto repaired = glifistore::repair_durable_store(source, workspace);
    GLIFI_REQUIRE(!repaired.has_value());
    GLIFI_REQUIRE(repaired.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("repair_durable_store refuses a missing catalog Segment") {
    RepairTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto workspace = root.path() / "workspace";
    std::string catalog_segment;
    {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("keep", bytes("value")).has_value());
        GLIFI_REQUIRE((*opened)->close().has_value());
    }
    for (const auto& entry : std::filesystem::directory_iterator{source}) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("segment-") && name.ends_with(".glifi")) {
            catalog_segment = name;
            break;
        }
    }
    GLIFI_REQUIRE(!catalog_segment.empty());
    GLIFI_REQUIRE(std::filesystem::remove(source / catalog_segment));
    const auto repaired = glifistore::repair_durable_store(source, workspace);
    GLIFI_REQUIRE(!repaired.has_value());
    GLIFI_REQUIRE(repaired.error().code == glifistore::ErrorCode::corrupted_data);
    GLIFI_REQUIRE(!std::filesystem::exists(workspace / "store"));
}
