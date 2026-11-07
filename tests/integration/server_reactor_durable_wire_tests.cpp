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

GLIFI_TEST("paired Writer closes strict durable groups without concurrent shard mutators") {
    ServerTemporaryDirectory temporary;
    GroupBatchObserver observer;
    BlockingFileSync blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 2, .durable_mutation_queue_capacity = 4},
        {.storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_group = {.max_records = 2, .max_bytes = 65'536, .max_wait_ms = 1'000, .min_records = 2},
         .filesystem_hooks = {.context = &observer,
                              .before = &GroupBatchObserver::before,
                              .file_io = {.context = &blocker, .sync_file = &BlockingFileSync::sync_file}}});
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
    const auto first = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 90,
        .key = bytes("group-first"),
        .value = bytes("first"),
    });
    const auto second = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 91,
        .key = bytes("group-second"),
        .value = bytes("second"),
    });
    GLIFI_REQUIRE(first.has_value());
    GLIFI_REQUIRE(second.has_value());
    blocker.arm();
    GLIFI_REQUIRE(send_all(first_socket, *first));
    GLIFI_REQUIRE(send_all(second_socket, *second));
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    const auto in_flight_batch_stats = server.durable_batch_stats();
    GLIFI_REQUIRE(in_flight_batch_stats.size() == 1);
    GLIFI_REQUIRE(in_flight_batch_stats[0].pending_records == 2);
    GLIFI_REQUIRE(in_flight_batch_stats[0].flush_attempts == 1);
    GLIFI_REQUIRE(in_flight_batch_stats[0].committed_batches == 0);
    blocker.release();

    const auto first_frame = receive_response(first_socket);
    const auto second_frame = receive_response(second_socket);
    const auto first_response = glifistore::server::decode_response(first_frame);
    const auto second_response = glifistore::server::decode_response(second_frame);
    GLIFI_REQUIRE(first_response.has_value());
    GLIFI_REQUIRE(second_response.has_value());
    GLIFI_REQUIRE(first_response->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(second_response->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(observer.maximum_writes_before_sync() == 2);
    GLIFI_REQUIRE(observer.sync_count() == 1);
    // Regression: a strict paired Writer batch that reaches max_records must
    // still cross the final commit-slot sync before either success response.
    GLIFI_REQUIRE(observer.commit_slot_sync_count() == 1);
    const auto batch_stats = server.durable_batch_stats();
    GLIFI_REQUIRE(batch_stats.size() == 1);
    GLIFI_REQUIRE(batch_stats[0].enabled);
    GLIFI_REQUIRE(batch_stats[0].pending_records == 0);
    GLIFI_REQUIRE(batch_stats[0].pending_bytes == 0);
    GLIFI_REQUIRE(batch_stats[0].current_record_target == 2);
    GLIFI_REQUIRE(batch_stats[0].flush_attempts == 1);
    GLIFI_REQUIRE(batch_stats[0].committed_batches == 1);
    GLIFI_REQUIRE(batch_stats[0].failed_batches == 0);
    GLIFI_REQUIRE(batch_stats[0].committed_records == 2);
    GLIFI_REQUIRE(batch_stats[0].committed_bytes > 0);
    GLIFI_REQUIRE(batch_stats[0].maximum_batch_records == 2);
    GLIFI_REQUIRE(batch_stats[0].maximum_batch_bytes == batch_stats[0].committed_bytes);
    GLIFI_REQUIRE(batch_stats[0].total_commit_duration_ns > 0);
    GLIFI_REQUIRE(batch_stats[0].maximum_commit_duration_ns == batch_stats[0].total_commit_duration_ns);
    GLIFI_REQUIRE(batch_stats[0].deadline_closes == 0);
    const auto writer_stats = server.pair_writer_stats();
    GLIFI_REQUIRE(writer_stats.size() == 1);
    GLIFI_REQUIRE(writer_stats[0].completed == 2);
    GLIFI_REQUIRE(writer_stats[0].writer_batches == 1);
    GLIFI_REQUIRE(writer_stats[0].writer_batch_records == 2);
    GLIFI_REQUIRE(writer_stats[0].completion_notifications == 1);

    static_cast<void>(::close(first_socket));
    static_cast<void>(::close(second_socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("paired Writer preserves same-key order across strict durable group boundaries") {
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 1, .durable_mutation_queue_capacity = 4},
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
    const auto put = glifistore::server::encode_request({.opcode = glifistore::server::RequestOpcode::put,
                                                         .request_id = 92,
                                                         .key = bytes("same-key-group"),
                                                         .value = bytes("value")});
    const auto erase = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::erase,
        .request_id = 93,
        .key = bytes("same-key-group"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(erase.has_value());
    std::vector<std::byte> pipeline;
    pipeline.reserve(put->size() + erase->size());
    pipeline.insert(pipeline.end(), put->begin(), put->end());
    pipeline.insert(pipeline.end(), erase->begin(), erase->end());
    GLIFI_REQUIRE(send_all(socket, pipeline));

    const auto put_response = glifistore::server::decode_response(receive_response(socket));
    const auto erase_response = glifistore::server::decode_response(receive_response(socket));
    GLIFI_REQUIRE(put_response.has_value());
    GLIFI_REQUIRE(erase_response.has_value());
    GLIFI_REQUIRE(put_response->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(erase_response->frame.status == glifistore::server::ResponseStatus::ok);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
    auto recovered =
        glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                 .storage_mode = glifistore::StorageMode::durable_sync,
                                 .data_directory = path,
                                 .durable_open_mode = glifistore::DurableOpenMode::open_existing});
    GLIFI_REQUIRE(recovered.has_value());
    const auto missing = (*recovered)->get("same-key-group");
    GLIFI_REQUIRE(!missing.has_value());
    GLIFI_REQUIRE(missing.error().code == glifistore::ErrorCode::not_found);
}

GLIFI_TEST("blocked durable cold GET leaves its Reactor responsive and applies bounded admission") {
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    {
        auto seed = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                             .storage_mode = glifistore::StorageMode::durable_sync,
                                             .data_directory = path,
                                             .durable_open_mode = glifistore::DurableOpenMode::create_new});
        GLIFI_REQUIRE(seed.has_value());
        GLIFI_REQUIRE((*seed)->put("cold-a", bytes("value-a")).has_value());
        GLIFI_REQUIRE((*seed)->put("cold-b", bytes("value-b")).has_value());
        GLIFI_REQUIRE((*seed)->close().has_value());
    }

    BlockingColdRead blocker;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .disk_read_thread_count = 1, .disk_read_queue_capacity = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = path,
         .durable_open_mode = glifistore::DurableOpenMode::open_existing,
         .filesystem_hooks = {
             .file_io = {.context = &blocker, .read_some_at = &BlockingColdRead::read_some_at}}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto blocked_socket = connect_to(server.port());
    GLIFI_REQUIRE(blocked_socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(blocked_socket, 0, 1));
    blocker.arm();
    const auto cold_a = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 40,
        .key = bytes("cold-a"),
    });
    const auto ordered_put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 43,
        .key = bytes("ordered-after-cold"),
        .value = bytes("ordered-value"),
    });
    GLIFI_REQUIRE(cold_a.has_value());
    GLIFI_REQUIRE(ordered_put.has_value());
    std::vector<std::byte> blocked_pipeline;
    blocked_pipeline.insert(blocked_pipeline.end(), cold_a->begin(), cold_a->end());
    blocked_pipeline.insert(blocked_pipeline.end(), ordered_put->begin(), ordered_put->end());
    GLIFI_REQUIRE(send_all(blocked_socket, blocked_pipeline));
    GLIFI_REQUIRE(blocker.wait_until_blocked());

    // The only disk-read admission is occupied, but the owner-affine Reactor
    // must still accept, initialize, mutate, and respond on another socket.
    const auto responsive_socket = connect_to(server.port());
    GLIFI_REQUIRE(responsive_socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(responsive_socket, 0, 1));
    const auto ordered_not_yet_visible = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 44,
        .key = bytes("ordered-after-cold"),
    });
    GLIFI_REQUIRE(ordered_not_yet_visible.has_value());
    GLIFI_REQUIRE(send_all(responsive_socket, *ordered_not_yet_visible));
    const auto not_yet_visible_frame = receive_response(responsive_socket);
    const auto not_yet_visible = glifistore::server::decode_response(not_yet_visible_frame);
    GLIFI_REQUIRE(not_yet_visible.has_value());
    GLIFI_REQUIRE(not_yet_visible->frame.status == glifistore::server::ResponseStatus::not_found);
    const auto saturated_get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 41,
        .key = bytes("cold-b"),
    });
    const auto same_worker_put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 42,
        .key = bytes("reactor-remains-live"),
        .value = bytes("stored"),
    });
    GLIFI_REQUIRE(saturated_get.has_value());
    GLIFI_REQUIRE(same_worker_put.has_value());
    std::vector<std::byte> pipeline;
    pipeline.insert(pipeline.end(), saturated_get->begin(), saturated_get->end());
    pipeline.insert(pipeline.end(), same_worker_put->begin(), same_worker_put->end());
    GLIFI_REQUIRE(send_all(responsive_socket, pipeline));

    const auto overload_frame = receive_response(responsive_socket);
    const auto put_frame = receive_response(responsive_socket);
    const auto overload = glifistore::server::decode_response(overload_frame);
    const auto put = glifistore::server::decode_response(put_frame);
    GLIFI_REQUIRE(overload.has_value());
    GLIFI_REQUIRE(overload->frame.request_id == 41);
    GLIFI_REQUIRE(overload->frame.status == glifistore::server::ResponseStatus::overloaded);
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(put->frame.request_id == 42);
    GLIFI_REQUIRE(put->frame.status == glifistore::server::ResponseStatus::ok);

    blocker.release();
    const auto cold_frame = receive_response(blocked_socket);
    const auto cold = glifistore::server::decode_response(cold_frame);
    GLIFI_REQUIRE(cold.has_value());
    GLIFI_REQUIRE(cold->frame.request_id == 40);
    GLIFI_REQUIRE(cold->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(text(cold->frame.value) == "value-a");
    const auto ordered_put_frame = receive_response(blocked_socket);
    const auto ordered_put_response = glifistore::server::decode_response(ordered_put_frame);
    GLIFI_REQUIRE(ordered_put_response.has_value());
    GLIFI_REQUIRE(ordered_put_response->frame.request_id == 43);
    GLIFI_REQUIRE(ordered_put_response->frame.status == glifistore::server::ResponseStatus::ok);

    // Both ACKs must follow immutable durable-generation publication. These
    // GETs exercise the captured active-file pins directly; neither key was in
    // the recovery bootstrap generation.
    const auto visible_after_ack = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 45,
        .key = bytes("reactor-remains-live"),
    });
    const auto ordered_after_ack = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 46,
        .key = bytes("ordered-after-cold"),
    });
    GLIFI_REQUIRE(visible_after_ack.has_value());
    GLIFI_REQUIRE(ordered_after_ack.has_value());
    GLIFI_REQUIRE(send_all(responsive_socket, *visible_after_ack));
    const auto visible_frame = receive_response(responsive_socket);
    const auto visible = glifistore::server::decode_response(visible_frame);
    GLIFI_REQUIRE(visible.has_value());
    GLIFI_REQUIRE(visible->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(text(visible->frame.value) == "stored");
    GLIFI_REQUIRE(send_all(blocked_socket, *ordered_after_ack));
    const auto ordered_frame = receive_response(blocked_socket);
    const auto ordered = glifistore::server::decode_response(ordered_frame);
    GLIFI_REQUIRE(ordered.has_value());
    GLIFI_REQUIRE(ordered->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(text(ordered->frame.value) == "ordered-value");

    static_cast<void>(::close(blocked_socket));
    static_cast<void>(::close(responsive_socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("durable cold GET keeps one bounded scatter lease across partial socket writes") {
    // Slow-client short-write path must keep a single bounded scatter lease for the
    // response bytes; this is distinct from L1 QSBR ownership of read generations.
#if defined(__OpenBSD__)
    // Partial-write scatter timing is not stable under OpenBSD qemu (SO_RCVBUF / kqueue
    // scheduling); Linux ASan remains the authority for this lease invariant.
    return;
#endif
    ServerTemporaryDirectory temporary;
    const auto path = temporary.store_path();
    constexpr std::size_t kValueBytes = 768U * 1024U;
    std::vector<std::byte> expected(kValueBytes);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        expected[index] = static_cast<std::byte>(index & 0xFFU);
    }
    {
        auto seed = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                             .storage_mode = glifistore::StorageMode::durable_sync,
                                             .data_directory = path,
                                             .durable_open_mode = glifistore::DurableOpenMode::create_new});
        GLIFI_REQUIRE(seed.has_value());
        GLIFI_REQUIRE((*seed)->put("scatter-large", expected).has_value());
        GLIFI_REQUIRE((*seed)->close().has_value());
    }

    auto opened =
        glifistore::server::Server::create({.port = 0,
                                            .maximum_connections = 1,
                                            .maximum_output_bytes = 1024U * 1024U,
                                            .accepted_socket_send_buffer_bytes = 4U * 1024U,
                                            .disk_read_queue_capacity = 1},
                                           {.storage_mode = glifistore::StorageMode::durable_sync,
                                            .data_directory = path,
                                            .durable_open_mode = glifistore::DurableOpenMode::open_existing});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());
    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
    const int receive_buffer = 4U * 1024U;
    GLIFI_REQUIRE(::setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)) == 0);

    const auto get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 61,
        .key = bytes("scatter-large"),
    });
    GLIFI_REQUIRE(get.has_value());
    GLIFI_REQUIRE(send_all(socket, *get));

    bool observed_partial{};
    const auto partial_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!observed_partial && std::chrono::steady_clock::now() < partial_deadline) {
        const auto report = server.stats_report();
        GLIFI_REQUIRE(report.has_value());
        observed_partial = report->find("output_scatter_partial_writes=0\n") == std::string::npos;
        if (!observed_partial) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    GLIFI_REQUIRE(observed_partial);

    const auto frame = receive_response(socket);
    const auto response = glifistore::server::decode_response(frame, 1024U * 1024U);
    GLIFI_REQUIRE(response.has_value());
    GLIFI_REQUIRE(response->frame.request_id == 61);
    GLIFI_REQUIRE(response->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(std::ranges::equal(response->frame.value, expected));

    const auto report = server.stats_report();
    GLIFI_REQUIRE(report.has_value());
    GLIFI_REQUIRE(report->find("output_scatter_responses=1\n") != std::string::npos);
    GLIFI_REQUIRE(report->find("output_scatter_bytes=786432\n") != std::string::npos);
    GLIFI_REQUIRE(report->find("output_scatter_completions=1\n") != std::string::npos);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("BIND_WORKER handoff saturation returns overloaded then closes") {
    // Prefill Worker 1 handoff ring (capacity-1 → ring of 2), pump only Reactor 0,
    // cross-Reactor BIND must observe OVERLOADED then EOF — not silent success ACK.
    auto store = open_paired_store_for_writer(2, 8);
    GLIFI_REQUIRE(store.has_value());
    glifistore::server::ConnectionHandoffMesh mesh{2, 1};
    auto disk_reads = glifistore::server::DiskReadExecutor::create(**store, 2, 8);
    GLIFI_REQUIRE(disk_reads.has_value());
    auto pair_writers = glifistore::server::PairWriterPool::create(**store, 2, 8, kTestMutationArenaBytes,
                                                                   std::chrono::milliseconds{0});
    GLIFI_REQUIRE(pair_writers.has_value());
    GLIFI_REQUIRE((*pair_writers)->start().has_value());

    glifistore::server::ReactorConfig config{
        .port = 0,
        .maximum_connections = 4,
        .worker_count = 2,
        .connection_handoff_capacity = 1,
        .disk_read_thread_count = 2,
    };
    auto listener = glifistore::server::TcpListener::bind(config.bind_address, 0);
    GLIFI_REQUIRE(listener.has_value());
    auto reactor0 = glifistore::server::Reactor::create(config, 0, std::move(*listener), {}, {}, **store,
                                                        mesh, **disk_reads, **pair_writers);
    GLIFI_REQUIRE(reactor0.has_value());
    auto reactor1 = glifistore::server::Reactor::create(config, 1, {}, {}, {}, **store, mesh, **disk_reads,
                                                        **pair_writers);
    GLIFI_REQUIRE(reactor1.has_value());

    for (int i = 0; i < 2; ++i) {
        int fds[2]{-1, -1};
        GLIFI_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        GLIFI_REQUIRE(glifistore::server::set_nonblocking(fds[0]).has_value());
        glifistore::server::ConnectionHandoff filler{
            .socket = glifistore::server::SocketHandle{fds[0]},
            .bound_worker = 1,
            .initialized = true,
        };
        GLIFI_REQUIRE(mesh.try_handoff(1, std::move(filler)));
        static_cast<void>(::close(fds[1]));
    }

    std::atomic<bool> stop{false};
    std::thread pump{[&] {
        while (!stop.load(std::memory_order_acquire)) {
            static_cast<void>((*reactor0)->run_once(5));
        }
    }};

    const auto client = connect_to((*reactor0)->port());
    GLIFI_REQUIRE(client >= 0);
    const auto init = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::init,
        .request_id = 1,
    });
    GLIFI_REQUIRE(init.has_value());
    GLIFI_REQUIRE(send_all(client, *init));
    const auto init_frame = receive_response(client);
    const auto initialized = glifistore::server::decode_response(init_frame);
    GLIFI_REQUIRE(initialized.has_value());
    GLIFI_REQUIRE(initialized->frame.status == glifistore::server::ResponseStatus::ok);

    const auto bind = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::bind_worker,
        .request_id = 2,
        .target_worker = 1,
    });
    GLIFI_REQUIRE(bind.has_value());
    GLIFI_REQUIRE(send_all(client, *bind));
    const auto bind_frame = receive_response(client);
    const auto bound = glifistore::server::decode_response(bind_frame);
    GLIFI_REQUIRE(bound.has_value());
    GLIFI_REQUIRE(bound->frame.status == glifistore::server::ResponseStatus::overloaded);
    GLIFI_REQUIRE(bound->frame.request_id == 2);

    bool peer_closed = false;
    const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < close_deadline) {
        char byte{};
        const auto received = ::recv(client, &byte, 1, 0);
        if (received == 0) {
            peer_closed = true;
            break;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            continue;
        }
        break;
    }
    GLIFI_REQUIRE(peer_closed);

    stop.store(true, std::memory_order_release);
    pump.join();
    static_cast<void>(::close(client));
    GLIFI_REQUIRE((*pair_writers)->stop_and_drain().has_value());
}

