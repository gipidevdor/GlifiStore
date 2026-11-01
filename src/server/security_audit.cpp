#include "glifistore/server/security_audit.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

namespace glifistore::server {

struct SecurityAuditOutputCounters final {
    std::atomic<std::uint64_t> suppressed{};
    std::atomic<std::uint64_t> written{};
    std::atomic<std::uint64_t> pending{};
    std::atomic<std::uint64_t> failed{};
};

struct SecurityAuditSink final {
    struct Event final {
        std::string line;
        std::shared_ptr<SecurityAuditOutputCounters> counters;
    };

    explicit SecurityAuditSink(const int descriptor) noexcept : descriptor{descriptor} {}
    ~SecurityAuditSink() {
        if (descriptor >= 0) {
            static_cast<void>(::close(descriptor));
        }
    }

    int descriptor;
    std::mutex mutex;
    std::condition_variable ready;
    std::array<Event, 128> queue{};
    std::size_t head{};
    std::size_t size{};
    std::chrono::steady_clock::time_point admission_window{};
    std::uint32_t admitted_in_window{};
    bool failed{};
};

namespace {

constexpr std::size_t kMaxLoggedFieldBytes = 256U;
constexpr std::size_t kMaxEventBytes = 8U * 1024U;
constexpr std::uint32_t kMaximumEventsPerSecond = 128;

auto within_rate_limit(std::chrono::steady_clock::time_point& window, std::uint32_t& count) noexcept -> bool {
    const auto now = std::chrono::steady_clock::now();
    if (window.time_since_epoch().count() == 0 || now - window >= std::chrono::seconds{1}) {
        window = now;
        count = 0;
    }
    if (count >= kMaximumEventsPerSecond) {
        return false;
    }
    ++count;
    return true;
}

void emit_events(const std::shared_ptr<SecurityAuditSink>& sink) noexcept {
    // Linux/BSD generate write's SIGPIPE on the writing thread. Darwin additionally
    // needs F_SETNOSIGPIPE below because its pipe EPIPE signal is process-directed.
    sigset_t signals;
    static_cast<void>(sigemptyset(&signals));
    static_cast<void>(sigaddset(&signals, SIGPIPE));
    const bool can_write = ::pthread_sigmask(SIG_BLOCK, &signals, nullptr) == 0;
    std::chrono::steady_clock::time_point window{};
    std::uint32_t emitted_in_window{};
    for (;;) {
        SecurityAuditSink::Event event;
        bool available = false;
        {
            std::unique_lock lock{sink->mutex};
            sink->ready.wait(lock, [&] { return sink->size != 0; });
            event = std::move(sink->queue[sink->head]);
            sink->head = (sink->head + 1U) % sink->queue.size();
            --sink->size;
            available = can_write && !sink->failed;
        }
        bool written = available && within_rate_limit(window, emitted_in_window);
        bool failed = !can_write;
        std::size_t offset = 0;
        while (written && offset < event.line.size()) {
            const auto result =
                ::write(sink->descriptor, event.line.data() + offset, event.line.size() - offset);
            if (result > 0) {
                offset += static_cast<std::size_t>(result);
            } else if (result < 0 && errno == EINTR) {
                continue;
            } else if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // Preserve the current line across nonblocking backpressure.
                // Only this process-lifetime emitter may wait for the sink.
                pollfd interest{.fd = sink->descriptor, .events = POLLOUT, .revents = 0};
                int ready = 0;
                do {
                    ready = ::poll(&interest, 1, -1);
                } while (ready < 0 && errno == EINTR);
                if (ready > 0 && (interest.revents & POLLOUT) != 0 &&
                    (interest.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0) {
                    continue;
                }
                written = false;
                failed = true;
            } else {
                written = false;
                failed = true;
            }
        }
        if (failed) {
            // A hard error may have left a truncated record. Do not append a
            // subsequent event to that prefix. Reinitialization is unsupported.
            const std::lock_guard lock{sink->mutex};
            sink->failed = true;
            event.counters->failed.fetch_add(1U, std::memory_order_relaxed);
        }
        (written ? event.counters->written : event.counters->suppressed)
            .fetch_add(1U, std::memory_order_relaxed);
        event.counters->pending.fetch_sub(1U, std::memory_order_release);
    }
}

auto process_sink() noexcept -> std::shared_ptr<SecurityAuditSink> {
    // Exactly one worker and owned descriptor per process, including repeated
    // Server creation/destruction. The worker owns its state and no Server/Audit
    // pointer. It is deliberately never joined: an inherited blocking file/pipe
    // must not make Server destruction wait. Process exit reclaims this resource.
    static const auto sink = []() noexcept -> std::shared_ptr<SecurityAuditSink> {
        try {
            auto state = std::make_shared<SecurityAuditSink>(-1);
            state->descriptor = ::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
            if (state->descriptor < 0) {
                return {};
            }
#if defined(__APPLE__)
            // This is an open-file-description attribute shared with stderr and
            // its duplicates. Leave O_NONBLOCK and process signal policy alone.
            if (::fcntl(state->descriptor, F_SETNOSIGPIPE, 1) < 0) {
                return {};
            }
#endif
            std::thread{emit_events, state}.detach();
            return state;
        } catch (...) {
            return {}; // Every subsequent eligible event is counted as suppressed.
        }
    }();
    return sink;
}

[[nodiscard]] auto json_escape(const std::string_view text) -> std::string {
    std::string out;
    const auto limit = text.size() > kMaxLoggedFieldBytes ? kMaxLoggedFieldBytes : text.size();
    out.reserve(limit + 16U);
    for (std::size_t index = 0; index < limit; ++index) {
        const char ch = text[index];
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
                out.push_back(ch);
            }
            break;
        }
    }
    if (text.size() > kMaxLoggedFieldBytes) {
        out += "...";
    }
    return out;
}

