#include "glifistore/core/key_hash.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/persistence/filesystem_hooks.hpp"
#include "glifistore/persistence/store_migrate.hpp"
#include "glifistore/persistence/store_verify.hpp"
#include "glifistore/store/store.hpp"
#include "test.hpp"

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

class MigrateTemporaryDirectory final {
  public:
    MigrateTemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "glifistore-migrate-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        GLIFI_REQUIRE(created != nullptr);
        path_ = created;
    }

    ~MigrateTemporaryDirectory() {
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

struct FailAfterDestinationCommits {
    std::size_t commits_seen{};
    std::size_t fail_after{};

    static auto before(void* context, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& state = *static_cast<FailAfterDestinationCommits*>(context);
        if (operation == glifistore::FilesystemOperation::sync_commit_slot &&
            state.commits_seen >= state.fail_after) {
            return glifistore::fail(glifistore::ErrorCode::io_error, "injected migrate interrupt");
        }
        return {};
    }

    static void after(void* context, const glifistore::FilesystemOperation operation) {
        auto& state = *static_cast<FailAfterDestinationCommits*>(context);
        if (operation == glifistore::FilesystemOperation::sync_commit_slot) {
            ++state.commits_seen;
        }
    }

    [[nodiscard]] auto hooks() -> glifistore::FilesystemHooks {
        return {.context = this, .before = &before, .after = &after};
    }
};

[[nodiscard]] auto source_store_id_hex(const std::filesystem::path& source) -> std::string {
    const auto verified = glifistore::verify_durable_store_path(source);
    GLIFI_REQUIRE(verified.has_value());
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(verified->manifest.store_id.size() * 2);
    for (const auto byte : verified->manifest.store_id) {
        const auto value = static_cast<unsigned>(byte);
        out.push_back(kDigits[(value >> 4) & 0xf]);
        out.push_back(kDigits[value & 0xf]);
    }
    return out;
}

} // namespace

GLIFI_TEST("migrate_durable_store reshards live keys from 1 to 4 Workers") {
    MigrateTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto destination = root.path() / "destination";

    {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("alpha", bytes("one")).has_value());
        GLIFI_REQUIRE((*opened)->put("beta", bytes("two")).has_value());
        GLIFI_REQUIRE((*opened)->put("gamma", bytes("three"), 9'000'000'000'000'000'000ULL).has_value());
        GLIFI_REQUIRE((*opened)->close().has_value());
    }

    const auto migrated = glifistore::migrate_durable_store(source, destination, 4);
    GLIFI_REQUIRE(migrated.has_value());
    GLIFI_REQUIRE(migrated->source_worker_count == 1);
    GLIFI_REQUIRE(migrated->target_worker_count == 4);
    GLIFI_REQUIRE(migrated->keys_copied == 3);
    GLIFI_REQUIRE(!migrated->resumed);
    GLIFI_REQUIRE(!std::filesystem::exists(migrated->checkpoint));

    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 4},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = destination,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->worker_count() == 4);
    GLIFI_REQUIRE(value_string(*(*reopened)->get("alpha")) == "one");
    GLIFI_REQUIRE(value_string(*(*reopened)->get("beta")) == "two");
    const auto gamma = (*reopened)->get("gamma");
    GLIFI_REQUIRE(gamma.has_value());
    GLIFI_REQUIRE(value_string(*gamma) == "three");
    GLIFI_REQUIRE(gamma->expire_at_ns == 9'000'000'000'000'000'000ULL);
    GLIFI_REQUIRE(glifistore::route_worker("alpha", 4) ==
                  glifistore::route_worker(glifistore::hash_key("alpha"), 4));
    GLIFI_REQUIRE((*reopened)->close().has_value());

    auto wrong_count = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = destination,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(!wrong_count.has_value());
    GLIFI_REQUIRE(wrong_count.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("migrate_durable_store resumes from a sibling checkpoint") {
    MigrateTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto destination = root.path() / "destination";

    {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 2},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("a", bytes("1")).has_value());
        GLIFI_REQUIRE((*opened)->put("b", bytes("2")).has_value());
        GLIFI_REQUIRE((*opened)->put("c", bytes("3")).has_value());
        GLIFI_REQUIRE((*opened)->close().has_value());
    }

    const auto first = glifistore::migrate_durable_store(source, destination, 3);
    GLIFI_REQUIRE(first.has_value());

    // Simulate an interrupted run: recreate partial destination + checkpoint after key "a".
    std::error_code ignored;
    std::filesystem::remove_all(destination, ignored);
    {
        auto partial = glifistore::Store::open({
            .worker_config = {.explicit_count = 3},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = destination,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(partial.has_value());
        GLIFI_REQUIRE((*partial)->put("a", bytes("1")).has_value());
        GLIFI_REQUIRE((*partial)->close().has_value());
    }
    const auto verified = glifistore::verify_durable_store_path(source);
    GLIFI_REQUIRE(verified.has_value());
    const auto checkpoint = glifistore::migrate_checkpoint_path(destination);
    {
        std::ofstream out{checkpoint};
        GLIFI_REQUIRE(static_cast<bool>(out));
        out << "GlifiStore/migrate-state/1\n"
            << "source_store_id=";
        static constexpr char kDigits[] = "0123456789abcdef";
        for (const auto byte : verified->manifest.store_id) {
            const auto value = static_cast<unsigned>(byte);
            out << kDigits[(value >> 4) & 0xf] << kDigits[value & 0xf];
        }
        out << "\nsource_worker_count=2\ntarget_worker_count=3\nkeys_copied=1\n"
            << "last_key_hex=61\nphase=copying\n";
    }

    const auto resumed = glifistore::migrate_durable_store(source, destination, 3);
    GLIFI_REQUIRE(resumed.has_value());
    GLIFI_REQUIRE(resumed->resumed);
    GLIFI_REQUIRE(resumed->keys_skipped >= 1);

    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 3},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = destination,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(value_string(*(*reopened)->get("a")) == "1");
    GLIFI_REQUIRE(value_string(*(*reopened)->get("b")) == "2");
    GLIFI_REQUIRE(value_string(*(*reopened)->get("c")) == "3");
    GLIFI_REQUIRE(!std::filesystem::exists(checkpoint));
}