GLIFI_TEST("shutdown force-close rejects pending BIND handoff with OVERLOADED") {
    // Destination idle_for_shutdown used to ignore the mesh: BIND OK sat in an MPSC
    // cell, then teardown destroyed the socket with neither OK nor OVERLOADED.
    auto store = open_paired_store_for_writer(2, 8);
    GLIFI_REQUIRE(store.has_value());
    glifistore::server::ConnectionHandoffMesh mesh{2, 8};
    auto disk_reads = glifistore::server::DiskReadExecutor::create(**store, 2, 8);
    GLIFI_REQUIRE(disk_reads.has_value());
    auto pair_writers = glifistore::server::PairWriterPool::create(**store, 2, 8, kTestMutationArenaBytes,
                                                                   std::chrono::milliseconds{0});
    GLIFI_REQUIRE(pair_writers.has_value());
    GLIFI_REQUIRE((*pair_writers)->start().has_value());

    glifistore::server::ReactorConfig config{
        .port = 0,
        .maximum_connections = 4,
        .worker_count = 2,
        .connection_handoff_capacity = 8,
        .disk_read_thread_count = 2,
    };
    auto reactor1 = glifistore::server::Reactor::create(config, 1, {}, {}, {}, **store, mesh, **disk_reads,
                                                        **pair_writers);
    GLIFI_REQUIRE(reactor1.has_value());

    int fds[2]{-1, -1};
    GLIFI_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    GLIFI_REQUIRE(glifistore::server::set_nonblocking(fds[0]).has_value());
    const auto bind_ok = glifistore::server::encode_response({
        .status = glifistore::server::ResponseStatus::ok,
        .request_id = 2,
        .owner_worker = 1,
        .worker_count = 2,
        .routing_epoch = 1,
    });
    GLIFI_REQUIRE(bind_ok.has_value());
    glifistore::server::ConnectionHandoff pending{
        .socket = glifistore::server::SocketHandle{fds[0]},
        .output = *bind_ok,
        .bound_worker = 1,
        .initialized = true,
    };
    GLIFI_REQUIRE(mesh.try_handoff(1, std::move(pending)));
    GLIFI_REQUIRE(mesh.has_pending(1));
    GLIFI_REQUIRE((*reactor1)->active_connections() == 0);
    GLIFI_REQUIRE(!(*reactor1)->idle_for_shutdown());

    // Force-close path (shutdown drain timeout): must surface OVERLOADED, not bare EOF.
    (*reactor1)->stop_accepting();
    (*reactor1)->close_all_connections();
    GLIFI_REQUIRE(!mesh.has_pending(1));
    GLIFI_REQUIRE((*reactor1)->idle_for_shutdown());

    const auto bind_frame = receive_response(fds[1]);
    const auto bound = glifistore::server::decode_response(bind_frame);
    GLIFI_REQUIRE(bound.has_value());
    GLIFI_REQUIRE(bound->frame.request_id == 2);
    GLIFI_REQUIRE(bound->frame.status == glifistore::server::ResponseStatus::overloaded);

    static_cast<void>(::close(fds[1]));
    GLIFI_REQUIRE((*pair_writers)->stop_and_drain().has_value());
}

