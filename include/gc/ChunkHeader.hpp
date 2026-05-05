#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace gc {

using DestructorFn = void (*)(void*);

inline constexpr std::uint32_t kChunkMagicNumber = 0x47434C42U;

struct alignas(8) ChunkHeader {
    std::size_t size = 0;
    DestructorFn destructor = nullptr;
    std::uint32_t magic_number = kChunkMagicNumber;
    std::uint16_t payload_offset = 0;
    std::uint8_t marked = 0;
    std::uint8_t alignment_shift = 0;

    bool is_marked() const noexcept {
        return marked != 0U;
    }

    void set_marked(bool value) noexcept {
        marked = static_cast<std::uint8_t>(value);
    }

    void set_allocation_alignment(std::size_t alignment) noexcept {
        alignment_shift = static_cast<std::uint8_t>(std::countr_zero(alignment));
    }

    std::size_t allocation_alignment() const noexcept {
        return std::size_t{1} << alignment_shift;
    }
};

static_assert(sizeof(ChunkHeader) >= 16 && sizeof(ChunkHeader) <= 24,
              "ChunkHeader should remain compact.");
static_assert(sizeof(ChunkHeader) % alignof(ChunkHeader) == 0,
              "ChunkHeader size must preserve payload alignment.");

}  // namespace gc