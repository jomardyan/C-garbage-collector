#pragma once

#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "gc/ChunkHeader.hpp"
#include "gc/GCManager.hpp"
#include "gc/gc_ptr.hpp"

namespace gc {

// --- gc_new<T> ---

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

// --- gc_new_nothrow<T> ---

template <typename T, typename... Args>
gc_ptr<T> gc_new_nothrow(Args&&... args) noexcept {
    try {
        return gc_new<T>(std::forward<Args>(args)...);
    } catch (...) {
        return gc_ptr<T>();
    }
}

// --- gc_new_array<T> ---
//
// Layout of the allocated GC block:
//   [ size_t count ][ T elem[0] ][ T elem[1] ] ... [ T elem[count-1] ]
//
// The gc_ptr<T[]> returned points to elem[0] (an interior pointer relative
// to the block's payload_begin).  find_block_locked() accepts interior
// pointers, so the GC correctly traces and collects the block.
//
// The destructor uses the count prefix to destroy all elements in reverse order.

namespace detail {

template <typename T>
struct ArrayDestructor {
    static void destroy(void* payload) noexcept {
        auto* prefix = static_cast<std::size_t*>(payload);
        const std::size_t count = *prefix;
        T* elems = reinterpret_cast<T*>(prefix + 1);
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

    // Payload = [size_t count prefix] + [T elements...]
    const std::size_t prefix_size = sizeof(std::size_t);
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
    T* elems = reinterpret_cast<T*>(prefix + 1);

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
