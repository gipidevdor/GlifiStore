/**
 * @file
 * @brief Owning read results and contiguous byte storage with a small-value inline buffer.
 */
#pragma once

#include "glifistore/segment/record.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace glifistore {

/// Owning value bytes with small-value SSO (inline ≤ kInlineBytes). Public reads still
/// expose contiguous bytes via data()/size()/view(); heap storage is used above the
/// inline threshold. Ownership representation only — wire and disk formats unchanged.
struct OwnedBytes final {
    static constexpr std::size_t kInlineBytes = 64;

    OwnedBytes() noexcept = default;

    OwnedBytes(const OwnedBytes& other) {
        assign(other.data(), other.size());
    }

    OwnedBytes(OwnedBytes&& other) noexcept {
        move_from(std::move(other));
    }

    auto operator=(const OwnedBytes& other) -> OwnedBytes& {
        if (this != &other) {
            assign(other.data(), other.size());
        }
        return *this;
    }

    auto operator=(OwnedBytes&& other) noexcept -> OwnedBytes& {
        if (this != &other) {
            reset();
            move_from(std::move(other));
        }
        return *this;
    }

    auto operator=(std::vector<std::byte> other) -> OwnedBytes& {
        assign(other.data(), other.size());
        return *this;
    }

    OwnedBytes(std::vector<std::byte> other) {
        assign(other.data(), other.size());
    }

    /**
     * @brief Replace the contents with size bytes, allocating above kInlineBytes.
     * @param data Source range, which may overlap this value; nullptr skips source copying.
     * @param size New length in bytes.
     * @note Invalidates previously obtained views/iterators. Allocation may throw std::bad_alloc.
     */
    void assign(const std::byte* data, const std::size_t size) {
        if (size <= kInlineBytes) {
            if (size != 0 && data != nullptr) {
                // The source may be a subrange of this value, including its heap.
                std::memmove(inline_.data(), data, size);
            }
            heap_.reset();
            size_ = size;
            return;
        }
        auto heap = std::make_unique<std::byte[]>(size);
        if (size != 0 && data != nullptr) {
            std::memcpy(heap.get(), data, size);
        }
        heap_ = std::move(heap);
        size_ = size;
    }

    /**
     * @brief Copy the half-open source range into owned storage.
     * @pre begin and end delimit a valid ordered range in the same array.
     * @see assign(const std::byte*, std::size_t)
     */
    void assign(const std::byte* begin, const std::byte* end) {
        assign(begin, static_cast<std::size_t>(end - begin));
    }

    [[nodiscard]] auto data() noexcept -> std::byte* {
        return heap_ ? heap_.get() : inline_.data();
    }

    [[nodiscard]] auto data() const noexcept -> const std::byte* {
        return heap_ ? heap_.get() : inline_.data();
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return size_;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return size_ == 0;
    }

    [[nodiscard]] auto begin() noexcept -> std::byte* {
        return data();
    }

    [[nodiscard]] auto end() noexcept -> std::byte* {
        return data() + size_;
    }

    [[nodiscard]] auto begin() const noexcept -> const std::byte* {
        return data();
    }

    [[nodiscard]] auto end() const noexcept -> const std::byte* {
        return data() + size_;
    }

    /// @pre The buffer is nonempty; no bounds check is performed.
    [[nodiscard]] auto front() noexcept -> std::byte& {
        return data()[0];
    }

    /// @pre The buffer is nonempty; no bounds check is performed.
    [[nodiscard]] auto front() const noexcept -> const std::byte& {
        return data()[0];
    }

    /// @pre The buffer is nonempty; no bounds check is performed.
    [[nodiscard]] auto back() noexcept -> std::byte& {
        return data()[size_ - 1];
    }

    /// @pre The buffer is nonempty; no bounds check is performed.
    [[nodiscard]] auto back() const noexcept -> const std::byte& {
        return data()[size_ - 1];
    }

    /// @pre index < size(); the returned reference borrows this buffer.
    [[nodiscard]] auto operator[](const std::size_t index) noexcept -> std::byte& {
        return data()[index];
    }

    /// @pre index < size(); the returned reference borrows this buffer.
    [[nodiscard]] auto operator[](const std::size_t index) const noexcept -> const std::byte& {
        return data()[index];
    }

    [[nodiscard]] friend auto operator==(const OwnedBytes& left, const OwnedBytes& right) noexcept -> bool {
        if (left.size_ != right.size_) {
            return false;
        }
        return left.size_ == 0 || std::memcmp(left.data(), right.data(), left.size_) == 0;
    }

    /**
     * @brief Borrow contiguous bytes until assignment, move, or destruction of this buffer.
     * @note No copy or ownership transfer occurs.
     */
    [[nodiscard]] operator std::span<const std::byte>() const noexcept {
        return {data(), size_};
    }

    /**
     * @brief Allocate an independent vector copy of the current bytes.
     */
    [[nodiscard]] auto as_vector() const -> std::vector<std::byte> {
        return {data(), data() + size_};
    }

  private:
    void reset() noexcept {
        heap_.reset();
        size_ = 0;
    }

    void move_from(OwnedBytes&& other) noexcept {
        size_ = other.size_;
        heap_ = std::move(other.heap_);
        if (!heap_ && size_ != 0) {
            std::memcpy(inline_.data(), other.inline_.data(), size_);
        }
        other.size_ = 0;
    }

    std::size_t size_{};
    std::array<std::byte, kInlineBytes> inline_{};
    std::unique_ptr<std::byte[]> heap_{};
};

/**
 * @brief Independent value bytes and metadata copied from one visible Record.
 * @note Owning results survive Store mutation, close, and destruction. view() borrows
 * this object's bytes and follows OwnedBytes invalidation rules.
 */
struct OwnedValue {
    OwnedBytes bytes{};                       ///< Owning contiguous value storage.
    std::uint64_t sequence{};                 ///< Sequence of the selected visible Record.
    std::uint64_t expire_at_ns{};             ///< Absolute Unix nanoseconds; zero means no expiration.
    ValueType storage_type{ValueType::bytes}; ///< Representation tag of the returned value.

    [[nodiscard]] auto view() const noexcept -> std::span<const std::byte> {
        return bytes;
    }

    /**
     * @brief Copy borrowed bytes and attach the supplied sequence, expiry, and storage type.
     */
    [[nodiscard]] static auto from_bytes(const std::span<const std::byte> value, const std::uint64_t sequence,
                                         const std::uint64_t expire_at_ns,
                                         ValueType storage_type = ValueType::bytes) -> OwnedValue {
        OwnedValue owned;
        owned.bytes.assign(value.data(), value.size());
        owned.sequence = sequence;
        owned.expire_at_ns = expire_at_ns;
        owned.storage_type = storage_type;
        return owned;
    }
};

} // namespace glifistore
