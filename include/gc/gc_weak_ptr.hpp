#pragma once

#include <cstdint>
#include <type_traits>

#include "gc/GCManager.hpp"
#include "gc/gc_ptr.hpp"

namespace gc {

namespace detail {

inline constexpr std::uintptr_t kWeakPtrMask =
    static_cast<std::uintptr_t>(0x9E3779B97F4A7C10ULL);

inline std::uintptr_t encode_weak_ptr(const void* ptr) noexcept {
    if (ptr == nullptr) {
        return 0U;
    }
    return (reinterpret_cast<std::uintptr_t>(ptr) ^ kWeakPtrMask) | 1U;
}

template <typename T>
inline T* decode_weak_ptr(std::uintptr_t encoded) noexcept {
    if (encoded == 0U) {
        return nullptr;
    }
    const auto raw = (encoded & ~std::uintptr_t{1}) ^ kWeakPtrMask;
    return reinterpret_cast<T*>(raw);
}

}  // namespace detail

/// A non-owning handle to a GC-managed object.
/// The pointer is nulled out automatically when the GC reclaims the target.
/// gc_weak_ptrs must not outlive the GC_Manager singleton.
template <typename T>
class gc_weak_ptr {
    static_assert(!std::is_void_v<T>, "gc_weak_ptr<void> is not supported.");

public:
    gc_weak_ptr() noexcept = default;

    explicit gc_weak_ptr(const gc_ptr<T>& p)
        : encoded_ptr_(detail::encode_weak_ptr(p.get())) {
        if (encoded_ptr_ != 0U) {
            GC_Manager::instance().register_weak_ref(
                reinterpret_cast<std::uintptr_t>(p.get()),
                &encoded_ptr_);
        }
    }

    ~gc_weak_ptr() {
        if (encoded_ptr_ != 0U) {
            GC_Manager::instance().unregister_weak_ref(&encoded_ptr_);
        }
    }

    gc_weak_ptr(const gc_weak_ptr& other) : encoded_ptr_(other.encoded_ptr_) {
        if (encoded_ptr_ != 0U) {
            GC_Manager::instance().register_weak_ref(
                reinterpret_cast<std::uintptr_t>(detail::decode_weak_ptr<T>(encoded_ptr_)),
                &encoded_ptr_);
        }
    }

    gc_weak_ptr(gc_weak_ptr&& other) noexcept : encoded_ptr_(other.encoded_ptr_) {
        if (encoded_ptr_ != 0U) {
            GC_Manager::instance().update_weak_ref(
                &other.encoded_ptr_,
                &encoded_ptr_);
            other.encoded_ptr_ = 0U;
        }
    }

    gc_weak_ptr& operator=(const gc_weak_ptr& other) {
        if (this == &other) {
            return *this;
        }
        reset();
        encoded_ptr_ = other.encoded_ptr_;
        if (encoded_ptr_ != 0U) {
            GC_Manager::instance().register_weak_ref(
                reinterpret_cast<std::uintptr_t>(detail::decode_weak_ptr<T>(encoded_ptr_)),
                &encoded_ptr_);
        }
        return *this;
    }

    gc_weak_ptr& operator=(gc_weak_ptr&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        encoded_ptr_ = other.encoded_ptr_;
        if (encoded_ptr_ != 0U) {
            GC_Manager::instance().update_weak_ref(
                &other.encoded_ptr_,
                &encoded_ptr_);
            other.encoded_ptr_ = 0U;
        }
        return *this;
    }

    gc_weak_ptr& operator=(const gc_ptr<T>& p) {
        reset();
        encoded_ptr_ = detail::encode_weak_ptr(p.get());
        if (encoded_ptr_ != 0U) {
            GC_Manager::instance().register_weak_ref(
                reinterpret_cast<std::uintptr_t>(p.get()),
                &encoded_ptr_);
        }
        return *this;
    }

    /// Returns a strong gc_ptr if the target is still alive, nullptr otherwise.
    gc_ptr<T> lock() const noexcept {
        T* ptr = detail::decode_weak_ptr<T>(encoded_ptr_);
        if (ptr == nullptr) {
            return gc_ptr<T>();
        }
        return GC_Manager::instance().is_live(ptr) ? gc_ptr<T>(ptr) : gc_ptr<T>();
    }

    bool expired() const noexcept {
        return detail::decode_weak_ptr<T>(encoded_ptr_) == nullptr ||
               !GC_Manager::instance().is_live(detail::decode_weak_ptr<T>(encoded_ptr_));
    }

    void reset() noexcept {
        if (encoded_ptr_ != 0U) {
            GC_Manager::instance().unregister_weak_ref(&encoded_ptr_);
            encoded_ptr_ = 0U;
        }
    }

private:
    // Encoded so conservative root scanning does not treat weak refs as strong roots.
    std::uintptr_t encoded_ptr_ = 0U;
};

}  // namespace gc