GLIFI_TEST("BIND_WORKER destination slot exhaustion returns overloaded then closes") {
    auto store = open_paired_store_for_writer(2, 8);
    GLIFI_REQUIRE(store.has_value());
    glifistore::server::ConnectionHandoffMesh mesh{2, 8};
    auto disk_reads = glifistore::server::DiskReadExecutor::create(**store, 2, 8);
    GLIFI_REQUIRE(disk_reads.has_value());
    auto pair_writers = glifistore::server::PairWriterPool::create(**store, 2, 8, kTestMutationArenaBytes,
                                                                   std::chrono::milliseconds{0});
    GLIFI_REQUIRE(pair_writers.has_value());
    GLIFI_REQUIRE((*pair_writers)->start().has_value());

    glifistore::server::ReactorConfig config0{
        .port = 0,
        .maximum_connections = 4,
        .worker_count = 2,
        .connection_handoff_capacity = 8,
        .disk_read_thread_count = 2,
    };
    glifistore::server::ReactorConfig config1 = config0;
    config1.maximum_connections = 1;

    auto listener = glifistore::server::TcpListener::bind(config0.bind_address, 0);
    GLIFI_REQUIRE(listener.has_value());
    auto reactor0 = glifistore::server::Reactor::create(config0, 0, std::move(*listener), {}, {}, **store,
                                                        mesh, **disk_reads, **pair_writers);
    GLIFI_REQUIRE(reactor0.has_value());
    auto reactor1 = glifistore::server::Reactor::create(config1, 1, {}, {}, {}, **store, mesh, **disk_reads,
                                                        **pair_writers);
    GLIFI_REQUIRE(reactor1.has_value());

    int filler_fds[2]{-1, -1};
    GLIFI_REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, filler_fds) == 0);
    GLIFI_REQUIRE(glifistore::server::set_nonblocking(filler_fds[0]).has_value());
    glifistore::server::ConnectionHandoff filler{
        .socket = glifistore::server::SocketHandle{filler_fds[0]},
        .bound_worker = 1,
        .initialized = true,
    };
    GLIFI_REQUIRE(mesh.try_handoff(1, std::move(filler)));
    const auto adopt_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while ((*reactor1)->active_connections() == 0 && std::chrono::steady_clock::now() < adopt_deadline) {
        static_cast<void>((*reactor1)->run_once(5));
    }
    GLIFI_REQUIRE((*reactor1)->active_connections() == 1);

    std::atomic<bool> stop{false};
    std::thread pump{[&] {
        while (!stop.load(std::memory_order_acquire)) {
            static_cast<void>((*reactor0)->run_once(5));
            static_cast<void>((*reactor1)->run_once(5));
        }
    }};

    const auto client = connect_to((*reactor0)->port());
    GLIFI_REQUIRE(client >= 0);
    const auto init = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::init,
        .request_id = 1,
    });
    GLIFI_REQUIRE(init.has_value());
    GLIFI_REQUIRE(send_all(client, *init));
    const auto init_frame = receive_response(client);
    const auto initialized = glifistore::server::decode_response(init_frame);
    GLIFI_REQUIRE(initialized.has_value());
    GLIFI_REQUIRE(initialized->frame.status == glifistore::server::ResponseStatus::ok);

    const auto bind = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::bind_worker,
        .request_id = 2,
        .target_worker = 1,
    });
    GLIFI_REQUIRE(bind.has_value());
    GLIFI_REQUIRE(send_all(client, *bind));
    const auto bind_frame = receive_response(client);
    const auto bound = glifistore::server::decode_response(bind_frame);
    GLIFI_REQUIRE(bound.has_value());
    GLIFI_REQUIRE(bound->frame.status == glifistore::server::ResponseStatus::overloaded);
    GLIFI_REQUIRE(bound->frame.request_id == 2);

    bool peer_closed = false;
    const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < close_deadline) {
        char byte{};
        const auto received = ::recv(client, &byte, 1, 0);
        if (received == 0) {
            peer_closed = true;
            break;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            continue;
        }
        break;
    }
    GLIFI_REQUIRE(peer_closed);

    stop.store(true, std::memory_order_release);
    pump.join();
    static_cast<void>(::close(client));
    static_cast<void>(::close(filler_fds[1]));
    GLIFI_REQUIRE((*pair_writers)->stop_and_drain().has_value());
}

