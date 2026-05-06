#pragma once

#include <type_traits>

#include "gc/GCManager.hpp"
#include "gc/gc_ptr.hpp"

namespace gc {

/// Exact RAII root for a GC-managed object.
///
/// Unlike a plain stack gc_ptr, gc_root registers its own storage as an
/// explicit root range, so the collector does not rely on compiler register
/// spilling to keep the contained pointer visible.
template <typename T>
class gc_root {
    static_assert(!std::is_void_v<T>, "gc_root<void> is not supported.");

public:
    gc_root() { register_self(); }

    gc_root(std::nullptr_t) { register_self(); }

    explicit gc_root(const gc_ptr<T>& ptr) : ptr_(ptr) { register_self(); }

    template <typename U>
        requires std::is_convertible_v<U*, T*>
    gc_root(const gc_ptr<U>& ptr) : ptr_(ptr) {
        register_self();
    }

    gc_root(const gc_root& other) : ptr_(other.ptr_) { register_self(); }

    gc_root(gc_root&& other) noexcept : ptr_(other.ptr_) {
        register_self();
        other.ptr_ = nullptr;
    }

    ~gc_root() { unregister_self(); }

    gc_root& operator=(const gc_root& other) noexcept {
        if (this != &other) {
            ptr_ = other.ptr_;
        }
        return *this;
    }

    gc_root& operator=(gc_root&& other) noexcept {
        if (this != &other) {
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    gc_root& operator=(const gc_ptr<T>& ptr) noexcept {
        ptr_ = ptr;
        return *this;
    }

    gc_root& operator=(std::nullptr_t) noexcept {
        ptr_ = nullptr;
        return *this;
    }

    T* get() const noexcept { return ptr_.get(); }
    T& operator*() const noexcept { return *ptr_; }
    T* operator->() const noexcept { return ptr_.operator->(); }
    explicit operator bool() const noexcept { return static_cast<bool>(ptr_); }
    void reset(T* ptr = nullptr) noexcept { ptr_.reset(ptr); }
    gc_ptr<T> as_gc_ptr() const noexcept { return ptr_; }

private:
    void register_self() {
        GC_Manager::instance().register_root_object(this);
    }

    void unregister_self() noexcept {
        GC_Manager::instance().unregister_root_object(this);
    }

    gc_ptr<T> ptr_;
};

template <typename T>
class gc_root<T[]> {
public:
    gc_root() { register_self(); }

    gc_root(std::nullptr_t) { register_self(); }

    explicit gc_root(const gc_ptr<T[]>& ptr) : ptr_(ptr) { register_self(); }

    gc_root(const gc_root& other) : ptr_(other.ptr_) { register_self(); }

    gc_root(gc_root&& other) noexcept : ptr_(other.ptr_) {
        register_self();
        other.ptr_ = nullptr;
    }

    ~gc_root() { unregister_self(); }

    gc_root& operator=(const gc_root& other) noexcept {
        if (this != &other) {
            ptr_ = other.ptr_;
        }
        return *this;
    }

    gc_root& operator=(gc_root&& other) noexcept {
        if (this != &other) {
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    gc_root& operator=(const gc_ptr<T[]>& ptr) noexcept {
        ptr_ = ptr;
        return *this;
    }

    gc_root& operator=(std::nullptr_t) noexcept {
        ptr_ = nullptr;
        return *this;
    }

    T* get() const noexcept { return ptr_.get(); }
    T& operator[](std::size_t index) const noexcept { return ptr_[index]; }
    explicit operator bool() const noexcept { return static_cast<bool>(ptr_); }
    void reset(T* ptr = nullptr) noexcept { ptr_.reset(ptr); }
    gc_ptr<T[]> as_gc_ptr() const noexcept { return ptr_; }

private:
    void register_self() {
        GC_Manager::instance().register_root_object(this);
    }

    void unregister_self() noexcept {
        GC_Manager::instance().unregister_root_object(this);
    }

    gc_ptr<T[]> ptr_;
};

}  // namespace gc