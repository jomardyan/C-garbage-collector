#pragma once

#include <cstddef>
#include <exception>
#include <functional>
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
        manager.malloc_internal(sizeof(T), alignof(T), &GC_Manager::destroy_object<T>, true);

    try {
        ::new (payload) T(std::forward<Args>(args)...);
    } catch (...) {
        manager.release_unconstructed(payload);
        throw;
    }

    manager.finish_construction(payload);
    return gc_ptr<T>(static_cast<T*>(payload));
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

namespace detail {

constexpr bool add_would_overflow(std::size_t lhs, std::size_t rhs) noexcept {
    return lhs > (std::numeric_limits<std::size_t>::max)() - rhs;
}

constexpr bool multiply_would_overflow(std::size_t lhs, std::size_t rhs) noexcept {
    return rhs != 0U && lhs > (std::numeric_limits<std::size_t>::max)() / rhs;
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

template <typename T, typename Construct>
gc_ptr<T[]> construct_array(std::size_t count, Construct&& construct) {
    static_assert(!std::is_array_v<T>, "T must be an element type, not an array type.");

    if (count == 0U) {
        return gc_ptr<T[]>();
    }

    auto& manager = GC_Manager::instance();

    // Payload = [size_t count prefix][padding][T elements...]
    const std::size_t prefix_size = array_elements_offset<T>();
    if (multiply_would_overflow(sizeof(T), count) ||
        add_would_overflow(prefix_size, sizeof(T) * count)) {
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
        &ArrayDestructor<T>::destroy, true);

    auto* prefix = static_cast<std::size_t*>(payload);
    T* elems = array_elements_begin<T>(payload);

    // Write count before constructing elements so the destructor is safe
    // even if a constructor throws mid-way.
    *prefix = 0U;

    std::size_t constructed = 0U;
    try {
        for (; constructed < count; ++constructed) {
            void* element = static_cast<unsigned char*>(payload) + prefix_size + constructed * sizeof(T);
            construct(element, constructed);
            *prefix = constructed + 1U;  // keep count current for the destructor
        }
    } catch (...) {
        // Preserve the construction exception when cleanup succeeds. A throwing
        // finalizer must terminate, including during partial-array cleanup;
        // unwinding past it would leave a permanently constructing GC block.
        try {
            for (std::size_t i = constructed; i > 0U; --i) {
                elems[i - 1U].~T();
            }
        } catch (...) {
            std::terminate();
        }
        manager.release_unconstructed(payload);
        throw;
    }

    manager.finish_construction(payload);
    return gc_ptr<T[]>(elems);
}

}  // namespace detail

/// Allocates a GC-managed array of value-initialized elements.
/// The returned pointer addresses element zero after an internal count prefix.
template <typename T>
gc_ptr<T[]> gc_new_array(std::size_t count) {
    return detail::construct_array<T>(count, [](void* element, std::size_t) {
        ::new (element) T();
    });
}

/// Allocates an array by copy-constructing each element from initial_value.
/// The source and any GC objects it references must remain rooted during this call.
template <typename T>
gc_ptr<T[]> gc_new_array(std::size_t count, const T& initial_value) {
    return detail::construct_array<T>(count, [&initial_value](void* element, std::size_t) {
        ::new (element) T(initial_value);
    });
}

/// Invokes generator(index), in increasing index order, to directly construct
/// each element. Supports move-only elements and move-only callable objects.
/// Captured GC objects must remain rooted across generator safepoints.
template <typename T, typename Generator>
gc_ptr<T[]> gc_generate_array(std::size_t count, Generator&& generator) {
    return detail::construct_array<T>(count, [&generator](void* element, std::size_t index) {
        ::new (element) T(std::invoke(generator, index));
    });
}

/// Returns null on allocation or element-construction failure.
template <typename T>
gc_ptr<T[]> gc_new_array_nothrow(std::size_t count) noexcept {
    try {
        return gc_new_array<T>(count);
    } catch (...) {
        return gc_ptr<T[]>();
    }
}

template <typename T>
gc_ptr<T[]> gc_new_array_nothrow(std::size_t count, const T& initial_value) noexcept {
    try {
        return gc_new_array<T>(count, initial_value);
    } catch (...) {
        return gc_ptr<T[]>();
    }
}

template <typename T, typename Generator>
gc_ptr<T[]> gc_generate_array_nothrow(std::size_t count, Generator&& generator) noexcept {
    try {
        return gc_generate_array<T>(count, std::forward<Generator>(generator));
    } catch (...) {
        return gc_ptr<T[]>();
    }
}

}  // namespace gc
