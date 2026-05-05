#pragma once

#include <cstdint>
#include <type_traits>

#include "gc/GCManager.hpp"
#include "gc/gc_ptr.hpp"

namespace gc {

/// A non-owning handle to a GC-managed object.
/// The pointer is nulled out automatically when the GC reclaims the target.
/// gc_weak_ptrs must not outlive the GC_Manager singleton.
template <typename T>
class gc_weak_ptr {
    static_assert(!std::is_void_v<T>, "gc_weak_ptr<void> is not supported.");

public:
    gc_weak_ptr() noexcept = default;

    explicit gc_weak_ptr(const gc_ptr<T>& p) : ptr_(p.get()) {
        if (ptr_) {
            GC_Manager::instance().register_weak_ref(
                reinterpret_cast<std::uintptr_t>(ptr_),
                reinterpret_cast<void**>(&ptr_));
        }
    }

    ~gc_weak_ptr() {
        if (ptr_) {
            GC_Manager::instance().unregister_weak_ref(
                reinterpret_cast<void**>(&ptr_));
        }
    }

    gc_weak_ptr(const gc_weak_ptr& other) : ptr_(other.ptr_) {
        if (ptr_) {
            GC_Manager::instance().register_weak_ref(
                reinterpret_cast<std::uintptr_t>(ptr_),
                reinterpret_cast<void**>(&ptr_));
        }
    }

    gc_weak_ptr(gc_weak_ptr&& other) noexcept : ptr_(other.ptr_) {
        if (ptr_) {
            GC_Manager::instance().update_weak_ref(
                reinterpret_cast<void**>(&other.ptr_),
                reinterpret_cast<void**>(&ptr_));
            other.ptr_ = nullptr;
        }
    }

    gc_weak_ptr& operator=(const gc_weak_ptr& other) {
        if (this == &other) {
            return *this;
        }
        reset();
        ptr_ = other.ptr_;
        if (ptr_) {
            GC_Manager::instance().register_weak_ref(
                reinterpret_cast<std::uintptr_t>(ptr_),
                reinterpret_cast<void**>(&ptr_));
        }
        return *this;
    }

    gc_weak_ptr& operator=(gc_weak_ptr&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        ptr_ = other.ptr_;
        if (ptr_) {
            GC_Manager::instance().update_weak_ref(
                reinterpret_cast<void**>(&other.ptr_),
                reinterpret_cast<void**>(&ptr_));
            other.ptr_ = nullptr;
        }
        return *this;
    }

    gc_weak_ptr& operator=(const gc_ptr<T>& p) {
        reset();
        ptr_ = p.get();
        if (ptr_) {
            GC_Manager::instance().register_weak_ref(
                reinterpret_cast<std::uintptr_t>(ptr_),
                reinterpret_cast<void**>(&ptr_));
        }
        return *this;
    }

    /// Returns a strong gc_ptr if the target is still alive, nullptr otherwise.
    gc_ptr<T> lock() const noexcept {
        return ptr_ ? gc_ptr<T>(ptr_) : gc_ptr<T>();
    }

    bool expired() const noexcept { return ptr_ == nullptr; }

    void reset() noexcept {
        if (ptr_) {
            GC_Manager::instance().unregister_weak_ref(
                reinterpret_cast<void**>(&ptr_));
            ptr_ = nullptr;
        }
    }

private:
    // The GC nulls this out (via stored void**) when the target block is swept.
    T* ptr_ = nullptr;
};

}  // namespace gc
