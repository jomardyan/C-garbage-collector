#pragma once

#include <cstddef>
#include <functional>
#include <type_traits>

namespace gc {

/// Non-owning strong handle to a GC-managed object.
template <typename T>
class gc_ptr {
public:
    using element_type = T;

    constexpr gc_ptr() noexcept = default;
    constexpr gc_ptr(std::nullptr_t) noexcept {}
    explicit constexpr gc_ptr(T* ptr) noexcept : ptr_(ptr) {}

    template <typename U>
        requires std::is_convertible_v<U*, T*>
    constexpr gc_ptr(const gc_ptr<U>& other) noexcept : ptr_(other.get()) {}

    template <typename U>
        requires std::is_convertible_v<U*, T*>
    constexpr gc_ptr& operator=(const gc_ptr<U>& other) noexcept {
        ptr_ = other.get();
        return *this;
    }

    constexpr gc_ptr& operator=(std::nullptr_t) noexcept {
        ptr_ = nullptr;
        return *this;
    }

    constexpr T* get() const noexcept { return ptr_; }

    constexpr T& operator*() const noexcept {
        static_assert(!std::is_void_v<T>, "gc_ptr<void> cannot be dereferenced.");
        return *ptr_;
    }

    constexpr T* operator->() const noexcept {
        static_assert(!std::is_void_v<T>, "gc_ptr<void> does not support operator->.");
        return ptr_;
    }

    constexpr explicit operator bool() const noexcept { return ptr_ != nullptr; }

    constexpr void reset(T* ptr = nullptr) noexcept { ptr_ = ptr; }

private:
    template <typename U>
    friend class gc_ptr;

    T* ptr_ = nullptr;
};

/// Array specialization used by gc_new_array().
template <typename T>
class gc_ptr<T[]> {
public:
    using element_type = T;

    constexpr gc_ptr() noexcept = default;
    constexpr gc_ptr(std::nullptr_t) noexcept {}
    explicit constexpr gc_ptr(T* ptr) noexcept : ptr_(ptr) {}

    constexpr T* get() const noexcept { return ptr_; }

    constexpr T& operator[](std::size_t index) const noexcept { return ptr_[index]; }

    constexpr explicit operator bool() const noexcept { return ptr_ != nullptr; }

    constexpr void reset(T* ptr = nullptr) noexcept { ptr_ = ptr; }

private:
    T* ptr_ = nullptr;
};

// --- Equality ---
template <typename T, typename U>
constexpr bool operator==(const gc_ptr<T>& lhs, const gc_ptr<U>& rhs) noexcept {
    return lhs.get() == rhs.get();
}

template <typename T>
constexpr bool operator==(const gc_ptr<T>& lhs, std::nullptr_t) noexcept {
    return lhs.get() == nullptr;
}

// --- Ordering (required for std::map / std::set keys) ---
template <typename T, typename U>
constexpr bool operator<(const gc_ptr<T>& lhs, const gc_ptr<U>& rhs) noexcept {
    return std::less<const void*>{}(static_cast<const void*>(lhs.get()),
                                    static_cast<const void*>(rhs.get()));
}

// --- Swap ---
template <typename T>
constexpr void swap(gc_ptr<T>& lhs, gc_ptr<T>& rhs) noexcept {
    T* tmp = lhs.get();
    lhs.reset(rhs.get());
    rhs.reset(tmp);
}

/// Casts a gc_ptr using static_cast semantics.
template <typename To, typename From>
gc_ptr<To> static_gc_ptr_cast(const gc_ptr<From>& p) noexcept {
    return gc_ptr<To>(static_cast<To*>(p.get()));
}

/// Casts a gc_ptr using dynamic_cast semantics.
template <typename To, typename From>
gc_ptr<To> dynamic_gc_ptr_cast(const gc_ptr<From>& p) noexcept {
    return gc_ptr<To>(dynamic_cast<To*>(p.get()));
}

/// Casts away constness from a gc_ptr target.
template <typename To, typename From>
gc_ptr<To> const_gc_ptr_cast(const gc_ptr<From>& p) noexcept {
    return gc_ptr<To>(const_cast<To*>(p.get()));
}

}  // namespace gc

// --- STL hash specialisation (enables use in unordered_map / unordered_set) ---
namespace std {

template <typename T>
struct hash<gc::gc_ptr<T>> {
    std::size_t operator()(const gc::gc_ptr<T>& p) const noexcept {
        return std::hash<T*>{}(p.get());
    }
};

}  // namespace std
