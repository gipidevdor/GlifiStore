#pragma once

// History recorder + linearizability checker for GlifiStore key operations
// (GET/PUT/ERASE/TTL/RAW/close/compaction). Requirement: GS-CONCUR-LIN-001.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace glifistore::test::lin {

enum class OpKind : std::uint8_t {
    get,
    put,
    erase,
    put_ttl,
    raw_get,
    raw_put,
    raw_erase,
    compact,
    close,
};

enum class OutcomeKind : std::uint8_t {
    ok_void,
    ok_value,
    not_found,
    rejected,
    unavailable,
    other_error,
};

struct Operation final {
    std::size_t id{};
    OpKind kind{};
    std::string key;
    std::string value;
    std::uint64_t expire_at_ns{};
    std::uint64_t now_ns{};
    OutcomeKind outcome{OutcomeKind::ok_void};
    std::string result_value;
    std::uint64_t call_tick{};
    std::uint64_t return_tick{};
    bool completed{false};
};

struct Entry final {
    std::string value;
    std::uint64_t expire_at_ns{};
};

struct ModelState final {
    std::unordered_map<std::string, Entry> map;
    bool closed{false};
};

struct CheckResult final {
    bool linearizable{true};
    bool inconclusive{false};
    std::string message;
    std::vector<Operation> minimal_trace;
};

[[nodiscard]] inline auto kind_name(const OpKind kind) -> const char* {
    switch (kind) {
    case OpKind::get:
        return "GET";
    case OpKind::put:
        return "PUT";
    case OpKind::erase:
        return "ERASE";
    case OpKind::put_ttl:
        return "PUT_TTL";
    case OpKind::raw_get:
        return "RAW_GET";
    case OpKind::raw_put:
        return "RAW_PUT";
    case OpKind::raw_erase:
        return "RAW_ERASE";
    case OpKind::compact:
        return "COMPACT";
    case OpKind::close:
        return "CLOSE";
    }
    return "?";
}

[[nodiscard]] inline auto outcome_name(const OutcomeKind outcome) -> const char* {
    switch (outcome) {
    case OutcomeKind::ok_void:
        return "ok";
    case OutcomeKind::ok_value:
        return "ok_value";
    case OutcomeKind::not_found:
        return "not_found";
    case OutcomeKind::rejected:
        return "rejected";
    case OutcomeKind::unavailable:
        return "unavailable";
    case OutcomeKind::other_error:
        return "other_error";
    }
    return "?";
}

[[nodiscard]] inline auto format_operation(const Operation& op) -> std::string {
    std::ostringstream out;
    out << '#' << op.id << ' ' << kind_name(op.kind);
    if (!op.key.empty())
        out << " key=" << op.key;
    if (op.kind == OpKind::put || op.kind == OpKind::put_ttl || op.kind == OpKind::raw_put) {
        out << " value=" << op.value;
        if (op.expire_at_ns != 0)
            out << " expire=" << op.expire_at_ns;
    }
    out << " [" << op.call_tick << ',' << op.return_tick << "] -> " << outcome_name(op.outcome);
    if (op.outcome == OutcomeKind::ok_value)
        out << '(' << op.result_value << ')';
    return out.str();
}

[[nodiscard]] inline auto live(const Entry& entry, const std::uint64_t now_ns) -> bool {
    return entry.expire_at_ns == 0 || now_ns == 0 || entry.expire_at_ns > now_ns;
}

