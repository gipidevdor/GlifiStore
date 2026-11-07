#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace glifistore {

enum class ErrorCode {
    invalid_argument,
    arithmetic_overflow,
    record_too_large,
    segment_full,
    segment_sealed,
    invalid_record,
    checksum_mismatch,
    invalid_reference,
    sequence_conflict,
    corrupted_data,
    not_found,
    resource_exhausted,
    storage_exhausted,
    file_too_large,
    descriptor_exhausted,
    read_only_filesystem,
    internal_error,
    unavailable,
    io_error,
};

struct Error {
    ErrorCode code{ErrorCode::invalid_argument};
    std::string message;
    // Portable client-semantics v1 enrichment. Store paths leave these empty;
    // TCP clients populate them when known (docs/spec/client-semantics-v1.md §2.1).
    std::string category{};
    std::optional<std::uint16_t> wire_status{};
    std::size_t bytes_sent{};
    std::optional<std::uint64_t> request_id{};
    std::optional<std::uint32_t> worker{};
    std::optional<std::uint64_t> routing_epoch{};
    std::string retryability{};
    std::string operation{};
    // Standalone PUT/ERASE (and pipeline mutation positions): "committed" / "rejected" /
    // "indeterminate". Empty on non-mutation errors and Store-embedded paths.
    std::string mutation_outcome{};
};

struct Unexpected {
    Error error;
};

template <typename T> class [[nodiscard]] Result final {
  public:
    Result(T value) : storage_(std::move(value)) {}

    Result(Unexpected failure) : storage_(std::move(failure.error)) {}

    Result(Result&&) noexcept = default;
    auto operator=(Result&&) noexcept -> Result& = default;
    Result(const Result&) = delete;
    auto operator=(const Result&) -> Result& = delete;

    [[nodiscard]] auto has_value() const noexcept -> bool {
        return std::holds_alternative<T>(storage_);
    }

    explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] auto value() & -> T& {
        return std::get<T>(storage_);
    }

    [[nodiscard]] auto value() const& -> const T& {
        return std::get<T>(storage_);
    }

    [[nodiscard]] auto value() && -> T&& {
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] auto error() & -> Error& {
        return std::get<Error>(storage_);
    }

    [[nodiscard]] auto error() const& -> const Error& {
        return std::get<Error>(storage_);
    }

    [[nodiscard]] auto operator*() & -> T& {
        return value();
    }

    [[nodiscard]] auto operator*() const& -> const T& {
        return value();
    }

    [[nodiscard]] auto operator->() -> T* {
        return &value();
    }

    [[nodiscard]] auto operator->() const -> const T* {
        return &value();
    }

  private:
    std::variant<T, Error> storage_;
};

template <> class [[nodiscard]] Result<void> final {
  public:
    Result() = default;

    Result(Unexpected failure) : error_(std::move(failure.error)) {}

    [[nodiscard]] auto has_value() const noexcept -> bool {
        return !error_.has_value();
    }

    explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] auto error() & -> Error& {
        if (!error_) {
            throw std::bad_optional_access{};
        }
        return *error_;
    }

    [[nodiscard]] auto error() const& -> const Error& {
        if (!error_) {
            throw std::bad_optional_access{};
        }
        return *error_;
    }

  private:
    std::optional<Error> error_;
};

using Status = Result<void>;

inline auto unexpected(Error error) -> Unexpected {
    return Unexpected{std::move(error)};
}

inline auto fail(ErrorCode code, std::string message) -> Unexpected {
    return unexpected(Error{code, std::move(message)});
}

} // namespace glifistore
