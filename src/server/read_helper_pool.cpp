#include "glifistore/server/read_helper_pool.hpp"

#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/server/read_capacity_manager.hpp"

#include <atomic>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace glifistore::server {
namespace {
enum class CellState : std::uint8_t { empty, reserved, pending, running, complete, consuming };
}

struct ReadHelperPool::Cell final {
    std::atomic<CellState> state{CellState::empty};
    std::atomic_size_t owner{kNoReadOwner};
    std::atomic_bool accepting{};
    std::atomic_bool stopping{};
    std::atomic_uint64_t signal{};
    std::thread thread{};
    std::string key{};
    std::uint64_t key_hash{};
    std::uint64_t now_ns{};
    const store::paired::PairReadGeneration* generation{};
    Wakeup* wakeup{};
    ReadHelperResult result{};
};

ReadHelperPool::ReadHelperPool(const std::size_t helpers) {
    if (helpers > kMaximumReadHelpers) {
        throw std::invalid_argument{"read helper cap exceeds 16"};
    }
    cells_.reserve(helpers);
    for (std::size_t index = 0; index < helpers; ++index) {
        auto cell = std::make_unique<Cell>();
        cell->key.reserve(1024U);
        cells_.push_back(std::move(cell));
    }
}

ReadHelperPool::~ReadHelperPool() {
    stop();
}

auto ReadHelperPool::create(const std::size_t helpers) -> Result<std::unique_ptr<ReadHelperPool>> {
    if (helpers > kMaximumReadHelpers) {
        return fail(ErrorCode::invalid_argument, "read helper cap exceeds 16");
    }
    try {
        return std::make_unique<ReadHelperPool>(helpers);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::resource_exhausted, "read helper storage allocation failed");
    }
}

auto ReadHelperPool::size() const noexcept -> std::size_t {
    return cells_.size();
}

auto ReadHelperPool::start() -> Status {
    if (started_) {
        return fail(ErrorCode::invalid_argument, "read helpers already started");
    }
    started_ = true;
    try {
        for (auto& cell : cells_) {
            cell->thread = std::thread{[this, pointer = cell.get()] { run(*pointer); }};
        }
    } catch (const std::exception&) {
        stop();
        return fail(ErrorCode::resource_exhausted, "read helper thread startup failed");
    }
    return {};
}

void ReadHelperPool::stop() noexcept {
    for (auto& cell : cells_) {
        cell->accepting.store(false, std::memory_order_release);
        cell->stopping.store(true, std::memory_order_release);
        cell->signal.fetch_add(1U, std::memory_order_release);
        cell->signal.notify_one();
    }
    for (auto& cell : cells_) {
        if (cell->thread.joinable()) {
            cell->thread.join();
        }
        cell->result = {};
        cell->generation = nullptr;
        cell->wakeup = nullptr;
    }
}

auto ReadHelperPool::assign(const std::size_t helper, const std::size_t owner) noexcept -> bool {
    if (helper >= cells_.size() || owner >= kMaximumReadOwners) {
        return false;
    }
    auto& cell = *cells_[helper];
    auto expected = CellState::empty;
    if (!cell.state.compare_exchange_strong(expected, CellState::reserved, std::memory_order_acquire)) {
        return false;
    }
    cell.owner.store(owner, std::memory_order_relaxed);
    cell.accepting.store(true, std::memory_order_release);
    cell.state.store(CellState::empty, std::memory_order_release);
    return true;
}

auto ReadHelperPool::park(const std::size_t helper) noexcept -> bool {
    if (helper >= cells_.size()) {
        return false;
    }
    auto& cell = *cells_[helper];
    // Close before checking the cell: a task racing this store either fails admission,
    // or retains the assignment until the Reactor consumes its completion.
    cell.accepting.store(false, std::memory_order_release);
    auto expected = CellState::empty;
    if (!cell.state.compare_exchange_strong(expected, CellState::reserved, std::memory_order_acquire)) {
        return false;
    }
    cell.owner.store(kNoReadOwner, std::memory_order_relaxed);
    cell.state.store(CellState::empty, std::memory_order_release);
    return true;
}

