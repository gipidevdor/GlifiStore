#include "allocation_fault_test_support.hpp"
#include "allocation_fault_tests_decl.hpp"
#include "glifistore/client/client.hpp"
#include "glifistore/server/server.hpp"
#include "glifistore/store/paired/lane_publication.hpp"
#include "store/paired/shard_pair_runtime_impl.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>

namespace allocation_fault_test {
namespace {

class ClientTestServer final {
  public:
    ClientTestServer() {
        auto created =
            glifistore::server::Server::create({.port = 0, .maximum_connections = 16, .worker_count = 1});
        require(created.has_value(), "client fault Server creation failed");
        server_ = std::move(*created);
        require(server_->start().has_value(), "client fault Server start failed");
    }

    ~ClientTestServer() {
        server_->request_stop();
        static_cast<void>(server_->join());
    }

    auto connect() const -> glifistore::client::Client {
        auto connected = glifistore::client::Client::connect({.port = server_->port()});
        require(connected.has_value(), "client fault connection failed");
        return std::move(*connected);
    }

  private:
    std::unique_ptr<glifistore::server::Server> server_;
};

} // namespace

void run_client_mutation_allocation_failures() {
    using namespace glifistore::client;
    ClientTestServer server;
    auto observer = server.connect();
    for (const bool erase : {false, true}) {
        bool covered_post_send{};
        bool reached_success{};
        for (std::size_t fail_at = 0; fail_at < 128; ++fail_at) {
            require(observer.put("fault-key", "before").committed(), "client fault seeding failed");
            auto client = server.connect();
            allocation_fault::arm(fail_at);
            auto result = erase ? client.erase("fault-key") : client.put("fault-key", "after");
            const auto observation = allocation_fault::disarm();
            // Reading on the same client also proves that a partially read response
            // was discarded before the connection is used for the next operation.
            auto read = client.get("fault-key");
            const bool changed = erase ? !read && read.error().code == glifistore::ErrorCode::not_found
                                       : read && read->size() == 5 &&
                                             std::string_view{reinterpret_cast<const char*>(read->data()),
                                                              read->size()} == "after";
            if (changed) {
                require(result.outcome != MutationOutcome::rejected,
                        "client reported rejection for an applied mutation after OOM");
            }
            if (observation.fired && result.outcome == MutationOutcome::indeterminate) {
                require(result.error.has_value(), "indeterminate allocation failure has no error");
                require(result.error->bytes_sent > 0, "post-send allocation failure lost send progress");
                require(result.error->category == "overloaded", "allocation failure category changed");
                require(result.error->mutation_outcome == "indeterminate", "allocation outcome changed");
                require(result.error->retryability == "reconcile_first", "OOM allowed a blind retry");
                require(result.error->request_id.has_value(), "post-send OOM lost request id");
                covered_post_send = true;
            }
            if (!observation.fired) {
                require(result.committed() && changed, "client fault sweep did not finish successfully");
                reached_success = true;
                break;
            }
        }
        require(covered_post_send && reached_success, "client OOM sweep missed a response allocation");
    }
    std::cout << "client mutation allocation failures preserve outcomes and reset connections\n";
}

void run_client_late_response_allocation() {
    using namespace glifistore::client;
    ClientTestServer server;
    auto client = server.connect();
    auto observer = server.connect();
    for (const bool erase : {false, true}) {
        allocation_fault::delay_next_allocation_of_size(glifistore::server::kResponseHeaderBytes,
                                                        std::chrono::milliseconds{100});
        auto result = erase ? client.erase("late-key", {.timeout = std::chrono::milliseconds{50}})
                            : client.put("late-key", "after", {}, {.timeout = std::chrono::milliseconds{50}});
        const auto delayed = allocation_fault::end_allocation_delay();
        require(delayed, "late-response test did not suspend response allocation");
        require(result.outcome == MutationOutcome::indeterminate, "late mutation response was accepted");
        require(result.error && result.error->category == "transport", "late response was not transport");
        require(result.error->bytes_sent > 0, "late response lost mutation send progress");
        require(result.error->retryability == "reconcile_first", "late mutation allowed a blind retry");
        auto read = observer.get("late-key");
        require(erase ? !read && read.error().code == glifistore::ErrorCode::not_found : bool(read),
                "deadline cancelled an already applied mutation");
        require(client.ping().has_value(), "client did not reconnect after late response");
    }
    std::cout << "client late mutation responses respect the absolute deadline\n";
}

void run_slot_pool_mirror_without_allocation() {
    using namespace glifistore::store::paired;
    auto pool = GenerationSlotPool::create({});
    require(pool.has_value(), "slot pool allocation test creation failed");
    GenerationState generation;
    generation.slot_pool = std::move(*pool);
    allocation_fault::begin_forbid_all();
    auto view = runtime_detail::non_owning_generation_view(generation.slot_pool->writer_generation());
    mirror_slot_pool_publication(generation);
    mirror_slot_pool_publication(generation);
    const auto allocated = allocation_fault::end_forbid_all();
    require(!allocated, "non-owning generation publication allocated a control block");
    require(view.get() == generation.writer_generation.get(), "slot pool observer identity changed");
    generation.writer_generation.reset();
    generation.slot_pool->stop_admission();
    generation.slot_pool->revoke_publication();
    require(generation.slot_pool->mark_reader_quiescent(), "slot pool reader did not quiesce");
    require(generation.slot_pool->try_finish_shutdown(), "slot pool publication did not drain");
    std::cout << "slot pool publication mirrors allocate no control blocks\n";
}

} // namespace allocation_fault_test