#if defined(GLIFISTORE_FAULT_INJECTION)
GLIFI_TEST("BIND_WORKER poller remove failure returns overloaded without dual registration") {
    // Unregister must succeed before socket ownership moves. A failed remove used to
    // hand off while the source poller still watched the fd (stale-token spin).
    auto store = open_paired_store_for_writer(2, 8);
    GLIFI_REQUIRE(store.has_value());
    glifistore::server::ConnectionHandoffMesh mesh{2, 8};
    auto disk_reads = glifistore::server::DiskReadExecutor::create(**store, 2, 8);
    GLIFI_REQUIRE(disk_reads.has_value());
    auto pair_writers = glifistore::server::PairWriterPool::create(**store, 2, 8, kTestMutationArenaBytes,
                                                                   std::chrono::milliseconds{0});
    GLIFI_REQUIRE(pair_writers.has_value());
    GLIFI_REQUIRE((*pair_writers)->start().has_value());

    glifistore::server::ReactorConfig config{
        .port = 0,
        .maximum_connections = 4,
        .worker_count = 2,
        .connection_handoff_capacity = 8,
        .disk_read_thread_count = 2,
    };
    auto listener = glifistore::server::TcpListener::bind(config.bind_address, 0);
    GLIFI_REQUIRE(listener.has_value());
    auto reactor0 = glifistore::server::Reactor::create(config, 0, std::move(*listener), {}, {}, **store,
                                                        mesh, **disk_reads, **pair_writers);
    GLIFI_REQUIRE(reactor0.has_value());
    auto reactor1 = glifistore::server::Reactor::create(config, 1, {}, {}, {}, **store, mesh, **disk_reads,
                                                        **pair_writers);
    GLIFI_REQUIRE(reactor1.has_value());

    std::atomic<bool> stop{false};
    std::thread pump{[&] {
        while (!stop.load(std::memory_order_acquire)) {
            static_cast<void>((*reactor0)->run_once(5));
            static_cast<void>((*reactor1)->run_once(5));
        }
    }};

    const auto client = connect_to((*reactor0)->port());
    GLIFI_REQUIRE(client >= 0);
    const auto init = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::init,
        .request_id = 1,
    });
    GLIFI_REQUIRE(init.has_value());
    GLIFI_REQUIRE(send_all(client, *init));
    const auto init_frame = receive_response(client);
    const auto initialized = glifistore::server::decode_response(init_frame);
    GLIFI_REQUIRE(initialized.has_value());
    GLIFI_REQUIRE(initialized->frame.status == glifistore::server::ResponseStatus::ok);

    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::poller_remove);
    const auto bind = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::bind_worker,
        .request_id = 2,
        .target_worker = 1,
    });
    GLIFI_REQUIRE(bind.has_value());
    GLIFI_REQUIRE(send_all(client, *bind));
    const auto bind_frame = receive_response(client);
    glifistore::fault::reset();
    const auto bound = glifistore::server::decode_response(bind_frame);
    GLIFI_REQUIRE(bound.has_value());
    GLIFI_REQUIRE(bound->frame.status == glifistore::server::ResponseStatus::overloaded);
    GLIFI_REQUIRE(bound->frame.request_id == 2);
    GLIFI_REQUIRE((*reactor1)->active_connections() == 0);

    bool peer_closed = false;
    const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < close_deadline) {
        char byte{};
        const auto received = ::recv(client, &byte, 1, 0);
        if (received == 0) {
            peer_closed = true;
            break;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            continue;
        }
        break;
    }
    GLIFI_REQUIRE(peer_closed);

    stop.store(true, std::memory_order_release);
    pump.join();
    static_cast<void>(::close(client));
    GLIFI_REQUIRE((*pair_writers)->stop_and_drain().has_value());
}
#endif