auto ReadHelperPool::try_submit(const std::size_t owner, const ConnectionToken connection,
                                const std::uint64_t request_id,
                                const store::paired::PairReadGeneration& generation,
                                const std::string_view key, const std::uint64_t key_hash,
                                const std::uint64_t now_ns, Wakeup& wakeup) noexcept -> bool {
    if (key.size() > 1024U) {
        return false;
    }
    for (auto& pointer : cells_) {
        auto& cell = *pointer;
        if (!cell.accepting.load(std::memory_order_acquire) ||
            cell.owner.load(std::memory_order_relaxed) != owner) {
            continue;
        }
        auto expected = CellState::empty;
        if (!cell.state.compare_exchange_strong(expected, CellState::reserved, std::memory_order_acquire)) {
            continue;
        }
        // Recheck after ownership: assignment may have changed between the hint and CAS.
        if (!cell.accepting.load(std::memory_order_acquire) ||
            cell.owner.load(std::memory_order_relaxed) != owner) {
            cell.state.store(CellState::empty, std::memory_order_release);
            continue;
        }
        try {
            cell.key.assign(key);
        } catch (...) {
            cell.state.store(CellState::empty, std::memory_order_release);
            return false;
        }
        cell.key_hash = key_hash;
        cell.now_ns = now_ns;
        cell.generation = &generation;
        cell.wakeup = &wakeup;
        cell.result = {.helper = static_cast<std::size_t>(&pointer - cells_.data()),
                       .connection = connection,
                       .request_id = request_id,
                       .generation_epoch = generation.epoch()};
        cell.state.store(CellState::pending, std::memory_order_release);
        cell.signal.fetch_add(1U, std::memory_order_release);
        cell.signal.notify_one();
        return true;
    }
    return false;
}

auto ReadHelperPool::take(const std::size_t owner) noexcept -> std::optional<ReadHelperResult> {
    for (auto& cell : cells_) {
        if (cell->owner.load(std::memory_order_relaxed) != owner) {
            continue;
        }
        auto expected = CellState::complete;
        if (cell->state.compare_exchange_strong(expected, CellState::consuming, std::memory_order_acquire)) {
            return std::move(cell->result);
        }
    }
    return std::nullopt;
}

void ReadHelperPool::finish(const std::size_t helper) noexcept {
    auto& cell = *cells_[helper];
    cell.result = {};
    cell.generation = nullptr;
    cell.wakeup = nullptr;
    cell.state.store(CellState::empty, std::memory_order_release);
}

void ReadHelperPool::run(Cell& cell) noexcept {
    for (;;) {
        const auto observed = cell.signal.load(std::memory_order_acquire);
        auto expected = CellState::pending;
        if (cell.state.compare_exchange_strong(expected, CellState::running, std::memory_order_acquire)) {
            const auto started_cpu_ns = read_thread_cpu_ns();
            try {
                auto record =
                    cell.generation->get_volatile_view({.key = cell.key, .hash = cell.key_hash}, cell.now_ns);
                if (record) {
                    cell.result.record = *record;
                } else {
                    cell.result.error = record.error().code;
                }
            } catch (...) {
                cell.result.error = ErrorCode::resource_exhausted;
            }
            GS_FAULT_BLOCK(read_helper_selected);
            cell.result.service_cpu_ns = read_thread_cpu_ns() - started_cpu_ns;
            auto* wakeup = cell.wakeup;
            cell.state.store(CellState::complete, std::memory_order_release);
            // Endpoint lifetime is retained through Server join, including closed connections.
            try {
                static_cast<void>(wakeup->notify());
            } catch (...) {
                // Reactor also polls completions every event-loop turn.
            }
            continue;
        }
        if (cell.stopping.load(std::memory_order_acquire)) {
            return;
        }
        cell.signal.wait(observed, std::memory_order_acquire);
    }
}

} // namespace glifistore::server