[[nodiscard]] auto epoch_seconds() noexcept -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

SecurityAudit::SecurityAudit(const bool emit_events, const bool quiet)
    : emit_events_{emit_events}, quiet_{quiet}, output_{std::make_shared<SecurityAuditOutputCounters>()},
      sink_{emit_events ? process_sink() : nullptr} {}

void SecurityAudit::write_event(const std::string_view event, const std::string_view principal,
                                const std::string_view opcode, const std::string_view reason,
                                const std::string_view decision) noexcept {
    if (!emit_events_) {
        return;
    }
    try {
        if (!sink_) {
            output_->suppressed.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        const std::unique_lock lock{sink_->mutex, std::try_to_lock};
        if (!lock.owns_lock() || sink_->failed || sink_->size == sink_->queue.size() ||
            !within_rate_limit(sink_->admission_window, sink_->admitted_in_window)) {
            output_->suppressed.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        std::string line;
        line.reserve(256U);
        line += "{\"ts\":";
        line += std::to_string(epoch_seconds());
        line += ",\"event\":\"";
        line += event;
        line += '"';
        if (!decision.empty()) {
            line += ",\"decision\":\"";
            line += decision;
            line += '"';
        }
        if (!principal.empty()) {
            line += ",\"principal\":\"";
            line += json_escape(principal);
            line += '"';
        }
        if (!opcode.empty()) {
            line += ",\"opcode\":\"";
            line += json_escape(opcode);
            line += '"';
        }
        if (!reason.empty()) {
            line += ",\"reason\":\"";
            line += json_escape(reason);
            line += '"';
        }
        line += "}\n";
        if (line.size() > kMaxEventBytes) {
            output_->suppressed.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        auto& entry = sink_->queue[(sink_->head + sink_->size) % sink_->queue.size()];
        entry.line = std::move(line);
        entry.counters = output_;
        output_->pending.fetch_add(1U, std::memory_order_relaxed);
        ++sink_->size;
        sink_->ready.notify_one();
    } catch (...) {
        output_->suppressed.fetch_add(1U, std::memory_order_relaxed);
        // Audit must never throw into the reactor accept/request path.
        return;
    }
}

void SecurityAudit::auth_accept(const std::string_view principal) noexcept {
    auth_accepts_.fetch_add(1, std::memory_order_relaxed);
    if (quiet_) {
        return;
    }
    write_event("auth", principal, {}, {}, "accept");
}

void SecurityAudit::auth_deny(const std::string_view reason, const std::string_view principal) noexcept {
    auth_denies_.fetch_add(1, std::memory_order_relaxed);
    write_event("auth", principal, {}, reason, "deny");
}

void SecurityAudit::authz_deny(const std::string_view principal, const std::string_view opcode,
                               const std::string_view reason) noexcept {
    authz_denies_.fetch_add(1, std::memory_order_relaxed);
    write_event("authz", principal, opcode, reason, "deny");
}

void SecurityAudit::tls_error(const std::string_view reason) noexcept {
    tls_errors_.fetch_add(1, std::memory_order_relaxed);
    write_event("tls", {}, {}, reason, "error");
}

auto SecurityAudit::stats() const noexcept -> SecurityAuditStats {
    const auto pending = output_->pending.load(std::memory_order_acquire);
    return SecurityAuditStats{
        .auth_accepts = auth_accepts_.load(std::memory_order_relaxed),
        .auth_denies = auth_denies_.load(std::memory_order_relaxed),
        .authz_denies = authz_denies_.load(std::memory_order_relaxed),
        .tls_errors = tls_errors_.load(std::memory_order_relaxed),
        .events_suppressed = output_->suppressed.load(std::memory_order_relaxed),
        .events_written = output_->written.load(std::memory_order_relaxed),
        .events_pending = pending,
        .events_write_failed = output_->failed.load(std::memory_order_relaxed),
    };
}

} // namespace glifistore::server
