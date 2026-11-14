/**
 * @file
 * @brief Fallible value/status results and portable error context.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace glifistore {

/**
 * @brief Stable programmatic failure categories; diagnostic message text is not a contract.
 * @note not_found also represents an expired or tombstoned key on Store reads.
 */
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

/**
 * @brief Owning error plus optional wire/client diagnostic context.
 *
 * Embedded Store paths generally leave transport fields empty. Retryability and
 * mutation_outcome describe known context; an error alone does not prove rollback.
 */
struct Error {
    ErrorCode code{ErrorCode::invalid_argument}; ///< Programmatic error category.
    std::string message; ///< Diagnostic text; callers must not parse it for control flow.
    /// Portable client-semantics v1 enrichment. Store paths leave these empty;
    /// TCP clients populate them when known (docs/spec/client-semantics-v1.md §2.1).
    std::string category{};                     ///< Portable client taxonomy category when known.
    std::optional<std::uint16_t> wire_status{}; ///< Decoded wire status, if a response was received.
    std::size_t bytes_sent{}; ///< Transport bytes sent for the affected operation when tracked.
    std::optional<std::uint64_t> request_id{};    ///< Wire request identifier when assigned.
    std::optional<std::uint32_t> worker{};        ///< Target owner connection when known.
    std::optional<std::uint64_t> routing_epoch{}; ///< Routing identity used by the request when known.
    std::string retryability{}; ///< Portable retry classification when populated by the client.
    std::string operation{};    ///< Client operation name when known.
    /// Standalone PUT/ERASE (and pipeline mutation positions): "committed" / "rejected" /
    /// "indeterminate". Empty on non-mutation errors and Store-embedded paths.
    std::string mutation_outcome{}; ///< Committed, rejected, or indeterminate context for a mutation.
};

/**
 * @brief Owning error wrapper used to construct a failed Result.
 */
struct Unexpected {
    Error error;
};

/**
 * @brief Move-only discriminated value-or-error result.
 * @tparam T Owned success payload.
 * @note Inspect has_value() before accessing a branch. value(), error(), and the
 * pointer/dereference operators use std::get and throw std::bad_variant_access
 * when the wrong alternative is selected. Returned references borrow this Result.
 */
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

/**
 * @brief Success-or-error result for operations without a payload.
 * @note Default construction means success. error() on success throws
 * std::bad_optional_access; an error reference borrows this Result.
 */
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

/**
 * @brief Fallible operation without a success payload; inspect before accessing error().
 */
using Status = Result<void>;

/**
 * @brief Move an owning Error into the failure wrapper expected by Result constructors.
 */
inline auto unexpected(Error error) -> Unexpected {
    return Unexpected{std::move(error)};
}

/**
 * @brief Build an owning failure from a programmatic category and diagnostic message.
 */
inline auto fail(ErrorCode code, std::string message) -> Unexpected {
    return unexpected(Error{code, std::move(message)});
}

} // namespace glifistore