[[nodiscard]] inline auto apply(ModelState& state, const Operation& op) -> bool {
    if (!op.completed)
        return false;
    if (state.closed)
        return op.outcome == OutcomeKind::unavailable;

    switch (op.kind) {
    case OpKind::get:
    case OpKind::raw_get: {
        const auto it = state.map.find(op.key);
        if (it == state.map.end() || !live(it->second, op.now_ns)) {
            if (it != state.map.end() && !live(it->second, op.now_ns))
                state.map.erase(it);
            return op.outcome == OutcomeKind::not_found;
        }
        if (op.outcome != OutcomeKind::ok_value)
            return false;
        return op.result_value == it->second.value;
    }
    case OpKind::put:
    case OpKind::put_ttl:
    case OpKind::raw_put: {
        if (op.outcome == OutcomeKind::rejected)
            return true;
        if (op.outcome == OutcomeKind::unavailable)
            return false;
        if (op.outcome != OutcomeKind::ok_void)
            return false;
        if (op.expire_at_ns != 0 && op.now_ns != 0 && op.expire_at_ns <= op.now_ns) {
            state.map.erase(op.key);
            return true;
        }
        state.map.insert_or_assign(op.key, Entry{.value = op.value, .expire_at_ns = op.expire_at_ns});
        return true;
    }
    case OpKind::erase:
    case OpKind::raw_erase: {
        if (op.outcome == OutcomeKind::rejected)
            return true;
        if (op.outcome == OutcomeKind::unavailable)
            return false;
        const auto erased = state.map.erase(op.key);
        return erased == 0U ? op.outcome == OutcomeKind::not_found : op.outcome == OutcomeKind::ok_void;
    }
    case OpKind::compact: {
        return op.outcome == OutcomeKind::ok_void || op.outcome == OutcomeKind::rejected;
    }
    case OpKind::close: {
        if (op.outcome == OutcomeKind::ok_void || op.outcome == OutcomeKind::unavailable) {
            state.closed = true;
            return true;
        }
        return false;
    }
    }
    return false;
}

[[nodiscard]] inline auto precedes(const Operation& a, const Operation& b) -> bool {
    return a.completed && b.completed && a.return_tick < b.call_tick;
}

inline auto check_history(const std::vector<Operation>& history, const std::size_t max_states = 500'000)
    -> CheckResult {
    std::vector<const Operation*> ops;
    ops.reserve(history.size());
    for (const auto& op : history)
        if (op.completed)
            ops.push_back(&op);
    const auto n = ops.size();
    if (n == 0)
        return CheckResult{.linearizable = true, .message = "empty history", .minimal_trace = {}};

    std::vector<std::vector<std::size_t>> must_before(n);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < n; ++j)
            if (i != j && precedes(*ops[i], *ops[j]))
                must_before[j].push_back(i);

    std::vector<std::uint8_t> done(n, 0);
    ModelState state;
    std::size_t states_visited = 0;
    bool found = false;
    std::unordered_set<std::string> seen;

    const auto state_fingerprint = [&]() {
        std::ostringstream out;
        out << (state.closed ? 'C' : 'O') << ';';
        std::vector<std::string> keys;
        keys.reserve(state.map.size());
        for (const auto& [key, _] : state.map)
            keys.push_back(key);
        std::sort(keys.begin(), keys.end());
        for (const auto& key : keys) {
            const auto& entry = state.map.at(key);
            out << key << '=' << entry.value << '@' << entry.expire_at_ns << ';';
        }
        return out.str();
    };
    const auto encode_done = [&]() {
        std::string bits(n, '0');
        for (std::size_t i = 0; i < n; ++i)
            if (done[i])
                bits[i] = '1';
        return bits;
    };

    auto dfs = [&](auto&& self, const std::size_t remaining) -> void {
        if (found || states_visited >= max_states)
            return;
        ++states_visited;
        if (remaining == 0) {
            found = true;
            return;
        }
        if (!seen.insert(encode_done() + '|' + state_fingerprint()).second)
            return;
        for (std::size_t i = 0; i < n; ++i) {
            if (done[i])
                continue;
            bool ready = true;
            for (const auto pred : must_before[i])
                if (!done[pred]) {
                    ready = false;
                    break;
                }
            if (!ready)
                continue;
            ModelState checkpoint = state;
            if (!apply(state, *ops[i])) {
                state = std::move(checkpoint);
                continue;
            }
            done[i] = 1;
            self(self, remaining - 1);
            done[i] = 0;
            state = std::move(checkpoint);
            if (found)
                return;
        }
    };
    dfs(dfs, n);

    CheckResult result;
    if (found) {
        result.linearizable = true;
        result.message = "history is linearizable (" + std::to_string(states_visited) + " states)";
        return result;
    }
    if (states_visited >= max_states) {
        result.linearizable = false;
        result.inconclusive = true;
        result.message = "checker state budget exhausted; treat as inconclusive";
        result.minimal_trace.reserve(n);
        for (const auto* op : ops)
            result.minimal_trace.push_back(*op);
        return result;
    }
    result.linearizable = false;
    result.message = "no valid linearization";
    result.minimal_trace.reserve(n);
    for (const auto* op : ops)
        result.minimal_trace.push_back(*op);
    return result;
}

