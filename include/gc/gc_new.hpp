#pragma once

#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "gc/ChunkHeader.hpp"
#include "gc/GCManager.hpp"
#include "gc/gc_ptr.hpp"

namespace gc {

template <typename T>
gc_ptr<T[]> gc_new_array(std::size_t count);

/// Allocates and constructs one GC-managed object.
template <typename T, typename... Args>
gc_ptr<T> gc_new(Args&&... args) {
    static_assert(!std::is_array_v<T>, "Use gc_new_array<T>(count) for arrays.");

    auto& manager = GC_Manager::instance();
    manager.collect_if_needed(sizeof(T));

    void* payload =
        manager.malloc_internal(sizeof(T), alignof(T), &GC_Manager::destroy_object<T>);

    try {
        new (payload) T(std::forward<Args>(args)...);
    } catch (...) {
        manager.release_unconstructed(payload);
        throw;
    }

    gc_ptr<T> result(static_cast<T*>(payload));
    manager.collect_if_needed();
    return result;
}

/// Allocates and constructs one GC-managed object, returning null on failure.
template <typename T, typename... Args>
gc_ptr<T> gc_new_nothrow(Args&&... args) noexcept {
    try {
        return gc_new<T>(std::forward<Args>(args)...);
    } catch (...) {
        return gc_ptr<T>();
    }
}

/// Allocates a GC-managed array of default-constructed elements, returning null on failure.
template <typename T>
gc_ptr<T[]> gc_new_array_nothrow(std::size_t count) noexcept {
    try {
        return gc_new_array<T>(count);
    } catch (...) {
        return gc_ptr<T[]>();
    }
}

/// Allocates a GC-managed array of default-constructed elements.
///
/// Layout of the allocated GC block:
///   [ size_t count ][ T elem[0] ][ T elem[1] ] ... [ T elem[count-1] ]
///
/// The returned gc_ptr<T[]> points at elem[0], and the collector resolves that
/// interior pointer back to the owning allocation during tracing.

namespace detail {

constexpr bool add_would_overflow(std::size_t lhs, std::size_t rhs) noexcept {
    return lhs > std::numeric_limits<std::size_t>::max() - rhs;
}

constexpr bool multiply_would_overflow(std::size_t lhs, std::size_t rhs) noexcept {
    return rhs != 0U && lhs > std::numeric_limits<std::size_t>::max() / rhs;
}

constexpr std::size_t round_up_to_alignment(std::size_t value,
                                            std::size_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

template <typename T>
constexpr std::size_t array_elements_offset() noexcept {
    static_assert(alignof(T) != 0U);
    return round_up_to_alignment(sizeof(std::size_t), alignof(T));
}

template <typename T>
T* array_elements_begin(void* payload) noexcept {
    auto* bytes = static_cast<unsigned char*>(payload);
    return reinterpret_cast<T*>(bytes + array_elements_offset<T>());
}

template <typename T>
struct ArrayDestructor {
    static void destroy(void* payload) noexcept {
        const std::size_t count = *static_cast<std::size_t*>(payload);
        T* elems = array_elements_begin<T>(payload);
        for (std::size_t i = count; i > 0U; --i) {
            elems[i - 1U].~T();
        }
    }
};

}  // namespace detail

template <typename T>
gc_ptr<T[]> gc_new_array(std::size_t count) {
    static_assert(!std::is_array_v<T>, "T must be an element type, not an array type.");

    if (count == 0U) {
        return gc_ptr<T[]>();
    }

    auto& manager = GC_Manager::instance();

    // Payload = [size_t count prefix][padding][T elements...]
    const std::size_t prefix_size = detail::array_elements_offset<T>();
    if (detail::multiply_would_overflow(sizeof(T), count) ||
        detail::add_would_overflow(prefix_size, sizeof(T) * count)) {
        throw std::bad_alloc();
    }

    const std::size_t elems_size = sizeof(T) * count;
    const std::size_t payload_size = prefix_size + elems_size;

    // Use max alignment of size_t and T for the whole payload.
    const std::size_t alignment = alignof(T) >= alignof(std::size_t)
                                      ? alignof(T)
                                      : alignof(std::size_t);

    manager.collect_if_needed(payload_size);

    void* payload = manager.malloc_internal(
        payload_size, alignment,
        &detail::ArrayDestructor<T>::destroy);

    auto* prefix = static_cast<std::size_t*>(payload);
    T* elems = detail::array_elements_begin<T>(payload);

    // Write count before constructing elements so the destructor is safe
    // even if a constructor throws mid-way.
    *prefix = 0U;

    std::size_t constructed = 0U;
    try {
        for (; constructed < count; ++constructed) {
            new (elems + constructed) T();
            *prefix = constructed + 1U;  // keep count current for the destructor
        }
    } catch (...) {
        // Destroy successfully-constructed elements, then release the block.
        for (std::size_t i = constructed; i > 0U; --i) {
            elems[i - 1U].~T();
        }
        manager.release_unconstructed(payload);
        throw;
    }

    gc_ptr<T[]> result(elems);
    manager.collect_if_needed();
    return result;
}

}  // namespace gc
