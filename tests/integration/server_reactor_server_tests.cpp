#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/persistence/store_backup.hpp"
#include "glifistore/server/daemon_log.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/store/store.hpp"
#include "server/reactor_detail.hpp"
#include "server_reactor_test_support.hpp"
#include "store/store_internal.hpp"
#include "test.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <mutex>
#include <netinet/in.h>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

using namespace glifistore::test::server_reactor_support;

GLIFI_TEST("server rejects unsupported worker counts and undersized protocol buffers") {
    GLIFI_REQUIRE(
        !glifistore::server::Server::create({.port = 0, .worker_count = glifistore::kMaximumWorkerCount + 1U})
             .has_value());
    GLIFI_REQUIRE(!glifistore::server::Server::create(
                       {.port = 0, .maximum_input_bytes = glifistore::server::kRequestHeaderBytes - 1U})
                       .has_value());
    GLIFI_REQUIRE(!glifistore::server::Server::create(
                       {.port = 0, .maximum_output_bytes = glifistore::server::kResponseHeaderBytes - 1U})
                       .has_value());
    GLIFI_REQUIRE(!glifistore::server::Server::create(
                       {.port = 0,
                        .accepted_socket_send_buffer_bytes =
                            static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1U})
                       .has_value());
    GLIFI_REQUIRE(
        !glifistore::server::Server::create({.port = 0, .disk_read_queue_capacity = 0}).has_value());
    GLIFI_REQUIRE(
        !glifistore::server::Server::create({.port = 0, .durable_mutation_queue_capacity = 0}).has_value());
    GLIFI_REQUIRE(
        !glifistore::server::Server::create({.port = 0, .durable_mutation_queue_bytes = 0}).has_value());
    GLIFI_REQUIRE(!glifistore::server::Server::create(
                       {.port = 0, .disk_read_thread_count = glifistore::kMaximumWorkerCount + 1U})
                       .has_value());
    GLIFI_REQUIRE(
        !glifistore::server::Server::create({.port = 0, .worker_count = 2, .disk_read_thread_count = 1})
             .has_value());
    GLIFI_REQUIRE(!glifistore::server::Server::create({.port = 0, .worker_count = 2},
                                                      {.worker_config = {.explicit_count = 1}})
                       .has_value());
}