#if defined(GLIFISTORE_FAULT_INJECTION)
GLIFI_TEST("sticky post-commit Writer failure is INTERNAL_ERROR on the wire") {
    // After sticky post-commit failure, HEALTH may still succeed while READY must fail
    // under degraded durability (orchestrators gate traffic on READY, not HEALTH alone).
    ServerTemporaryDirectory temporary;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .worker_count = 1, .disk_read_thread_count = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::publish);
    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 77,
        .key = bytes("sticky-wire"),
        .value = bytes("first"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(send_all(socket, *put));
    const auto put_frame = receive_response(socket);
    glifistore::fault::reset();
    const auto put_response = glifistore::server::decode_response(put_frame);
    GLIFI_REQUIRE(put_response.has_value());
    GLIFI_REQUIRE(put_response->frame.request_id == 77);
    GLIFI_REQUIRE(put_response->frame.status == glifistore::server::ResponseStatus::ok ||
                  put_response->frame.status == glifistore::server::ResponseStatus::internal_error);
    GLIFI_REQUIRE(put_response->frame.status != glifistore::server::ResponseStatus::overloaded);
    GLIFI_REQUIRE(!server.healthy());
    GLIFI_REQUIRE(server.live());
    GLIFI_REQUIRE(!server.ready());
    GLIFI_REQUIRE(!server.store_operational());
    GLIFI_REQUIRE(!server.pair_writers_healthy());
    GLIFI_REQUIRE(glifistore::server::classify_ready_loss(server) ==
                  glifistore::server::ReadyLossReason::store_not_operational);

    const auto health = probe_lifecycle(socket, glifistore::server::RequestOpcode::health, 79);
    GLIFI_REQUIRE(health.has_value());
    GLIFI_REQUIRE(health->decoded.frame.status == glifistore::server::ResponseStatus::ok);
    const auto ready = probe_lifecycle(socket, glifistore::server::RequestOpcode::ready, 80);
    GLIFI_REQUIRE(ready.has_value());
    GLIFI_REQUIRE(ready->decoded.frame.status == glifistore::server::ResponseStatus::internal_error);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("pre-Store sibling after sticky capture is wire OVERLOADED not INTERNAL_ERROR") {
    // Two connections, same key: first commits then Site::capture sticky; second must
    // not enter Store and must not look like reconcile-first INTERNAL_ERROR.
    ServerTemporaryDirectory temporary;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 8, .worker_count = 1, .disk_read_thread_count = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto first = connect_to(server.port());
    const auto second = connect_to(server.port());
    GLIFI_REQUIRE(first >= 0);
    GLIFI_REQUIRE(second >= 0);
    GLIFI_REQUIRE(initialize_and_bind(first, 0, 1));
    GLIFI_REQUIRE(initialize_and_bind(second, 0, 1));

    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::capture);
    const auto put1 = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 101,
        .key = bytes("sibling-fc-key"),
        .value = bytes("first"),
    });
    const auto put2 = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 102,
        .key = bytes("sibling-fc-key"),
        .value = bytes("second"),
    });
    GLIFI_REQUIRE(put1.has_value());
    GLIFI_REQUIRE(put2.has_value());
    GLIFI_REQUIRE(send_all(first, *put1));
    GLIFI_REQUIRE(send_all(second, *put2));

    const auto frame1 = receive_response(first);
    const auto frame2 = receive_response(second);
    glifistore::fault::reset();
    const auto response1 = glifistore::server::decode_response(frame1);
    const auto response2 = glifistore::server::decode_response(frame2);
    GLIFI_REQUIRE(response1.has_value());
    GLIFI_REQUIRE(response2.has_value());
    GLIFI_REQUIRE(response1->frame.request_id == 101);
    GLIFI_REQUIRE(response2->frame.request_id == 102);
    // First: ACK-after-drain success (or sticky INTERNAL_ERROR if drain fails).
    GLIFI_REQUIRE(response1->frame.status == glifistore::server::ResponseStatus::ok ||
                  response1->frame.status == glifistore::server::ResponseStatus::internal_error);
    GLIFI_REQUIRE(response1->frame.status != glifistore::server::ResponseStatus::overloaded);
    // Second: never Store-entered after sticky → OVERLOADED, not INTERNAL_ERROR.
    GLIFI_REQUIRE(response2->frame.status == glifistore::server::ResponseStatus::overloaded);
    GLIFI_REQUIRE(!server.healthy());
    GLIFI_REQUIRE(server.live());

    static_cast<void>(::close(first));
    static_cast<void>(::close(second));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("response queue allocation failure closes connection without daemon fail-stop") {
    // Committed PUT must not escalate an ACK-buffer bad_alloc into executor failed_.
    // Close without inventing OVERLOADED (mutation may already be durable).
    ServerTemporaryDirectory temporary;
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .worker_count = 1, .disk_read_thread_count = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));

    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::response_queue);
    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 81,
        .key = bytes("ack-alloc-key"),
        .value = bytes("ack-alloc-value"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(send_all(socket, *put));
    const auto put_frame = receive_response(socket);
    glifistore::fault::reset();
    GLIFI_REQUIRE(put_frame.empty());
    GLIFI_REQUIRE(server.live());
    GLIFI_REQUIRE(server.healthy());
    GLIFI_REQUIRE(server.ready());
    static_cast<void>(::close(socket));

    const auto probe = connect_to(server.port());
    GLIFI_REQUIRE(probe >= 0);
    GLIFI_REQUIRE(initialize_and_bind(probe, 0, 1));
    const auto health = probe_lifecycle(probe, glifistore::server::RequestOpcode::health, 82);
    GLIFI_REQUIRE(health.has_value());
    GLIFI_REQUIRE(health->decoded.frame.status == glifistore::server::ResponseStatus::ok);
    const auto get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 83,
        .key = bytes("ack-alloc-key"),
    });
    GLIFI_REQUIRE(get.has_value());
    GLIFI_REQUIRE(send_all(probe, *get));
    const auto get_frame = receive_response(probe);
    const auto got = glifistore::server::decode_response(get_frame);
    GLIFI_REQUIRE(got.has_value());
    GLIFI_REQUIRE(got->frame.request_id == 83);
    GLIFI_REQUIRE(got->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(std::ranges::equal(got->frame.value, bytes("ack-alloc-value")));

    static_cast<void>(::close(probe));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("INIT identity allocation failure returns OVERLOADED without daemon fail-stop") {
    // Identity encode bad_alloc must not mark initialized or stop the executor.
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .worker_count = 1, .disk_read_thread_count = 1});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    glifistore::fault::reset();
    glifistore::fault::fail_once(glifistore::fault::Site::init_identity);
    const auto init = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::init,
        .request_id = 91,
    });
    GLIFI_REQUIRE(init.has_value());
    GLIFI_REQUIRE(send_all(socket, *init));
    const auto init_frame = receive_response(socket);
    glifistore::fault::reset();
    const auto initialized = glifistore::server::decode_response(init_frame);
    GLIFI_REQUIRE(initialized.has_value());
    GLIFI_REQUIRE(initialized->frame.request_id == 91);
    GLIFI_REQUIRE(initialized->frame.status == glifistore::server::ResponseStatus::overloaded);
    GLIFI_REQUIRE(server.live());
    GLIFI_REQUIRE(server.healthy());

    // Same connection must not be treated as initialized (BIND without INIT succeeds
    // only after a real INIT OK).
    const auto bind = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::bind_worker,
        .request_id = 92,
        .target_worker = 0,
    });
    GLIFI_REQUIRE(bind.has_value());
    GLIFI_REQUIRE(send_all(socket, *bind));
    const auto bind_frame = receive_response(socket);
    const auto bound = glifistore::server::decode_response(bind_frame);
    GLIFI_REQUIRE(bound.has_value());
    GLIFI_REQUIRE(bound->frame.status == glifistore::server::ResponseStatus::invalid_request);

    const auto probe = connect_to(server.port());
    GLIFI_REQUIRE(probe >= 0);
    GLIFI_REQUIRE(initialize_and_bind(probe, 0, 1));

    static_cast<void>(::close(socket));
    static_cast<void>(::close(probe));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}
