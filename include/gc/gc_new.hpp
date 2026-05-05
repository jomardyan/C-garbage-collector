#pragma once

#include <type_traits>
#include <utility>

#include "gc/ChunkHeader.hpp"
#include "gc/GCManager.hpp"
#include "gc/gc_ptr.hpp"

namespace gc {

template <typename T, typename... Args>
gc_ptr<T> gc_new(Args&&... args) {
    static_assert(!std::is_array_v<T>, "gc_new does not support array types.");

    auto& manager = GC_Manager::instance();
    manager.collect_if_needed(sizeof(T));

    void* payload = manager.malloc_internal(sizeof(T), alignof(T), &GC_Manager::destroy_object<T>);
    T* object = nullptr;

    try {
        object = new (payload) T(std::forward<Args>(args)...);
    } catch (...) {
        manager.release_unconstructed(payload);
        throw;
    }

    gc_ptr<T> result(object);
    manager.collect_if_needed();
    return result;
}

}  // namespace gc