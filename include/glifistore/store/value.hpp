#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace glifistore {

// Owning value bytes with small-value SSO (inline ≤ kInlineBytes). Public reads still
// expose contiguous bytes via data()/size()/view(); heap storage is used above the
// inline threshold. Ownership representation only — wire and disk formats unchanged.
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

    void assign(const std::byte* data, const std::size_t size) {
        if (size <= kInlineBytes) {
            heap_.reset();
            if (size != 0 && data != nullptr) {
                std::memcpy(inline_.data(), data, size);
            }
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

    [[nodiscard]] auto front() noexcept -> std::byte& {
        return data()[0];
    }

    [[nodiscard]] auto front() const noexcept -> const std::byte& {
        return data()[0];
    }

    [[nodiscard]] auto back() noexcept -> std::byte& {
        return data()[size_ - 1];
    }

    [[nodiscard]] auto back() const noexcept -> const std::byte& {
        return data()[size_ - 1];
    }

    [[nodiscard]] auto operator[](const std::size_t index) noexcept -> std::byte& {
        return data()[index];
    }

    [[nodiscard]] auto operator[](const std::size_t index) const noexcept -> const std::byte& {
        return data()[index];
    }

    [[nodiscard]] friend auto operator==(const OwnedBytes& left, const OwnedBytes& right) noexcept -> bool {
        if (left.size_ != right.size_) {
            return false;
        }
        return left.size_ == 0 || std::memcmp(left.data(), right.data(), left.size_) == 0;
    }

    [[nodiscard]] operator std::span<const std::byte>() const noexcept {
        return {data(), size_};
    }

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

struct OwnedValue {
    OwnedBytes bytes{};
    std::uint64_t sequence{};
    std::uint64_t expire_at_ns{};

    [[nodiscard]] auto view() const noexcept -> std::span<const std::byte> {
        return bytes;
    }

    [[nodiscard]] static auto from_bytes(const std::span<const std::byte> value, const std::uint64_t sequence,
                                         const std::uint64_t expire_at_ns) -> OwnedValue {
        OwnedValue owned;
        owned.bytes.assign(value.data(), value.size());
        owned.sequence = sequence;
        owned.expire_at_ns = expire_at_ns;
        return owned;
    }
};

} // namespace glifistore