#endif

GLIFI_TEST("BIND_WORKER OK is flushed before pipelined decode failure closes") {
    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .worker_count = 1, .disk_read_thread_count = 1});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    const auto init = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::init,
        .request_id = 1,
    });
    GLIFI_REQUIRE(init.has_value());
    GLIFI_REQUIRE(send_all(socket, *init));
    const auto init_frame = receive_response(socket);
    const auto initialized = glifistore::server::decode_response(init_frame);
    GLIFI_REQUIRE(initialized.has_value());
    GLIFI_REQUIRE(initialized->frame.status == glifistore::server::ResponseStatus::ok);

    const auto bind = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::bind_worker,
        .request_id = 2,
        .target_worker = 0,
    });
    GLIFI_REQUIRE(bind.has_value());
    std::vector<std::byte> pipeline(bind->begin(), bind->end());
    std::array<std::byte, glifistore::server::kRequestHeaderBytes> bad{};
    bad[0] = std::byte{static_cast<unsigned char>(glifistore::server::kRequestHeaderBytes)};
    bad[4] = std::byte{0xff};
    pipeline.insert(pipeline.end(), bad.begin(), bad.end());
    GLIFI_REQUIRE(send_all(socket, pipeline));

    const auto bind_frame = receive_response(socket);
    const auto bound = glifistore::server::decode_response(bind_frame);
    GLIFI_REQUIRE(bound.has_value());
    GLIFI_REQUIRE(bound->frame.status == glifistore::server::ResponseStatus::ok);
    GLIFI_REQUIRE(bound->frame.request_id == 2);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("durable segment_full surfaces as wire OVERLOADED not INTERNAL_ERROR") {
    // Append rejected before commit must not look like sticky INTERNAL_ERROR / reconcile.
    ServerTemporaryDirectory temporary;

    struct SegmentFullOnPut final {
        std::size_t write_records{};

        static auto before(void* opaque, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto& state = *static_cast<SegmentFullOnPut*>(opaque);
            if (operation != glifistore::FilesystemOperation::write_record) {
                return {};
            }
            ++state.write_records;
            return glifistore::fail(glifistore::ErrorCode::segment_full, "injected segment full");
        }
    } failure;

    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .worker_count = 1, .disk_read_thread_count = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.context = &failure, .before = &SegmentFullOnPut::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 90,
        .key = bytes("segment-full-key"),
        .value = bytes("value"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(send_all(socket, *put));
    const auto frame = receive_response(socket);
    const auto response = glifistore::server::decode_response(frame);
    GLIFI_REQUIRE(response.has_value());
    GLIFI_REQUIRE(response->frame.request_id == 90);
    GLIFI_REQUIRE(response->frame.status == glifistore::server::ResponseStatus::overloaded);
    GLIFI_REQUIRE(failure.write_records >= 1);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("durable not-committed io_error surfaces as wire OVERLOADED not INTERNAL_ERROR") {
    // Pre-write append reject is authoritative not_committed; Writer must not leave
    // ErrorCode::io_error (Reactor → INTERNAL_ERROR → client indeterminate/reconcile).
    ServerTemporaryDirectory temporary;

    struct IoErrorOnPut final {
        std::size_t write_records{};

        static auto before(void* opaque, const glifistore::FilesystemOperation operation)
            -> glifistore::Status {
            auto& state = *static_cast<IoErrorOnPut*>(opaque);
            if (operation != glifistore::FilesystemOperation::write_record) {
                return {};
            }
            ++state.write_records;
            return glifistore::fail(glifistore::ErrorCode::io_error, "injected append reject");
        }
    } failure;

    auto opened = glifistore::server::Server::create(
        {.port = 0, .maximum_connections = 4, .worker_count = 1, .disk_read_thread_count = 1},
        {.storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.store_path(),
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .filesystem_hooks = {.context = &failure, .before = &IoErrorOnPut::before}});
    GLIFI_REQUIRE(opened.has_value());
    auto& server = **opened;
    GLIFI_REQUIRE(server.start().has_value());

    const auto socket = connect_to(server.port());
    GLIFI_REQUIRE(socket >= 0);
    GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
    const auto put = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::put,
        .request_id = 91,
        .key = bytes("io-error-not-committed-key"),
        .value = bytes("value"),
    });
    GLIFI_REQUIRE(put.has_value());
    GLIFI_REQUIRE(send_all(socket, *put));
    const auto frame = receive_response(socket);
    const auto response = glifistore::server::decode_response(frame);
    GLIFI_REQUIRE(response.has_value());
    GLIFI_REQUIRE(response->frame.request_id == 91);
    GLIFI_REQUIRE(response->frame.status == glifistore::server::ResponseStatus::overloaded);
    GLIFI_REQUIRE(failure.write_records >= 1);

    const auto get = glifistore::server::encode_request({
        .opcode = glifistore::server::RequestOpcode::get,
        .request_id = 92,
        .key = bytes("io-error-not-committed-key"),
    });
    GLIFI_REQUIRE(get.has_value());
    GLIFI_REQUIRE(send_all(socket, *get));
    const auto get_frame = receive_response(socket);
    const auto get_response = glifistore::server::decode_response(get_frame);
    GLIFI_REQUIRE(get_response.has_value());
    GLIFI_REQUIRE(get_response->frame.request_id == 92);
    GLIFI_REQUIRE(get_response->frame.status == glifistore::server::ResponseStatus::not_found);

    static_cast<void>(::close(socket));
    server.request_stop();
    GLIFI_REQUIRE(server.join().has_value());
}

