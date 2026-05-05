#pragma once

#include <cstddef>
#include <type_traits>

namespace gc {

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

    constexpr T* get() const noexcept {
        return ptr_;
    }

    constexpr T& operator*() const noexcept {
        return *ptr_;
    }

    constexpr T* operator->() const noexcept {
        return ptr_;
    }

    constexpr explicit operator bool() const noexcept {
        return ptr_ != nullptr;
    }

    constexpr void reset(T* ptr = nullptr) noexcept {
        ptr_ = ptr;
    }

private:
    template <typename U>
    friend class gc_ptr;

    T* ptr_ = nullptr;
};

template <typename T, typename U>
constexpr bool operator==(const gc_ptr<T>& lhs, const gc_ptr<U>& rhs) noexcept {
    return lhs.get() == rhs.get();
}

template <typename T>
constexpr bool operator==(const gc_ptr<T>& lhs, std::nullptr_t) noexcept {
    return lhs.get() == nullptr;
}

}  // namespace gc