GLIFI_TEST("migrate_durable_store refuses a locked source and occupied destination") {
    MigrateTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto destination = root.path() / "destination";
    {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("k", bytes("v")).has_value());

        const auto contested = glifistore::migrate_durable_store(source, destination, 2);
        GLIFI_REQUIRE(!contested.has_value());
        GLIFI_REQUIRE(contested.error().code == glifistore::ErrorCode::io_error);
        GLIFI_REQUIRE((*opened)->close().has_value());
    }
    {
        auto occupied = glifistore::Store::open({
            .worker_config = {.explicit_count = 2},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = destination,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(occupied.has_value());
        GLIFI_REQUIRE((*occupied)->put("x", bytes("y")).has_value());
        GLIFI_REQUIRE((*occupied)->close().has_value());
    }
    const auto refused = glifistore::migrate_durable_store(source, destination, 2);
    GLIFI_REQUIRE(!refused.has_value());
    GLIFI_REQUIRE(refused.error().code == glifistore::ErrorCode::sequence_conflict ||
                  refused.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("migrate_durable_store downscales from 4 to 1 Workers") {
    MigrateTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto destination = root.path() / "destination";
    {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 4},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(opened.has_value());
        for (int index = 0; index < 16; ++index) {
            const auto key = "key-" + std::to_string(index);
            GLIFI_REQUIRE((*opened)->put(key, bytes(key)).has_value());
        }
        GLIFI_REQUIRE((*opened)->close().has_value());
    }
    const auto migrated = glifistore::migrate_durable_store(source, destination, 1);
    GLIFI_REQUIRE(migrated.has_value());
    GLIFI_REQUIRE(migrated->keys_copied == 16);
    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = destination,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->worker_count() == 1);
    for (int index = 0; index < 16; ++index) {
        const auto key = "key-" + std::to_string(index);
        GLIFI_REQUIRE(value_string(*(*reopened)->get(key)) == key);
    }
}

GLIFI_TEST("migrate_durable_store resumes after an injected mid-copy interrupt") {
    MigrateTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto destination = root.path() / "destination";

    {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("a", bytes("1")).has_value());
        GLIFI_REQUIRE((*opened)->put("b", bytes("2")).has_value());
        GLIFI_REQUIRE((*opened)->put("c", bytes("3")).has_value());
        GLIFI_REQUIRE((*opened)->close().has_value());
    }

    // Allow two copied keys and their checkpoints to commit, then interrupt the
    // following copied key.
    FailAfterDestinationCommits interrupt{.fail_after = 2};
    const auto interrupted =
        glifistore::migrate_durable_store(source, destination, 2, true, {}, interrupt.hooks());
    GLIFI_REQUIRE(!interrupted.has_value());
    // The paired Writer entered the Store boundary before the injected storage
    // refusal, so the public outcome is indeterminate/fail-closed. The durable
    // checkpoint is what makes the operation safely resumable.
    GLIFI_REQUIRE(interrupted.error().code == glifistore::ErrorCode::unavailable);
    GLIFI_REQUIRE(std::filesystem::exists(glifistore::migrate_checkpoint_path(destination)));
    // Destination partially populated and checkpoint present.
    GLIFI_REQUIRE(std::filesystem::exists(destination));

    const auto resumed = glifistore::migrate_durable_store(source, destination, 2);
    GLIFI_REQUIRE(resumed.has_value());
    GLIFI_REQUIRE(resumed->resumed);
    GLIFI_REQUIRE(resumed->keys_skipped >= 1);
    GLIFI_REQUIRE(!std::filesystem::exists(resumed->checkpoint));

    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 2},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = destination,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(value_string(*(*reopened)->get("a")) == "1");
    GLIFI_REQUIRE(value_string(*(*reopened)->get("b")) == "2");
    GLIFI_REQUIRE(value_string(*(*reopened)->get("c")) == "3");
}