GLIFI_TEST("server StoreConfig persists acknowledged wire writes across restart") {
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    {
        auto opened = glifistore::server::Server::create(
            {.port = 0, .maximum_connections = 4},
            {.storage_mode = glifistore::StorageMode::durable_sync,
             .data_directory = path,
             .durable_open_mode = glifistore::DurableOpenMode::create_new});
        GLIFI_REQUIRE(opened.has_value());
        auto& server = **opened;
        GLIFI_REQUIRE(server.start().has_value());

        const auto socket = connect_to(server.port());
        GLIFI_REQUIRE(socket >= 0);
        GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
        const auto put = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::put,
            .request_id = 3,
            .key = bytes("durable-wire-key"),
            .value = bytes("durable-wire-value"),
        });
        GLIFI_REQUIRE(put.has_value());
        GLIFI_REQUIRE(send_all(socket, *put));
        const auto put_frame = receive_response(socket);
        const auto put_response = glifistore::server::decode_response(put_frame);
        GLIFI_REQUIRE(put_response.has_value());
        GLIFI_REQUIRE(put_response->frame.status == glifistore::server::ResponseStatus::ok);
        static_cast<void>(::close(socket));
        server.request_stop();
        GLIFI_REQUIRE(server.join().has_value());
    }

    auto reopened =
        glifistore::server::Server::create({.port = 0, .maximum_connections = 4},
                                           {.storage_mode = glifistore::StorageMode::durable_sync,
                                            .data_directory = path,
                                            .durable_open_mode = glifistore::DurableOpenMode::open_existing});
    GLIFI_REQUIRE(reopened.has_value());
    auto& server = **reopened;
    GLIFI_REQUIRE(server.start().has_value());
    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
    const auto get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 4,
        .key = bytes("durable-wire-key"),
    });
    GLIFI_REQUIRE(get.has_value());
    GLIFI_REQUIRE(send_all(socket, *get));
    const auto get_frame = receive_response(socket);
    const auto get_response = glifistore::server::decode_response(get_frame);
    GLIFI_REQUIRE(get_response.has_value());
    GLIFI_REQUIRE(get_response->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(text(get_response->frame.value) == "durable-wire-value");
    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("wire BACKUP before INIT returns NOT_BOUND and creates no destination") {
    // BACKUP is Bound-state only; unbound frames must not run the fenced path.
    ServerTemporaryDirectory temporary;
    auto opened =
        glifistore::server::Server::create({.port = 0, .maximum_connections = 4},
                                           {.storage_mode = glifistore::StorageMode::durable_sync,
                                            .data_directory = temporary.store_path(),
                                            .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    const auto backup_dir = temporary.store_path().parent_path() / "unbound-backup";
    const auto backup_path = backup_dir.string();
    const auto backup = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::backup,
        .request_id = 89,
        .key = bytes(backup_path),
    });
    GLIFI_REQUIRE(backup.has_value());
    GLIFI_REQUIRE(send_all(socket, *backup));
    const auto backup_frame = receive_response(socket);
    const auto backup_response = glifistore::server::decode_response(backup_frame);
    GLIFI_REQUIRE(backup_response.has_value());
    GLIFI_REQUIRE(backup_response->frame.request_id == 89);
    GLIFI_REQUIRE(backup_response->frame.status == glifistore::server::ResponseStatus::not_bound);
    GLIFI_REQUIRE(!std::filesystem::exists(backup_dir));

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("wire BACKUP copies a live durable Server catalog into an empty destination") {
    ServerTemporaryDirectory temporary;
    auto opened =
        glifistore::server::Server::create({.port = 0, .maximum_connections = 4},
                                           {.storage_mode = glifistore::StorageMode::durable_sync,
                                            .data_directory = temporary.store_path(),
                                            .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 90,
        .key = bytes("backup-live-key"),
        .value = bytes("backup-live-value"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(send_all(socket, *put));
    const auto put_frame = receive_response(socket);
    const auto put_response = glifistore::server::decode_response(put_frame);
    GLIFI_REQUIRE(put_response.has_value());
    GLIFI_REQUIRE(put_response->frame.status == glifistore::server::ResponseStatus::ok);

    const auto backup_dir = temporary.store_path().parent_path() / "online-backup";
    const auto backup_path = backup_dir.string();
    const auto backup = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::backup,
        .request_id = 91,
        .key = bytes(backup_path),
    });
    GLIFI_REQUIRE(backup.has_value());
    GLIFI_REQUIRE(send_all(socket, *backup));
    const auto backup_frame = receive_response(socket);
    const auto backup_response = glifistore::server::decode_response(backup_frame);
    GLIFI_REQUIRE(backup_response.has_value());
    GLIFI_REQUIRE(backup_response->frame.status == glifistore::server::ResponseStatus::ok);
    const auto report = text(backup_response->frame.value);
    GLIFI_REQUIRE(report.find("status=ok") != std::string_view::npos);
    GLIFI_REQUIRE(report.find("admission_fence_ns=") != std::string_view::npos);
    GLIFI_REQUIRE(report.find("catalog_copy_ns=") != std::string_view::npos);
    GLIFI_REQUIRE(report.find("destination_verify_ns=") != std::string_view::npos);
    GLIFI_REQUIRE(report.find("segment_copy_workers=") != std::string_view::npos);
    GLIFI_REQUIRE(report.find("source_crc_scanned=") != std::string_view::npos);
    GLIFI_REQUIRE(report.find("destination_crc_scanned=") != std::string_view::npos);

    // Offline tool still fails while the Server holds the lock.
    const auto contested = glifistore::backup_durable_store(
        temporary.store_path(), temporary.store_path().parent_path() / "offline-contested");
    GLIFI_REQUIRE(!contested.has_value());

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());

    const auto restored_dir = temporary.store_path().parent_path() / "restored";
    const auto restored = glifistore::restore_durable_store(backup_dir, restored_dir);
    GLIFI_REQUIRE(restored.has_value());
    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = restored_dir,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened.has_value());
    const auto got = (*reopened)->get("backup-live-key");
    GLIFI_REQUIRE(got.has_value());
    GLIFI_REQUIRE(text(got->bytes) == "backup-live-value");
}

GLIFI_TEST("wire BACKUP refuses before fence when OK report cannot fit output budget") {
    // Oversized OK report used to map to OVERLOADED after a successful fenced copy —
    // false known-not-committed polarity while the destination already held the backup.
    ServerTemporaryDirectory temporary;
    const auto backup_dir = temporary.store_path().parent_path() / "fit-refuse-backup";
    const auto backup_path = backup_dir.string();
    const auto estimated = glifistore::server::reactor_detail::backup_ok_report_max_bytes(backup_path.size());
    GLIFI_REQUIRE(estimated > 64);
    const auto max_output = glifistore::server::kResponseHeaderBytes + 64;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .maximum_output_bytes = max_output},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    const auto backup = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::backup,
        .request_id = 92,
        .key = bytes(backup_path),
    });
    GLIFI_REQUIRE(backup.has_value());
    GLIFI_REQUIRE(send_all(socket, *backup));
    const auto backup_frame = receive_response(socket);
    const auto backup_response = glifistore::server::decode_response(backup_frame);
    GLIFI_REQUIRE(backup_response.has_value());
    GLIFI_REQUIRE(backup_response->frame.request_id == 92);
    GLIFI_REQUIRE(backup_response->frame.status == glifistore::server::ResponseStatus::overloaded);
    GLIFI_REQUIRE(!std::filesystem::exists(backup_dir));

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("wire BACKUP keeps OK after report formatting fails post-commit") {
#if !defined(GLIFISTORE_FAULT_INJECTION)
    return;
#else
    // Site::backup_report throws after backup_to succeeds. Probe must still return
    // success (minimal status=ok) — not INTERNAL_ERROR with destination already filled.
    ServerTemporaryDirectory temporary;
    auto opened =
        glifistore::server::Server::create({.port = 0, .maximum_connections = 4},
                                           {.storage_mode = glifistore::StorageMode::durable_sync,
                                            .data_directory = temporary.store_path(),
                                            .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 93,
        .key = bytes("backup-report-key"),
        .value = bytes("backup-report-value"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(send_all(socket, *put));
    const auto put_frame = receive_response(socket);
    const auto put_response = glifistore::server::decode_response(put_frame);
    GLIFI_REQUIRE(put_response.has_value());
    GLIFI_REQUIRE(put_response->frame.status == glifistore::server::ResponseStatus::ok);

    const auto backup_dir = temporary.store_path().parent_path() / "report-fault-backup";
    const auto backup_path = backup_dir.string();
    glifistore::fault::fail_once(glifistore::fault::Site::backup_report);
    const auto backup = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::backup,
        .request_id = 94,
        .key = bytes(backup_path),
    });
    GLIFI_REQUIRE(backup.has_value());
    GLIFI_REQUIRE(send_all(socket, *backup));
    const auto backup_frame = receive_response(socket);
    glifistore::fault::reset();
    const auto backup_response = glifistore::server::decode_response(backup_frame);
    GLIFI_REQUIRE(backup_response.has_value());
    GLIFI_REQUIRE(backup_response->frame.status == glifistore::server::ResponseStatus::ok);
    const auto report = text(backup_response->frame.value);
    GLIFI_REQUIRE(report.find("status=ok") != std::string_view::npos);
    GLIFI_REQUIRE(std::filesystem::exists(backup_dir));

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());

    const auto restored_dir = temporary.store_path().parent_path() / "report-fault-restored";
    const auto restored = glifistore::restore_durable_store(backup_dir, restored_dir);
    GLIFI_REQUIRE(restored.has_value());
    auto reopened = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = restored_dir,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(reopened.has_value());
    const auto got = (*reopened)->get("backup-report-key");
    GLIFI_REQUIRE(got.has_value());
    GLIFI_REQUIRE(text(got->bytes) == "backup-report-value");
#endif
}

