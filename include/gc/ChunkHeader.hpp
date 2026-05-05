#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace gc {

using DestructorFn = void (*)(void*);

inline constexpr std::uint32_t kChunkMagicNumber = 0x47434C42U;

// payload_offset is stored as uint16_t; alignments > 65535 bytes are rejected.
struct alignas(8) ChunkHeader {
    std::size_t size = 0;
    DestructorFn destructor = nullptr;
    std::uint32_t magic_number = kChunkMagicNumber;
    std::uint16_t payload_offset = 0;
    // Atomic so concurrent/incremental marking never races on this flag.
    std::atomic<std::uint8_t> marked{0};
    std::uint8_t alignment_shift = 0;
    std::uint8_t reserved[8] = {};

    bool is_marked() const noexcept {
        return marked.load(std::memory_order_relaxed) != 0U;
    }

    void set_marked(bool value) noexcept {
        marked.store(static_cast<std::uint8_t>(value), std::memory_order_relaxed);
    }

    void set_allocation_alignment(std::size_t alignment) noexcept {
        alignment_shift = static_cast<std::uint8_t>(std::countr_zero(alignment));
    }

    std::size_t allocation_alignment() const noexcept {
        return std::size_t{1} << alignment_shift;
    }
};

static_assert(sizeof(ChunkHeader) == 32,
              "ChunkHeader layout changed; update allocator metadata assumptions.");
static_assert(std::has_single_bit(sizeof(ChunkHeader)),
              "ChunkHeader size must stay a power of two.");
static_assert(sizeof(ChunkHeader) % alignof(ChunkHeader) == 0,
              "ChunkHeader size must preserve payload alignment.");

}  // namespace gc