[[nodiscard]] inline auto history_has_matching_write(const std::vector<Operation>& history,
                                                     const Operation& get_op) -> bool {
    const bool successful_erase = (get_op.kind == OpKind::erase || get_op.kind == OpKind::raw_erase) &&
                                  get_op.outcome == OutcomeKind::ok_void;
    if (get_op.outcome != OutcomeKind::ok_value && !successful_erase)
        return true;
    for (const auto& op : history) {
        if (op.id == get_op.id || !op.completed)
            continue;
        if ((op.kind == OpKind::put || op.kind == OpKind::put_ttl || op.kind == OpKind::raw_put) &&
            op.key == get_op.key && (successful_erase || op.value == get_op.result_value) &&
            op.outcome == OutcomeKind::ok_void)
            return true;
    }
    return false;
}

inline auto minimize_failing(std::vector<Operation> history) -> std::vector<Operation> {
    auto current = std::move(history);
    bool changed = true;
    while (changed && current.size() > 1) {
        changed = false;
        for (std::size_t i = 0; i < current.size();) {
            std::vector<Operation> trial;
            trial.reserve(current.size() - 1);
            for (std::size_t j = 0; j < current.size(); ++j)
                if (j != i)
                    trial.push_back(current[j]);
            bool explains_gets = true;
            for (const auto& op : trial) {
                if ((op.kind == OpKind::get || op.kind == OpKind::raw_get || op.kind == OpKind::erase ||
                     op.kind == OpKind::raw_erase) &&
                    !history_has_matching_write(trial, op)) {
                    explains_gets = false;
                    break;
                }
            }
            // Only shrink on definite failures; budget exhaustion must not drive minimization.
            const auto trial_result = check_history(trial);
            if (explains_gets && !trial_result.linearizable && !trial_result.inconclusive) {
                current = std::move(trial);
                changed = true;
                i = 0;
            } else
                ++i;
        }
    }
    return current;
}

[[nodiscard]] inline auto check_and_minimize(const std::vector<Operation>& history) -> CheckResult {
    auto result = check_history(history);
    if (result.linearizable || result.inconclusive)
        return result;
    result.minimal_trace = minimize_failing(result.minimal_trace);
    std::ostringstream out;
    out << result.message << "; minimal failing trace (" << result.minimal_trace.size() << " ops):\n";
    for (const auto& op : result.minimal_trace)
        out << "  " << format_operation(op) << '\n';
    result.message = out.str();
    return result;
}

class HistoryRecorder final {
  public:
    [[nodiscard]] auto begin(const OpKind kind, std::string key = {}, std::string value = {},
                             const std::uint64_t expire_at_ns = 0, const std::uint64_t now_ns = 0)
        -> std::size_t {
        Operation op;
        op.id = next_id_.fetch_add(1, std::memory_order_relaxed);
        op.kind = kind;
        op.key = std::move(key);
        op.value = std::move(value);
        op.expire_at_ns = expire_at_ns;
        op.now_ns = now_ns;
        op.call_tick = tick_.fetch_add(1, std::memory_order_acq_rel);
        const auto id = op.id;
        {
            const std::lock_guard lock{mutex_};
            ops_.push_back(std::move(op));
        }
        return id;
    }

    void complete(const std::size_t id, const OutcomeKind outcome, std::string result_value = {}) {
        const auto return_tick = tick_.fetch_add(1, std::memory_order_acq_rel);
        const std::lock_guard lock{mutex_};
        for (auto& op : ops_) {
            if (op.id == id) {
                op.return_tick = return_tick;
                op.outcome = outcome;
                op.result_value = std::move(result_value);
                op.completed = true;
                return;
            }
        }
    }

    [[nodiscard]] auto snapshot() const -> std::vector<Operation> {
        const std::lock_guard lock{mutex_};
        return ops_;
    }

  private:
    mutable std::mutex mutex_;
    std::vector<Operation> ops_;
    std::atomic<std::size_t> next_id_{};
    std::atomic<std::uint64_t> tick_{0};
};

} // namespace glifistore::test::lin