GLIFI_TEST("blocked durable mutation leaves its Reactor responsive with bounded FIFO admission") {
    ServerTemporaryDirectory temporary;
    BlockingFileSync blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .durable_mutation_queue_capacity = 2},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.file_io = {.context = &blocker, .sync_file = &BlockingFileSync::sync_file}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    SyncReleaseGuard release_on_exit{blocker};
    GLIFI_REQUIRE(server.start().has_value());

    const auto first_socket = connect_to(server.port());
    const auto second_socket = connect_to(server.port());
    const auto responsive_socket = connect_to(server.port());
    GLIFI_REQUIRE(first_socket >= 0);
    GLIFI_REQUIRE(second_socket >= 0);
    GLIFI_REQUIRE(responsive_socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(first_socket, 0, 1));
    GLIFI_REQUIRE(initialize_and_bind(second_socket, 0, 1));
    GLIFI_REQUIRE(initialize_and_bind(responsive_socket, 0, 1));

    blocker.arm();
    const auto first = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 70,
        .key = bytes("async-first"),
        .value = bytes("first"),
    });
    const auto ordered_get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 74,
        .key = bytes("async-first"),
    });
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE(ordered_get.has_value());
    std::vector<std::byte> first_pipeline;
    first_pipeline.insert(first_pipeline.end(), first->begin(), first->end());
    first_pipeline.insert(first_pipeline.end(), ordered_get->begin(), ordered_get->end());
    GLIFI_REQUIRE(send_all(first_socket, first_pipeline));
    GLIFI_REQUIRE(blocker.wait_until_blocked());

    // A second mutation must be admitted without waiting for the lane's slow
    // I/O, proving that its queue mutex is not an equivalent storage lock.
    const auto second = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 71,
        .key = bytes("async-second"),
        .value = bytes("second"),
    });
    GLIFI_REQUIRE(second.has_value());
    GLIFI_REQUIRE(send_all(second_socket, *second));

    // send_all only proves kernel admission. Wait until the second mutation has
    // consumed the remaining bounded lane slot before asserting that the next
    // connection is rejected; slow OpenBSD runners can otherwise schedule the
    // responsive socket first.
    bool lane_full = false;
    const auto admission_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < admission_deadline) {
        const auto stats = server.pair_writer_stats();
        if (stats.size() == 1 && stats[0].payload_slots_in_use == 2) {
            lane_full = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    GLIFI_REQUIRE(lane_full);

    // The per-Worker admission budget is now exhausted. Rejection and the
    // following non-storage request are both handled while fsync is suspended.
    const auto rejected = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 72,
        .key = bytes("async-rejected"),
        .value = bytes("rejected"),
    });
    const auto ping = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 73,
        .value = bytes("reactor-live"),
    });
    GLIFI_REQUIRE(rejected.has_value());
    GLIFI_REQUIRE(ping.has_value());
    std::vector<std::byte> pipeline;
    pipeline.insert(pipeline.end(), rejected->begin(), rejected->end());
    pipeline.insert(pipeline.end(), ping->begin(), ping->end());
    GLIFI_REQUIRE(send_all(responsive_socket, pipeline));
    const auto rejected_frame = receive_response(responsive_socket);
    const auto ping_frame = receive_response(responsive_socket);
    const auto rejected_response = glifistore::server::decode_response(rejected_frame);
    const auto ping_response = glifistore::server::decode_response(ping_frame);
    GLIFI_REQUIRE(rejected_response.has_value());
    GLIFI_REQUIRE(rejected_response->frame.request_id == 72);
    GLIFI_REQUIRE(rejected_response->frame.status == glifistore::server::ResponseStatus::overloaded);
    GLIFI_REQUIRE(ping_response.has_value());
    GLIFI_REQUIRE(ping_response->frame.request_id == 73);
    GLIFI_REQUIRE(text(ping_response->frame.value) == "reactor-live");

    blocker.release();
    const auto first_frame = receive_response(first_socket);
    const auto ordered_get_frame = receive_response(first_socket);
    const auto second_frame = receive_response(second_socket);
    const auto first_response = glifistore::server::decode_response(first_frame);
    const auto ordered_get_response = glifistore::server::decode_response(ordered_get_frame);
    const auto second_response = glifistore::server::decode_response(second_frame);
    GLIFI_REQUIRE(first_response.has_value());
    GLIFI_REQUIRE(ordered_get_response.has_value());
    GLIFI_REQUIRE(second_response.has_value());
    GLIFI_REQUIRE(first_response->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(ordered_get_response->frame.request_id == 74);
    GLIFI_REQUIRE(ordered_get_response->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(text(ordered_get_response->frame.value) == "first");
    GLIFI_REQUIRE(second_response->frame.status == glifistore::server::ResponseStatus::ok);
    const auto mutation_stats = server.pair_writer_stats();
    GLIFI_REQUIRE(mutation_stats.size() == 1);
    GLIFI_REQUIRE(mutation_stats[0].queue_depth == 0);
    GLIFI_REQUIRE(mutation_stats[0].queued_bytes == 0);
    GLIFI_REQUIRE(mutation_stats[0].maximum_queue_depth >= 1);
    GLIFI_REQUIRE(mutation_stats[0].maximum_queued_bytes > 0);
    GLIFI_REQUIRE(mutation_stats[0].payload_slot_capacity == 2);
    GLIFI_REQUIRE(mutation_stats[0].payload_slots_in_use == 0);
    GLIFI_REQUIRE(mutation_stats[0].maximum_payload_slots_in_use == 2);
    GLIFI_REQUIRE(mutation_stats[0].payload_arena_capacity_bytes == 16U * 1024U * 1024U);
    GLIFI_REQUIRE(mutation_stats[0].payload_arena_storage_bytes >
                  mutation_stats[0].payload_arena_capacity_bytes);
    GLIFI_REQUIRE(mutation_stats[0].payload_arena_bytes_in_use == 0);
    GLIFI_REQUIRE(mutation_stats[0].maximum_payload_arena_bytes_in_use >= 34);
    GLIFI_REQUIRE(mutation_stats[0].payload_admission_bytes_in_use == 0);
    GLIFI_REQUIRE(mutation_stats[0].maximum_payload_admission_bytes_in_use >= 290);
    GLIFI_REQUIRE(mutation_stats[0].payload_slot_full_total == 1);
    GLIFI_REQUIRE(mutation_stats[0].payload_arena_full_total == 0);
    GLIFI_REQUIRE(mutation_stats[0].payload_too_large_total == 0);
    GLIFI_REQUIRE(mutation_stats[0].admitted == 2);
    GLIFI_REQUIRE(mutation_stats[0].rejected == 1);
    GLIFI_REQUIRE(mutation_stats[0].expired_before_store == 0);
    GLIFI_REQUIRE(mutation_stats[0].completed == 2);
    GLIFI_REQUIRE(mutation_stats[0].conflict_retries == 0);
    GLIFI_REQUIRE(mutation_stats[0].conflict_retry_commits == 0);
    GLIFI_REQUIRE(mutation_stats[0].maximum_service_ns > 0);
    GLIFI_REQUIRE(mutation_stats[0].read_generation_memory.current_allocated_lower_bound_bytes > 0);
    GLIFI_REQUIRE(mutation_stats[0].read_generation_memory.delta_entries == mutation_stats[0].delta_entries);

    static_cast<void>(::close(first_socket));
    static_cast<void>(::close(second_socket));
    static_cast<void>(::close(responsive_socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("mutation completion resumes a bounded pipeline without reordering decided responses") {
    ServerTemporaryDirectory temporary;
    BlockingFileSync blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.file_io = {.context = &blocker, .sync_file = &BlockingFileSync::sync_file}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    SyncReleaseGuard release_on_exit{blocker};
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    const auto put_a = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 501,
        .key = bytes("resume-a"),
        .value = bytes("one"),
    });
    const auto get_a = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 502,
        .key = bytes("resume-a"),
    });
    const auto put_b = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 503,
        .key = bytes("resume-b"),
        .value = bytes("two"),
    });
    const auto get_b = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 504,
        .key = bytes("resume-b"),
    });
    GLIFI_REQUIRE(put_a.has_value());
    GLIFI_REQUIRE(get_a.has_value());
    GLIFI_REQUIRE(put_b.has_value());
    GLIFI_REQUIRE(get_b.has_value());

    std::vector<std::byte> pipeline;
    for (const auto* frame : {&*put_a, &*get_a, &*put_b, &*get_b}) {
        pipeline.insert(pipeline.end(), frame->begin(), frame->end());
    }
    // The second completion encounters this only after ACK/GET responses have
    // been decided. They must drain in order before the connection closes.
    std::array<std::byte, glifistore::server::kRequestHeaderBytes> malformed{};
    malformed[0] = std::byte{static_cast<unsigned char>(glifistore::server::kRequestHeaderBytes)};
    malformed[4] = std::byte{0xff};
    pipeline.insert(pipeline.end(), malformed.begin(), malformed.end());

    blocker.arm();
    GLIFI_REQUIRE(send_all(socket, pipeline));
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    blocker.release();

    for (const auto& [request_id, expected_value] :
         {std::pair{501ULL, std::string_view{}}, std::pair{502ULL, std::string_view{"one"}},
          std::pair{503ULL, std::string_view{}}, std::pair{504ULL, std::string_view{"two"}}}) {
        const auto frame = receive_response(socket);
        const auto response = glifistore::server::decode_response(frame);
        GLIFI_REQUIRE(response.has_value());
        GLIFI_REQUIRE(response->frame.request_id == request_id);
        GLIFI_REQUIRE(response->frame.status == glifistore::server::ResponseStatus::ok);
        if (!expected_value.empty()) {
            GLIFI_REQUIRE(text(response->frame.value) == expected_value);
        }
    }

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("volatile large GET pipeline uses bounded scatter leases in response order") {
    constexpr std::size_t kValueBytes = 64U * 1024U;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 1, .maximum_output_bytes = 256U * 1024U});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
    const std::vector<std::byte> first_value(kValueBytes, std::byte{0x31});
    const std::vector<std::byte> second_value(kValueBytes, std::byte{0x72});
    for (const auto& [request_id, key, value] :
         {std::tuple{601ULL, std::string_view{"scatter-hot-a"}, &first_value},
          std::tuple{602ULL, std::string_view{"scatter-hot-b"}, &second_value}}) {
        const auto put = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::put,
            .request_id = request_id,
            .key = bytes(key),
            .value = *value,
        });
        GLIFI_REQUIRE(put.has_value());
        GLIFI_REQUIRE(send_all(socket, *put));
        const auto frame = receive_response(socket);
        const auto response = glifistore::server::decode_response(frame);
        GLIFI_REQUIRE(response.has_value());
        GLIFI_REQUIRE(response->frame.status == glifistore::server::ResponseStatus::ok);
    }

    const auto first_get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 603,
        .key = bytes("scatter-hot-a"),
    });
    const auto second_get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 604,
        .key = bytes("scatter-hot-b"),
    });
    GLIFI_REQUIRE(first_get.has_value());
    GLIFI_REQUIRE(second_get.has_value());
    std::vector<std::byte> pipeline;
    pipeline.insert(pipeline.end(), first_get->begin(), first_get->end());
    pipeline.insert(pipeline.end(), second_get->begin(), second_get->end());
    GLIFI_REQUIRE(send_all(socket, pipeline));

    for (const auto& [request_id, expected] :
         {std::pair{603ULL, &first_value}, std::pair{604ULL, &second_value}}) {
        const auto frame = receive_response(socket);
        const auto response = glifistore::server::decode_response(frame, 256U * 1024U);
        GLIFI_REQUIRE(response.has_value());
        GLIFI_REQUIRE(response->frame.request_id == request_id);
        GLIFI_REQUIRE(response->frame.status == glifistore::server::ResponseStatus::ok);
        GLIFI_REQUIRE(std::ranges::equal(response->frame.value, *expected));
    }

    // The hot pipelined threshold is deliberately above the established 4 KiB
    // cold-read threshold. Keep this boundary contiguous on every platform row.
    const std::vector<std::byte> boundary_value(4U * 1024U, std::byte{0x55});
    const auto boundary_put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 605,
        .key = bytes("scatter-hot-boundary"),
        .value = boundary_value,
    });
    GLIFI_REQUIRE(boundary_put.has_value());
    GLIFI_REQUIRE(send_all(socket, *boundary_put));
    const auto boundary_put_response = glifistore::server::decode_response(receive_response(socket));
    GLIFI_REQUIRE(boundary_put_response.has_value());
    GLIFI_REQUIRE(boundary_put_response->frame.request_id == 605);
    GLIFI_REQUIRE(boundary_put_response->frame.status == glifistore::server::ResponseStatus::ok);
    const auto boundary_get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 606,
        .key = bytes("scatter-hot-boundary"),
    });
    GLIFI_REQUIRE(boundary_get.has_value());
    GLIFI_REQUIRE(send_all(socket, *boundary_get));
    const auto boundary_frame = receive_response(socket);
    const auto boundary_response = glifistore::server::decode_response(boundary_frame, 256U * 1024U);
    GLIFI_REQUIRE(boundary_response.has_value());
    GLIFI_REQUIRE(boundary_response->frame.request_id == 606);
    GLIFI_REQUIRE(std::ranges::equal(boundary_response->frame.value, boundary_value));

    const auto report = server.stats_report();
    GLIFI_REQUIRE(report.has_value());
    GLIFI_REQUIRE(report->find("output_scatter_responses=2\n") != std::string::npos);
    GLIFI_REQUIRE(report->find("output_scatter_bytes=131072\n") != std::string::npos);
    GLIFI_REQUIRE(report->find("output_scatter_completions=2\n") != std::string::npos);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("pending output stops mutation completion pipeline resume until socket drain") {
#if defined(__OpenBSD__)
    // This backpressure timing test depends on a small SO_RCVBUF retaining the
    // large response in user space. OpenBSD under hosted qemu can drain it while
    // the VM is descheduled; ordered pipeline responses remain covered above.
    return;
#endif
    ServerTemporaryDirectory temporary;
    BlockingFileSync blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 1, .accepted_socket_send_buffer_bytes = 4U * 1024U},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.file_io = {.context = &blocker, .sync_file = &BlockingFileSync::sync_file}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    SyncReleaseGuard release_on_exit{blocker};
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    int receive_buffer_bytes = 4 * 1024;
    GLIFI_REQUIRE(::setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &receive_buffer_bytes,
                               sizeof(receive_buffer_bytes)) == 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    const auto put_a = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 511,
        .key = bytes("slow-a"),
        .value = bytes("one"),
    });
    // Use a protocol-maximum response. A 512 KiB response can be absorbed in
    // loopback autotuning buffers even after SO_RCVBUF/SO_SNDBUF are reduced,
    // which makes the intended EAGAIN boundary timing-dependent on Linux and
    // macOS. The maximum legal frame creates the same bounded condition without
    // test-only production hooks or relaxed assertions.
    constexpr std::size_t kPingBytes =
        glifistore::server::kMaxFrameBytes - glifistore::server::kRequestHeaderBytes;
    const std::vector<std::byte> ping_value(kPingBytes, std::byte{0x61});
    const auto ping = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::ping,
        .request_id = 512,
        .value = ping_value,
    });
    const auto put_b = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 513,
        .key = bytes("slow-b"),
        .value = bytes("two"),
    });
    const auto put_c = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 514,
        .key = bytes("slow-c"),
        .value = bytes("three"),
    });
    GLIFI_REQUIRE(put_a.has_value());
    GLIFI_REQUIRE(ping.has_value());
    GLIFI_REQUIRE(put_b.has_value());
    GLIFI_REQUIRE(put_c.has_value());

    std::vector<std::byte> pipeline;
    for (const auto* frame : {&*put_a, &*ping, &*put_b, &*put_c}) {
        pipeline.insert(pipeline.end(), frame->begin(), frame->end());
    }

    blocker.arm();
    GLIFI_REQUIRE(send_all(socket, pipeline));
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    blocker.release();

    // Completion A resumes the buffered PING and admits B. Its large response
    // cannot drain into the deliberately small TCP windows. Completion B must
    // therefore leave C buffered instead of advancing the Writer lane.
    bool two_completed = false;
    const auto completion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < completion_deadline) {
        const auto stats = server.pair_writer_stats();
        if (stats.size() == 1 && stats[0].completed == 2) {
            GLIFI_REQUIRE(stats[0].admitted == 2);
            two_completed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    GLIFI_REQUIRE(two_completed);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    auto stats = server.pair_writer_stats();
    GLIFI_REQUIRE(stats.size() == 1);
    GLIFI_REQUIRE(stats[0].admitted == 2);

    for (const auto request_id : {511ULL, 512ULL, 513ULL, 514ULL}) {
        const auto frame = receive_response(socket);
        const auto response = glifistore::server::decode_response(frame);
        GLIFI_REQUIRE(response.has_value());
        GLIFI_REQUIRE(response->frame.request_id == request_id);
        GLIFI_REQUIRE(response->frame.status == glifistore::server::ResponseStatus::ok);
        if (request_id == 512) {
            GLIFI_REQUIRE(response->frame.value.size() == kPingBytes);
        }
    }

    stats = server.pair_writer_stats();
    GLIFI_REQUIRE(stats.size() == 1);
    GLIFI_REQUIRE(stats[0].admitted == 3);
    GLIFI_REQUIRE(stats[0].completed == 3);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("mutation payload arena applies byte backpressure independently of queue slots") {
    ServerTemporaryDirectory temporary;
    BlockingFileSync blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0,
         .maximum_connections = 2,
         .durable_mutation_queue_capacity = 4,
         .durable_mutation_queue_bytes = 300},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.file_io = {.context = &blocker, .sync_file = &BlockingFileSync::sync_file}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    SyncReleaseGuard release_on_exit{blocker};
    GLIFI_REQUIRE(server.start().has_value());

    const auto first_socket = connect_to(server.port());
    const auto second_socket = connect_to(server.port());
    GLIFI_REQUIRE(first_socket >= 0);
    GLIFI_REQUIRE(second_socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(first_socket, 0, 1));
    GLIFI_REQUIRE(initialize_and_bind(second_socket, 0, 1));

    const std::string value(64, 'v');
    const auto first = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 77,
        .key = bytes("arena-first"),
        .value = bytes(value),
    });
    const auto second = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 78,
        .key = bytes("arena-second"),
        .value = bytes(value),
    });
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE(second.has_value());
    blocker.arm();
    GLIFI_REQUIRE(send_all(first_socket, *first));
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    GLIFI_REQUIRE(send_all(second_socket, *second));

    const auto rejected_frame = receive_response(second_socket);
    const auto rejected = glifistore::server::decode_response(rejected_frame);
    GLIFI_REQUIRE(rejected.has_value());
    GLIFI_REQUIRE(rejected->frame.request_id == 78);
    GLIFI_REQUIRE(rejected->frame.status == glifistore::server::ResponseStatus::overloaded);
    auto stats = server.pair_writer_stats();
    GLIFI_REQUIRE(stats.size() == 1);
    GLIFI_REQUIRE(stats[0].payload_slot_capacity == 4);
    GLIFI_REQUIRE(stats[0].payload_slots_in_use == 1);
    GLIFI_REQUIRE(stats[0].payload_arena_capacity_bytes == 300);
    GLIFI_REQUIRE(stats[0].payload_arena_bytes_in_use == 75);
    GLIFI_REQUIRE(stats[0].payload_admission_bytes_in_use == 203);
    GLIFI_REQUIRE(stats[0].payload_slot_full_total == 0);
    GLIFI_REQUIRE(stats[0].payload_arena_full_total == 1);

    blocker.release();
    const auto committed_frame = receive_response(first_socket);
    const auto committed = glifistore::server::decode_response(committed_frame);
    GLIFI_REQUIRE(committed.has_value());
    GLIFI_REQUIRE(committed->frame.request_id == 77);
    GLIFI_REQUIRE(committed->frame.status == glifistore::server::ResponseStatus::ok);
    stats = server.pair_writer_stats();
    GLIFI_REQUIRE(stats[0].payload_slots_in_use == 0);
    GLIFI_REQUIRE(stats[0].payload_arena_bytes_in_use == 0);
    GLIFI_REQUIRE(stats[0].payload_admission_bytes_in_use == 0);

    static_cast<void>(::close(first_socket));
    static_cast<void>(::close(second_socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("durable mutation queue deadline rejects only before Store execution") {
#if defined(__OpenBSD__)
    // Hosted OpenBSD qemu VMs do not reliably reach the hooked durable sync barrier for
    // this multi-connection deadline race; Linux/FreeBSD/macOS remain the authority.
    return;
#endif
    ServerTemporaryDirectory temporary;
    BlockingFileSync blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0,
         .maximum_connections = 2,
         .durable_mutation_queue_capacity = 2,
         .durable_mutation_queue_wait_ms = 10},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.file_io = {.context = &blocker, .sync_file = &BlockingFileSync::sync_file}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    SyncReleaseGuard release_on_exit{blocker};
    GLIFI_REQUIRE(server.start().has_value());
    const auto blocked_socket = connect_to(server.port());
    const auto expiring_socket = connect_to(server.port());
    GLIFI_REQUIRE(blocked_socket >= 0);
    GLIFI_REQUIRE(expiring_socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(blocked_socket, 0, 1));
    GLIFI_REQUIRE(initialize_and_bind(expiring_socket, 0, 1));

    blocker.arm();
    const auto blocked = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 75,
        .key = bytes("deadline-blocker"),
        .value = bytes("committed"),
    });
    const auto expiring = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 76,
        .key = bytes("deadline-expired"),
        .value = bytes("must-not-commit"),
    });
    GLIFI_REQUIRE(blocked.has_value());
    GLIFI_REQUIRE(expiring.has_value());
    GLIFI_REQUIRE(send_all(blocked_socket, *blocked));
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    GLIFI_REQUIRE(send_all(expiring_socket, *expiring));
    const auto expiry_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{30};
    while (std::chrono::steady_clock::now() < expiry_deadline) {
        std::this_thread::yield();
    }
    blocker.release();

    const auto blocked_frame = receive_response(blocked_socket);
    const auto expired_frame = receive_response(expiring_socket);
    const auto blocked_response = glifistore::server::decode_response(blocked_frame);
    const auto expired_response = glifistore::server::decode_response(expired_frame);
    GLIFI_REQUIRE(blocked_response.has_value());
    GLIFI_REQUIRE(expired_response.has_value());
    GLIFI_REQUIRE(blocked_response->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(expired_response->frame.status == glifistore::server::ResponseStatus::overloaded);
    const auto stats = server.pair_writer_stats();
    GLIFI_REQUIRE(stats.size() == 1);
    GLIFI_REQUIRE(stats[0].admitted == 2);
    GLIFI_REQUIRE(stats[0].expired_before_store == 1);
    GLIFI_REQUIRE(stats[0].completed == 2);
    GLIFI_REQUIRE(stats[0].maximum_queue_wait_ns >= 10'000'000U);

    static_cast<void>(::close(blocked_socket));
    static_cast<void>(::close(expiring_socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());

    auto recovered = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = temporary.store_path(),
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE((*recovered)->get("deadline-blocker").has_value());
    const auto absent = (*recovered)->get("deadline-expired");
    GLIFI_REQUIRE(!absent.has_value());
    GLIFI_REQUIRE(absent.error().code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE((*recovered)->close().has_value());
}

#if defined(GLIFISTORE_FAULT_INJECTION)
GLIFI_TEST("volatile pair sticky fails READY with pair_fail_closed reason") {
    // Store catalog stays operational on volatile sticky; ready() already fails on
    // pair_writers_->healthy(), but classify_ready_loss must not report none.
    auto opened =
        glifistore::server::Server::create({.port = 0, .maximum_connections = 4, .worker_count = 1});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());
    GLIFI_REQUIRE(server.ready());
    GLIFI_REQUIRE(server.store_operational());
    GLIFI_REQUIRE(server.pair_writers_healthy());
    GLIFI_REQUIRE(glifistore::server::classify_ready_loss(server) ==
                  glifistore::server::ReadyLossReason::none);

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    glifistore::fault::reset();
    glifistore::fault::configure(1, 0, 0);
    glifistore::fault::fail_once(glifistore::fault::Site::publish);
    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 91,
        .key = bytes("volatile-sticky"),
        .value = bytes("x"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(send_all(socket, *put));
    const auto put_frame = receive_response(socket);
    glifistore::fault::reset();
    const auto put_response = glifistore::server::decode_response(put_frame);
    GLIFI_REQUIRE(put_response.has_value());
    GLIFI_REQUIRE(put_response->frame.request_id == 91);
    GLIFI_REQUIRE(put_response->frame.status == glifistore::server::ResponseStatus::ok ||
                  put_response->frame.status == glifistore::server::ResponseStatus::internal_error);
    GLIFI_REQUIRE(put_response->frame.status != glifistore::server::ResponseStatus::overloaded);

    GLIFI_REQUIRE(server.live());
    GLIFI_REQUIRE(server.store_operational());
    GLIFI_REQUIRE(!server.pair_writers_healthy());
    GLIFI_REQUIRE(!server.ready());
    GLIFI_REQUIRE(glifistore::server::classify_ready_loss(server) ==
                  glifistore::server::ReadyLossReason::pair_fail_closed);

    const auto health = probe_lifecycle(socket, glifistore::server::RequestOpcode::health, 92);
    GLIFI_REQUIRE(health.has_value());
    GLIFI_REQUIRE(health->decoded.frame.status == glifistore::server::ResponseStatus::ok);
    const auto ready = probe_lifecycle(socket, glifistore::server::RequestOpcode::ready, 93);
    GLIFI_REQUIRE(ready.has_value());
    GLIFI_REQUIRE(ready->decoded.frame.status == glifistore::server::ResponseStatus::internal_error);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}
#endif
