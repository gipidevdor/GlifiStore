#pragma once

// Writer-only fixed shell storage for one PairReadGenerationEnableShared.
// Occupancy is not atomic: only the owning GenerationSlotPool claims/releases.

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>

namespace glifistore::store::paired {

class PairReadGeneration;
class GenerationSlotPool;

class GenerationDirectStorage final {
  public:
    static constexpr std::size_t kBytes = 512U;
    static constexpr std::size_t kAlignment = 64U;

    GenerationDirectStorage() = default;

    ~GenerationDirectStorage() {
        if (occupied_) {
            std::terminate();
        }
    }

    GenerationDirectStorage(const GenerationDirectStorage&) = delete;
    auto operator=(const GenerationDirectStorage&) -> GenerationDirectStorage& = delete;

    [[nodiscard]] auto allocation_count() const noexcept -> std::uint64_t {
        return allocation_count_;
    }

    [[nodiscard]] auto reuse_count() const noexcept -> std::uint64_t {
        return reuse_count_;
    }

  private:
    [[nodiscard]] auto claim(const std::size_t bytes, const std::size_t alignment) -> void* {
        if (bytes > storage_.size() || alignment > kAlignment || occupied_) {
            throw std::bad_alloc{};
        }
        occupied_ = true;
        if (allocation_count_ != 0U) {
            ++reuse_count_;
        }
        ++allocation_count_;
        return storage_.data();
    }

    void release(void* pointer) noexcept {
        if (pointer == storage_.data()) {
            occupied_ = false;
        }
    }

    alignas(kAlignment) std::array<std::byte, kBytes> storage_{};
    std::uint64_t allocation_count_{};
    std::uint64_t reuse_count_{};
    bool occupied_{};

    friend class PairReadGeneration;
    friend class GenerationSlotPool;
};

} // namespace glifistore::store::paired