GLIFI_TEST("migrate_durable_store refuses corrupt mismatched or noncanonical checkpoints") {
    MigrateTemporaryDirectory root;
    const auto source = root.path() / "source";
    const auto destination = root.path() / "destination";
    const auto checkpoint = glifistore::migrate_checkpoint_path(destination);

    {
        auto opened = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = source,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(opened.has_value());
        GLIFI_REQUIRE((*opened)->put("k", bytes("v")).has_value());
        GLIFI_REQUIRE((*opened)->put("m", bytes("source-only")).has_value());
        GLIFI_REQUIRE((*opened)->close().has_value());
    }

    // Checkpoint without destination.
    {
        std::ofstream out{checkpoint};
        GLIFI_REQUIRE(static_cast<bool>(out));
        out << "GlifiStore/migrate-state/1\n"
            << "source_store_id=" << source_store_id_hex(source) << '\n'
            << "source_worker_count=1\ntarget_worker_count=2\nkeys_copied=0\nphase=copying\n";
    }
    const auto missing_dest = glifistore::migrate_durable_store(source, destination, 2, false);
    GLIFI_REQUIRE(!missing_dest.has_value());
    GLIFI_REQUIRE(missing_dest.error().code == glifistore::ErrorCode::invalid_argument);

    std::error_code ignored;
    std::filesystem::remove(checkpoint, ignored);

    // Occupied destination without checkpoint already covered; add corrupt magic with empty dest.
    {
        std::ofstream out{checkpoint};
        GLIFI_REQUIRE(static_cast<bool>(out));
        out << "GlifiStore/migrate-state/999\n"
            << "source_store_id=" << source_store_id_hex(source) << '\n'
            << "source_worker_count=1\ntarget_worker_count=2\nkeys_copied=0\n";
    }
    const auto bad_magic = glifistore::migrate_durable_store(source, destination, 2, false);
    GLIFI_REQUIRE(!bad_magic.has_value());
    GLIFI_REQUIRE(bad_magic.error().code == glifistore::ErrorCode::invalid_argument);
    std::filesystem::remove(checkpoint, ignored);

    // Partial destination + worker-count mismatch in checkpoint.
    {
        auto partial = glifistore::Store::open({
            .worker_config = {.explicit_count = 2},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = destination,
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
        });
        GLIFI_REQUIRE(partial.has_value());
        GLIFI_REQUIRE((*partial)->put("k", bytes("v")).has_value());
        GLIFI_REQUIRE((*partial)->close().has_value());
    }
    {
        std::ofstream out{checkpoint};
        GLIFI_REQUIRE(static_cast<bool>(out));
        out << "GlifiStore/migrate-state/1\n"
            << "source_store_id=" << source_store_id_hex(source) << '\n'
            << "source_worker_count=1\ntarget_worker_count=3\nkeys_copied=1\n"
            << "last_key_hex=6b\nphase=copying\n";
    }
    const auto mismatch = glifistore::migrate_durable_store(source, destination, 2, false);
    GLIFI_REQUIRE(!mismatch.has_value());
    GLIFI_REQUIRE(mismatch.error().code == glifistore::ErrorCode::invalid_argument);

    // Source identity mismatch.
    {
        std::ofstream out{checkpoint};
        GLIFI_REQUIRE(static_cast<bool>(out));
        out << "GlifiStore/migrate-state/1\n"
            << "source_store_id=00000000000000000000000000000000\n"
            << "source_worker_count=1\ntarget_worker_count=2\nkeys_copied=1\n"
            << "last_key_hex=6b\nphase=copying\n";
    }
    const auto wrong_source = glifistore::migrate_durable_store(source, destination, 2, false);
    GLIFI_REQUIRE(!wrong_source.has_value());
    GLIFI_REQUIRE(wrong_source.error().code == glifistore::ErrorCode::invalid_argument);

    const auto valid_source_id = source_store_id_hex(source);
    const auto expect_invalid_checkpoint = [&](const std::string_view body) {
        {
            std::ofstream out{checkpoint, std::ios::trunc | std::ios::binary};
            GLIFI_REQUIRE(static_cast<bool>(out));
            out << "GlifiStore/migrate-state/1\n" << body;
            GLIFI_REQUIRE(static_cast<bool>(out));
        }
        const auto result = glifistore::migrate_durable_store(source, destination, 2, false);
        GLIFI_REQUIRE(!result.has_value());
        GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::invalid_argument);
    };

    expect_invalid_checkpoint("source_store_id=" + valid_source_id + "\nsource_store_id=" + valid_source_id +
                              "\nsource_worker_count=1\ntarget_worker_count=2\nkeys_copied=1\n"
                              "last_key_hex=6b\nphase=copying\n");
    expect_invalid_checkpoint("source_store_id=" + valid_source_id +
                              "\nsource_worker_count=1\ntarget_worker_count=2\n"
                              "last_key_hex=6b\nphase=copying\n");
    expect_invalid_checkpoint("source_store_id=" + valid_source_id +
                              "\nsource_worker_count=1\ntarget_worker_count=2\nkeys_copied=1\n"
                              "last_key_hex=6b\nphase=complete\n");
    expect_invalid_checkpoint("source_store_id=" + valid_source_id +
                              "\nsource_worker_count=1\ntarget_worker_count=2\nkeys_copied=1\n"
                              "phase=copying\n");

    auto oversized = "source_store_id=" + valid_source_id +
                     "\nsource_worker_count=1\ntarget_worker_count=2\nkeys_copied=1\n"
                     "last_key_hex=6b\nphase=copying\n";
    oversized.append(std::size_t{2} * glifistore::kMaxNormalRecordSize + std::size_t{2} * 1024U, '\n');
    expect_invalid_checkpoint(oversized);

    expect_invalid_checkpoint("source_store_id=" + valid_source_id +
                              "\nsource_worker_count=1\ntarget_worker_count=2\nkeys_copied=1\n"
                              "last_key_hex=ff\nphase=copying\n");

    {
        std::ofstream out{checkpoint, std::ios::trunc | std::ios::binary};
        GLIFI_REQUIRE(static_cast<bool>(out));
        out << "GlifiStore/migrate-state/1\n"
            << "source_store_id=" << valid_source_id
            << "\nsource_worker_count=1\ntarget_worker_count=2\nkeys_copied=18446744073709551615\n"
               "last_key_hex=6b\nphase=copying\n";
        GLIFI_REQUIRE(static_cast<bool>(out));
    }
    const auto overflowing_count = glifistore::migrate_durable_store(source, destination, 2, false);
    GLIFI_REQUIRE(!overflowing_count.has_value());
    GLIFI_REQUIRE(overflowing_count.error().code == glifistore::ErrorCode::arithmetic_overflow);

    {
        auto contaminated = glifistore::Store::open({
            .worker_config = {.explicit_count = 2},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = destination,
            .durable_open_mode = glifistore::DurableOpenMode::open_existing,
        });
        GLIFI_REQUIRE(contaminated.has_value());
        GLIFI_REQUIRE((*contaminated)->put("k", bytes("wrong-value")).has_value());
        GLIFI_REQUIRE((*contaminated)->close().has_value());
    }
    expect_invalid_checkpoint("source_store_id=" + valid_source_id +
                              "\nsource_worker_count=1\ntarget_worker_count=2\nkeys_copied=1\n"
                              "last_key_hex=6d\nphase=copying\n");

    {
        auto contaminated = glifistore::Store::open({
            .worker_config = {.explicit_count = 2},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = destination,
            .durable_open_mode = glifistore::DurableOpenMode::open_existing,
        });
        GLIFI_REQUIRE(contaminated.has_value());
        GLIFI_REQUIRE((*contaminated)->put("k", bytes("v")).has_value());
        GLIFI_REQUIRE((*contaminated)->put("extra", bytes("not-from-source")).has_value());
        GLIFI_REQUIRE((*contaminated)->close().has_value());
    }
    expect_invalid_checkpoint("source_store_id=" + valid_source_id +
                              "\nsource_worker_count=1\ntarget_worker_count=2\nkeys_copied=1\n"
                              "last_key_hex=6b\nphase=copying\n");
}
