#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/persistence/store_backup.hpp"
#include "glifistore/server/connection_handoff.hpp"
#include "glifistore/server/daemon_log.hpp"
#include "glifistore/server/disk_read_executor.hpp"
#include "glifistore/server/pair_writer.hpp"
#include "glifistore/server/protocol.hpp"
#include "glifistore/server/reactor.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/server/socket.hpp"
#include "glifistore/store/store.hpp"
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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace glifistore::test::server_reactor_support;

GLIFI_TEST("server request timeout closes socket without cancelling admitted durable mutation") {
#if defined(__OpenBSD__)
    // Same OpenBSD qemu residual as the durable mutation queue deadline test: BlockingFileSync
    // is not reached reliably under the hosted VM; keep the gate on LibreSSL TLS + suite body.
    return;
#endif
    // Beyond client-semantics §6: daemon --request-timeout-ms may reset the TCP
    // connection while Store execution is already underway. Wire v2 has no cancel
    // frame; the admitted durable mutation must still commit (client sees transport
    // / indeterminate and reconciles via read).
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    BlockingFileSync blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0,
         .maximum_connections = 2,
         .durable_mutation_queue_capacity = 1,
         .abuse = {.request_timeout_ms = 50}},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.file_io = {.context = &blocker, .sync_file = &BlockingFileSync::sync_file}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    SyncReleaseGuard release_on_exit{blocker};
    GLIFI_REQUIRE(server.start().has_value());

    const auto mutation_socket = connect_to(server.port());
    GLIFI_REQUIRE(mutation_socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(mutation_socket, 0, 1));
    blocker.arm();
    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 81,
        .key = bytes("timeout-survives"),
        .value = bytes("committed"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(send_all(mutation_socket, *put));
    GLIFI_REQUIRE(blocker.wait_until_blocked());

    bool peer_closed = false;
    const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < close_deadline) {
        char byte{};
        const auto received = ::recv(mutation_socket, &byte, 1, 0);
        if (received == 0) {
            peer_closed = true;
            break;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    GLIFI_REQUIRE(peer_closed);

    const auto probe_socket = connect_to(server.port());
    GLIFI_REQUIRE(probe_socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(probe_socket, 0, 1));
    const auto stats = probe_lifecycle(probe_socket, glifistore::server::RequestOpcode::stats, 82);
    GLIFI_REQUIRE(stats.has_value());
    const auto report = text(stats->decoded.frame.value);
    GLIFI_REQUIRE(report.find("abuse_request_timeout_closed=") != std::string_view::npos);
    const auto marker = report.find("abuse_request_timeout_closed=");
    GLIFI_REQUIRE(marker != std::string_view::npos);
    const auto count_begin = marker + std::string_view{"abuse_request_timeout_closed="}.size();
    GLIFI_REQUIRE(count_begin < report.size());
    GLIFI_REQUIRE(report[count_begin] != '0');

    blocker.release();
    // The Writer completes asynchronously after the timed-out peer is reset.
    // Poll visibility instead of assuming a scheduler-dependent fixed delay.
    bool mutation_visible = false;
    std::uint64_t request_id = 83;
    const auto visibility_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!mutation_visible && std::chrono::steady_clock::now() < visibility_deadline) {
        const auto get = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::get,
            .request_id = request_id++,
            .key = bytes("timeout-survives"),
        });
        GLIFI_REQUIRE(get.has_value());
        GLIFI_REQUIRE(send_all(probe_socket, *get));
        const auto get_frame = receive_response(probe_socket);
        const auto decoded = glifistore::server::decode_response(get_frame);
        GLIFI_REQUIRE(decoded.has_value());
        if (decoded->frame.status == glifistore::server::ResponseStatus::ok) {
            GLIFI_REQUIRE(text(decoded->frame.value) == "committed");
            mutation_visible = true;
            break;
        }
        GLIFI_REQUIRE(decoded->frame.status == glifistore::server::ResponseStatus::not_found);
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    GLIFI_REQUIRE(mutation_visible);

    static_cast<void>(::close(mutation_socket));
    static_cast<void>(::close(probe_socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("server shutdown drains an admitted durable mutation before Store close") {
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    BlockingFileSync blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 1, .durable_mutation_queue_capacity = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.file_io = {.context = &blocker, .sync_file = &BlockingFileSync::sync_file}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    SyncReleaseGuard release_on_exit{blocker};
    GLIFI_REQUIRE(server.start().has_value());
    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
    blocker.arm();
    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 80,
        .key = bytes("drained-mutation"),
        .value = bytes("survives"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(send_all(socket, *put));
    GLIFI_REQUIRE(blocker.wait_until_blocked());

    std::atomic_bool join_finished{};
    bool join_succeeded{};
    server.request_stop();
    std::thread joiner{[&] {
        join_succeeded = server.join().has_value();
        join_finished.store(true, std::memory_order_release);
    }};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{50};
    while (!join_finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    GLIFI_REQUIRE(!join_finished.load(std::memory_order_acquire));
    blocker.release();
    joiner.join();
    GLIFI_REQUIRE(join_succeeded);
    static_cast<void>(::close(socket));

    auto recovered = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = path,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(recovered.has_value());
    const auto value = (*recovered)->get("drained-mutation");
    GLIFI_REQUIRE(value.has_value());
    GLIFI_REQUIRE(text(value->bytes) == "survives");
    GLIFI_REQUIRE((*recovered)->close().has_value());
}

GLIFI_TEST("server shutdown drain deadline abandons queued durable mutations") {
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    BlockingFileSync blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 2, .durable_mutation_queue_capacity = 4, .shutdown_drain_ms = 50},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = path,
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
    timeval timeout{.tv_sec = 2, .tv_usec = 0};
    GLIFI_REQUIRE(::setsockopt(second_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    blocker.arm();
    const auto first = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 200,
        .key = bytes("drain-committed"),
        .value = bytes("kept"),
    });
    const auto second = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 201,
        .key = bytes("drain-abandoned"),
        .value = bytes("dropped"),
    });
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE(second.has_value());
    GLIFI_REQUIRE(send_all(first_socket, *first));
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    GLIFI_REQUIRE(send_all(second_socket, *second));
    const auto queued_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (std::chrono::steady_clock::now() < queued_deadline) {
        const auto stats = server.pair_writer_stats();
        if (!stats.empty() && stats[0].queue_depth >= 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    {
        const auto stats = server.pair_writer_stats();
        GLIFI_REQUIRE(!stats.empty());
        GLIFI_REQUIRE(stats[0].queue_depth >= 1);
    }

    server.request_stop();
    std::optional<glifistore::Status> joined;
    std::thread joiner{[&] { joined = server.join(); }};

    struct JoinGuard final {
        std::thread& thread;
        BlockingFileSync& blocker;
        bool released{};

        void release_and_join() {
            if (!released) {
                blocker.release();
                released = true;
            }
            if (thread.joinable()) {
                thread.join();
            }
        }

        ~JoinGuard() {
            release_and_join();
        }
    } join_guard{joiner, blocker};

    // Drain deadline abandons the queued PUT as wire OVERLOADED while the socket
    // is still live (before hard close). In-flight Store work stays blocked until
    // the sync hook releases.
    const auto abandoned_frame = receive_response(second_socket);
    GLIFI_REQUIRE(!abandoned_frame.empty());
    const auto abandoned_response = glifistore::server::decode_response(abandoned_frame);
    GLIFI_REQUIRE(abandoned_response.has_value());
    GLIFI_REQUIRE(abandoned_response->frame.request_id == 201);
    GLIFI_REQUIRE(abandoned_response->frame.status == glifistore::server::ResponseStatus::overloaded);
    join_guard.release_and_join();
    GLIFI_REQUIRE(joined.has_value());
    GLIFI_REQUIRE(!joined->has_value());
    GLIFI_REQUIRE(joined->error().code == glifistore::ErrorCode::unavailable);
    GLIFI_REQUIRE(joined->error().message.find("shutdown drain deadline") != std::string::npos);
    {
        const auto stats = server.pair_writer_stats();
        GLIFI_REQUIRE(!stats.empty());
        GLIFI_REQUIRE(stats[0].expired_before_store >= 1);
    }
    static_cast<void>(::close(first_socket));
    static_cast<void>(::close(second_socket));

    auto recovered = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_sync,
        .data_directory = path,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
    });
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE((*recovered)->get("drain-committed").has_value());
    const auto abandoned = (*recovered)->get("drain-abandoned");
    GLIFI_REQUIRE(!abandoned.has_value());
    GLIFI_REQUIRE(abandoned.error().code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE((*recovered)->close().has_value());
}

GLIFI_TEST("server shutdown drain deadline abandons durable_group coalescing hold") {
    // Writer has dequeued a PUT and is waiting for min_records — not in the MPSC
    // queue and not in Store. abandon_queued_mutations alone misses it; expire must
    // break coalescing so wire OVERLOADED flushes before hard close.
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 2, .durable_mutation_queue_capacity = 4, .shutdown_drain_ms = 50},
        {.storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_group = {.max_records = 2, .max_bytes = 65'536, .max_wait_ms = 1'000, .min_records = 2}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
    timeval timeout{.tv_sec = 2, .tv_usec = 0};
    GLIFI_REQUIRE(::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 210,
        .key = bytes("coalesce-abandoned"),
        .value = bytes("never"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(send_all(socket, *put));
    const auto held_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    bool held = false;
    while (std::chrono::steady_clock::now() < held_deadline) {
        const auto stats = server.pair_writer_stats();
        if (!stats.empty() && stats[0].queue_depth == 0 && stats[0].payload_slots_in_use >= 1 &&
            stats[0].completed == 0) {
            held = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    GLIFI_REQUIRE(held);

    server.request_stop();
    std::optional<glifistore::Status> joined;
    std::thread joiner{[&] { joined = server.join(); }};

    struct JoinGuard final {
        std::thread& thread;

        ~JoinGuard() {
            if (thread.joinable()) {
                thread.join();
            }
        }
    } join_guard{joiner};

    const auto frame = receive_response(socket);
    GLIFI_REQUIRE(!frame.empty());
    const auto response = glifistore::server::decode_response(frame);
    GLIFI_REQUIRE(response.has_value());
    GLIFI_REQUIRE(response->frame.request_id == 210);
    GLIFI_REQUIRE(response->frame.status == glifistore::server::ResponseStatus::overloaded);
    if (joiner.joinable()) {
        joiner.join();
    }
    GLIFI_REQUIRE(joined.has_value());
    GLIFI_REQUIRE(!joined->has_value());
    GLIFI_REQUIRE(joined->error().code == glifistore::ErrorCode::unavailable);
    {
        const auto stats = server.pair_writer_stats();
        GLIFI_REQUIRE(!stats.empty());
        GLIFI_REQUIRE(stats[0].expired_before_store >= 1);
    }
    static_cast<void>(::close(socket));

    auto recovered = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_group,
        .data_directory = path,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
        .durable_group = {.max_records = 2, .max_bytes = 65'536, .max_wait_ms = 1'000, .min_records = 2},
    });
    GLIFI_REQUIRE(recovered.has_value());
    const auto missing = (*recovered)->get("coalesce-abandoned");
    GLIFI_REQUIRE(!missing.has_value());
    GLIFI_REQUIRE(missing.error().code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE((*recovered)->close().has_value());
}

GLIFI_TEST("durable_group coalescing obeys the oldest mutation queue deadline") {
    // The durable_group fill window is intentionally much longer than the
    // admission SLO. The Writer must close on the oldest queued mutation's
    // deadline instead of sleeping until group-max-wait and only then rejecting.
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    auto opened = glifistore::server::Server::create(
        {.port = 0,
         .maximum_connections = 1,
         .durable_mutation_queue_capacity = 4,
         .durable_mutation_queue_wait_ms = 20},
        {.storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_group = {.max_records = 2, .max_bytes = 65'536, .max_wait_ms = 1'500, .min_records = 2}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 211,
        .key = bytes("queue-deadline-before-group-deadline"),
        .value = bytes("never-enter-store"),
    });
    GLIFI_REQUIRE(put.has_value());
    const auto started = std::chrono::steady_clock::now();
    GLIFI_REQUIRE(send_all(socket, *put));
    const auto frame = receive_response(socket);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    GLIFI_REQUIRE(!frame.empty());
    const auto response = glifistore::server::decode_response(frame);
    GLIFI_REQUIRE(response.has_value());
    GLIFI_REQUIRE(response->frame.request_id == 211);
    GLIFI_REQUIRE(response->frame.status == glifistore::server::ResponseStatus::overloaded);
    // Old behavior waited roughly 1.5 s here. Keep a wide CI margin while
    // proving that the configured queue deadline, not the group deadline, won.
    GLIFI_REQUIRE(elapsed < std::chrono::milliseconds{750});

    const auto stats = server.pair_writer_stats();
    GLIFI_REQUIRE(stats.size() == 1);
    GLIFI_REQUIRE(stats[0].writer_batches == 1);
    GLIFI_REQUIRE(stats[0].expired_before_store == 1);
    GLIFI_REQUIRE(stats[0].writer_batch_queue_deadline_closes == 1);
    GLIFI_REQUIRE(stats[0].writer_batch_durability_deadline_closes == 0);
    GLIFI_REQUIRE(stats[0].maximum_queue_wait_ns >= 20'000'000U);
    GLIFI_REQUIRE(stats[0].maximum_writer_batch_wait_ns > 0U);
    GLIFI_REQUIRE(stats[0].maximum_writer_batch_wait_ns < 750'000'000U);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());

    auto recovered = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .storage_mode = glifistore::StorageMode::durable_group,
        .data_directory = path,
        .durable_open_mode = glifistore::DurableOpenMode::open_existing,
        .durable_group = {.max_records = 2, .max_bytes = 65'536, .max_wait_ms = 1'500, .min_records = 2},
    });
    GLIFI_REQUIRE(recovered.has_value());
    const auto missing = (*recovered)->get("queue-deadline-before-group-deadline");
    GLIFI_REQUIRE(!missing.has_value());
    GLIFI_REQUIRE(missing.error().code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE((*recovered)->close().has_value());
}

GLIFI_TEST("server HEALTH and READY succeed while operational") {
    auto opened = glifistore::server::Server::create({.port = 0, .maximum_connections = 2});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());
    GLIFI_REQUIRE(server.live());
    GLIFI_REQUIRE(server.ready());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    const auto health = probe_lifecycle(socket, glifistore::server::RequestOpcode::health, 401);
    GLIFI_REQUIRE(health.has_value());
    GLIFI_REQUIRE(health->decoded.frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(text(health->decoded.frame.value) == "GlifiStore/live");
    const auto ready = probe_lifecycle(socket, glifistore::server::RequestOpcode::ready, 402);
    GLIFI_REQUIRE(ready.has_value());
    GLIFI_REQUIRE(ready->decoded.frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(text(ready->decoded.frame.value) == "GlifiStore/ready");
    const auto stats = probe_lifecycle(socket, glifistore::server::RequestOpcode::stats, 403);
    GLIFI_REQUIRE(stats.has_value());
    GLIFI_REQUIRE(stats->decoded.frame.status == glifistore::server::ResponseStatus::ok);
    const auto stats_text = text(stats->decoded.frame.value);
    GLIFI_REQUIRE(stats_text.starts_with("GlifiStore/stats\n"));
    GLIFI_REQUIRE(stats_text.find("live=1\n") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("ready=1\n") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("version=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("connections_active=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_state=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("useful_compactions=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_last_compaction_pacing_delay_ns=0\n") !=
                  std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_total_compaction_pacing_delay_ns=0\n") !=
                  std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_last_compaction_pacing_sleep_count=0\n") !=
                  std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_last_compaction_pacing_burst_bytes=0\n") !=
                  std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_skips=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_consecutive_no_gain=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_last_skip_reason=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_last_activation_reason=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_last_no_gain_source_records_verified=") !=
                  std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_total_no_gain_source_bytes_verified=") !=
                  std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_no_gain_scans_suppressed=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_no_gain_retry_after_ns=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("durable_rotation_attempts=0\n") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("durable_rotation_last_publication_wait_ns=0\n") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("durable_rotation_last_seal_ns=0\n") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("durable_rotation_last_create_ns=0\n") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("durable_rotation_last_manifest_publication_ns=0\n") !=
                  std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("durable_rotation_last_final_record_commit_ns=0\n") !=
                  std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("durable_rotation_maximum_total_ns=0\n") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_candidate_dead_byte_ratio_bp=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_foreground_latency_samples=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_last_foreground_p99_ns=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_latency_suspends=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_latency_guard_active=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_latency_deferral_age_ns=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("maintenance_latency_debt_overrides=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("lane[0].total_writer_batch_wait_ns=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("lane[0].maximum_writer_batch_wait_ns=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("lane[0].writer_batch_durability_deadline_closes=") !=
                  std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("lane[0].writer_batch_queue_deadline_closes=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("lane[0].sync_drain_turns=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("lane[0].sync_turn_splits=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("lane[0].sync_async_fairness_turns=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("lane[0].read_generation_base_record_storage_bytes=") !=
                  std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("read_generation_spare_mapping_bytes=") != std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("lane[0].read_generation_base_record_mapped_storage_bytes=") !=
                  std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("lane[0].read_generation_base_lookup_storage_bytes=") !=
                  std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("lane[0].read_generation_delta_lookup_storage_bytes=") !=
                  std::string_view::npos);
    GLIFI_REQUIRE(stats_text.find("lane[0].read_generation_current_allocated_lower_bound_bytes=") !=
                  std::string_view::npos);
    static_cast<void>(::close(socket));

    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("server READY fails during shutdown while live stays true") {
    auto opened = glifistore::server::Server::create({.port = 0, .maximum_connections = 2});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());
    GLIFI_REQUIRE(server.live());
    GLIFI_REQUIRE(server.ready());

    {
        const auto socket = connect_to(server.port());
        GLIFI_REQUIRE(socket >= 0);
        const auto health = probe_lifecycle(socket, glifistore::server::RequestOpcode::health, 411);
        GLIFI_REQUIRE(health.has_value());
        GLIFI_REQUIRE(health->decoded.frame.status == glifistore::server::ResponseStatus::ok);
        GLIFI_REQUIRE(text(health->decoded.frame.value) == "GlifiStore/live");
        const auto ready = probe_lifecycle(socket, glifistore::server::RequestOpcode::ready, 412);
        GLIFI_REQUIRE(ready.has_value());
        GLIFI_REQUIRE(ready->decoded.frame.status == glifistore::server::ResponseStatus::ok);
        GLIFI_REQUIRE(text(ready->decoded.frame.value) == "GlifiStore/ready");
        static_cast<void>(::close(socket));
    }

    server.request_stop();
    // Accept stops and idle peers are closed; readiness is fail-closed on the API immediately.
    GLIFI_REQUIRE(server.live());
    GLIFI_REQUIRE(!server.ready());
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("server READY fails under maintenance emergency") {
    ServerTemporaryDirectory temporary;
    glifistore::DurableResourceLimits limits{};
    limits.max_segment_count = 1;
    limits.max_store_bytes = 4ULL * glifistore::kSegmentSizeBytes;
    limits.max_temporary_compaction_bytes = glifistore::kSegmentSizeBytes;
    {
        auto seeded = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = temporary.store_path(),
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
            .durable_limits = limits,
            .maintenance = {.mode = glifistore::MaintenanceMode::cooperative},
        });
        GLIFI_REQUIRE(seeded.has_value());
        GLIFI_REQUIRE((*seeded)->put("seed", bytes("value")).has_value());
        GLIFI_REQUIRE((*seeded)->close().has_value());
    }
    auto opened =
        glifistore::server::Server::create({.port = 0, .maximum_connections = 2},
                                           {.worker_config = {.explicit_count = 1},
                                            .storage_mode = glifistore::StorageMode::durable_sync,
                                            .data_directory = temporary.store_path(),
                                            .durable_open_mode = glifistore::DurableOpenMode::open_existing,
                                            .durable_limits = limits,
                                            .maintenance = {
                                                .mode = glifistore::MaintenanceMode::background,
                                                .min_eval_interval_ms = 60'000,
                                                .max_eval_interval_ms = 60'000,
                                            }});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    const auto emergency_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < emergency_deadline) {
        const auto ready = probe_lifecycle(socket, glifistore::server::RequestOpcode::ready, 422);
        GLIFI_REQUIRE(ready.has_value());
        if (ready->decoded.frame.status == glifistore::server::ResponseStatus::internal_error) {
            GLIFI_REQUIRE(!server.ready());
            const auto health = probe_lifecycle(socket, glifistore::server::RequestOpcode::health, 421);
            GLIFI_REQUIRE(health.has_value());
            GLIFI_REQUIRE(health->decoded.frame.status == glifistore::server::ResponseStatus::ok);
            static_cast<void>(::close(socket));
            server.request_stop();
            GLIFI_REQUIRE(server.join().has_value());
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
    GLIFI_REQUIRE(false);
}

GLIFI_TEST("server rejects durable PUT and ERASE under maintenance emergency on wire") {
    ServerTemporaryDirectory temporary;
    glifistore::DurableResourceLimits limits{};
    limits.max_segment_count = 1;
    limits.max_store_bytes = 4ULL * glifistore::kSegmentSizeBytes;
    limits.max_temporary_compaction_bytes = glifistore::kSegmentSizeBytes;
    {
        auto seeded = glifistore::Store::open({
            .worker_config = {.explicit_count = 1},
            .storage_mode = glifistore::StorageMode::durable_sync,
            .data_directory = temporary.store_path(),
            .durable_open_mode = glifistore::DurableOpenMode::create_new,
            .durable_limits = limits,
            .maintenance = {.mode = glifistore::MaintenanceMode::cooperative},
        });
        GLIFI_REQUIRE(seeded.has_value());
        GLIFI_REQUIRE((*seeded)->put("seed", bytes("value")).has_value());
        GLIFI_REQUIRE((*seeded)->close().has_value());
    }
    auto opened =
        glifistore::server::Server::create({.port = 0, .maximum_connections = 2},
                                           {.worker_config = {.explicit_count = 1},
                                            .storage_mode = glifistore::StorageMode::durable_sync,
                                            .data_directory = temporary.store_path(),
                                            .durable_open_mode = glifistore::DurableOpenMode::open_existing,
                                            .durable_limits = limits,
                                            .maintenance = {
                                                .mode = glifistore::MaintenanceMode::background,
                                                .min_eval_interval_ms = 60'000,
                                                .max_eval_interval_ms = 60'000,
                                            }});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
    const auto emergency_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < emergency_deadline) {
        const auto ready = probe_lifecycle(socket, glifistore::server::RequestOpcode::ready, 422);
        GLIFI_REQUIRE(ready.has_value());
        if (ready->decoded.frame.status == glifistore::server::ResponseStatus::internal_error) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(!server.ready());

    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 423,
        .key = bytes("blocked-put"),
        .value = bytes("blocked"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(send_all(socket, *put));
    const auto put_frame = receive_response(socket);
    const auto put_response = glifistore::server::decode_response(put_frame);
    GLIFI_REQUIRE(put_response.has_value());
    GLIFI_REQUIRE(put_response->frame.status == glifistore::server::ResponseStatus::overloaded);

    const auto erase = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::erase,
        .request_id = 424,
        .key = bytes("seed"),
    });
    GLIFI_REQUIRE(erase.has_value());
    GLIFI_REQUIRE(send_all(socket, *erase));
    const auto erase_frame = receive_response(socket);
    const auto erase_response = glifistore::server::decode_response(erase_frame);
    GLIFI_REQUIRE(erase_response.has_value());
    GLIFI_REQUIRE(erase_response->frame.status == glifistore::server::ResponseStatus::overloaded);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("durable wire ERASE persists through reopen") {
    ServerTemporaryDirectory temporary;
    auto opened =
        glifistore::server::Server::create({.port = 0, .maximum_connections = 2},
                                           {.worker_config = {.explicit_count = 1},
                                            .storage_mode = glifistore::StorageMode::durable_sync,
                                            .data_directory = temporary.store_path(),
                                            .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());
    const auto port = server.port();

    const auto socket = connect_to(port);
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 10,
        .key = bytes("erase-me"),
        .value = bytes("gone"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(send_all(socket, *put));
    const auto put_frame = receive_response(socket);
    const auto put_response = glifistore::server::decode_response(put_frame);
    GLIFI_REQUIRE(put_response.has_value());
    GLIFI_REQUIRE(put_response->frame.status == glifistore::server::ResponseStatus::ok);
    const auto erase = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::erase,
        .request_id = 11,
        .key = bytes("erase-me"),
    });
    GLIFI_REQUIRE(erase.has_value());
    GLIFI_REQUIRE(send_all(socket, *erase));
    const auto erase_frame = receive_response(socket);
    const auto erase_response = glifistore::server::decode_response(erase_frame);
    GLIFI_REQUIRE(erase_response.has_value());
    GLIFI_REQUIRE(erase_response->frame.status == glifistore::server::ResponseStatus::ok);
    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());

    auto reopened =
        glifistore::server::Server::create({.port = 0, .maximum_connections = 2},
                                           {.worker_config = {.explicit_count = 1},
                                            .storage_mode = glifistore::StorageMode::durable_sync,
                                            .data_directory = temporary.store_path(),
                                            .durable_open_mode = glifistore::DurableOpenMode::open_existing});
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->start().has_value());
    const auto probe_socket = connect_to((*reopened)->port());
    GLIFI_REQUIRE(probe_socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(probe_socket, 0, 1));
    const auto get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 12,
        .key = bytes("erase-me"),
    });
    GLIFI_REQUIRE(get.has_value());
    GLIFI_REQUIRE(send_all(probe_socket, *get));
    const auto get_frame = receive_response(probe_socket);
    const auto get_response = glifistore::server::decode_response(get_frame);
    GLIFI_REQUIRE(get_response.has_value());
    GLIFI_REQUIRE(get_response->frame.status == glifistore::server::ResponseStatus::not_found);
    static_cast<void>(::close(probe_socket));
    (*reopened)->request_stop();
    GLIFI_REQUIRE((*reopened)->join().has_value());
}

GLIFI_TEST("server shutdown stops accepting and closes idle connections") {
    auto opened = glifistore::server::Server::create({.port = 0, .maximum_connections = 4});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());
    const auto port = server.port();

    const auto idle_socket = connect_to(port);
    GLIFI_REQUIRE(idle_socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(idle_socket, 0, 1));

    server.request_stop();
    const auto refuse_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    bool refused = false;
    while (std::chrono::steady_clock::now() < refuse_deadline) {
        const auto probe = connect_to(port);
        if (probe < 0) {
            refused = true;
            break;
        }
        static_cast<void>(::close(probe));
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(refused);

    const auto closed_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    bool peer_closed = false;
    while (std::chrono::steady_clock::now() < closed_deadline) {
        char byte{};
        const auto received = ::recv(idle_socket, &byte, 1, 0);
        if (received == 0) {
            peer_closed = true;
            break;
        }
        if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(peer_closed);
    GLIFI_REQUIRE(server.join().has_value());
    static_cast<void>(::close(idle_socket));
}

GLIFI_TEST("server shutdown drains in-flight durable response before closing connection") {
    ServerTemporaryDirectory temporary;
    BlockingFileSync blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 2, .shutdown_drain_ms = 5'000},
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
    blocker.arm();
    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 310,
        .key = bytes("connection-drain-key"),
        .value = bytes("flushed"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(send_all(socket, *put));
    GLIFI_REQUIRE(blocker.wait_until_blocked());

    server.request_stop();
    std::optional<glifistore::Status> joined;
    std::thread joiner{[&] { joined = server.join(); }};
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    blocker.release();

    const auto frame = receive_response(socket);
    const auto response = glifistore::server::decode_response(frame);
    GLIFI_REQUIRE(response.has_value());
    GLIFI_REQUIRE(response->frame.request_id == 310);
    GLIFI_REQUIRE(response->frame.status == glifistore::server::ResponseStatus::ok);

    char byte{};
    const auto closed_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    bool peer_closed = false;
    while (std::chrono::steady_clock::now() < closed_deadline) {
        const auto received = ::recv(socket, &byte, 1, 0);
        if (received == 0) {
            peer_closed = true;
            break;
        }
        if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(peer_closed);
    joiner.join();
    GLIFI_REQUIRE(joined.has_value());
    GLIFI_REQUIRE(joined->has_value());
    static_cast<void>(::close(socket));
}