GLIFI_TEST("durable not-committed INTERNAL_ERROR-bucket codes surface as wire OVERLOADED") {
    // corrupted_data / internal_error on authoritative not_committed must not stay in the
    // Reactor INTERNAL_ERROR bucket (client indeterminate). Mirrors rotation/open and
    // pre-boundary catch carriers that already stamp not_committed with those codes.
    const std::array codes{glifistore::ErrorCode::corrupted_data, glifistore::ErrorCode::internal_error};
    for (std::size_t index = 0; index < codes.size(); ++index) {
        ServerTemporaryDirectory temporary;

        struct InjectedReject final {
            glifistore::ErrorCode code{};
            std::size_t write_records{};

            static auto before(void* opaque, const glifistore::FilesystemOperation operation)
                -> glifistore::Status {
                auto& state = *static_cast<InjectedReject*>(opaque);
                if (operation != glifistore::FilesystemOperation::write_record) {
                    return {};
                }
                ++state.write_records;
                return glifistore::fail(state.code, "injected not_committed reject");
            }
        } failure{.code = codes[index]};

        auto opened = glifistore::server::Server::create(
            {.port = 0, .maximum_connections = 4, .worker_count = 1, .disk_read_thread_count = 1},
            {.storage_mode = glifistore::StorageMode::durable_sync,
             .data_directory = temporary.store_path(),
             .durable_open_mode = glifistore::DurableOpenMode::create_new,
             .filesystem_hooks = {.context = &failure, .before = &InjectedReject::before}});
        GLIFI_REQUIRE(opened.has_value());
        auto& server = **opened;
        GLIFI_REQUIRE(server.start().has_value());

        const auto socket = connect_to(server.port());
        GLIFI_REQUIRE(socket >= 0);
        GLIFI_REQUIRE(initialize_and_bind(socket, 0, 1));
        const auto key_text = std::string{"bucket-not-committed-"} + std::to_string(index);
        const auto key = bytes(key_text);
        const auto put = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::put,
            .request_id = 200U + static_cast<std::uint64_t>(index),
            .key = key,
            .value = bytes("value"),
        });
        GLIFI_REQUIRE(put.has_value());
        GLIFI_REQUIRE(send_all(socket, *put));
        const auto frame = receive_response(socket);
        const auto response = glifistore::server::decode_response(frame);
        GLIFI_REQUIRE(response.has_value());
        GLIFI_REQUIRE(response->frame.request_id == 200U + static_cast<std::uint64_t>(index));
        GLIFI_REQUIRE(response->frame.status == glifistore::server::ResponseStatus::overloaded);
        GLIFI_REQUIRE(failure.write_records >= 1);

        const auto get = glifistore::server::encode_request({
            .opcode = glifistore::server::RequestOpcode::get,
            .request_id = 210U + static_cast<std::uint64_t>(index),
            .key = key,
        });
        GLIFI_REQUIRE(get.has_value());
        GLIFI_REQUIRE(send_all(socket, *get));
        const auto get_frame = receive_response(socket);
        const auto get_response = glifistore::server::decode_response(get_frame);
        GLIFI_REQUIRE(get_response.has_value());
        GLIFI_REQUIRE(get_response->frame.status == glifistore::server::ResponseStatus::not_found);

        static_cast<void>(::close(socket));
        server.request_stop();
        GLIFI_REQUIRE(server.join().has_value());
    }
}
