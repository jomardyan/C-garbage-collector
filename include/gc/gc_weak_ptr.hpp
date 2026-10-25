#pragma once

#include <cstdint>
#include <type_traits>
#include <utility>

#include "gc/GCManager.hpp"
#include "gc/gc_ptr.hpp"

namespace gc {

namespace detail {

// Negation is reversible over uintptr_t, preserves every interior-pointer bit,
// and maps only null to zero. Encoded words avoid ordinary heap addresses on
// supported flat-address platforms, so weak handles do not act as strong roots.
inline std::uintptr_t encode_weak_ptr(const volatile void* ptr) noexcept {
    return ptr == nullptr ? 0U : std::uintptr_t{0} - reinterpret_cast<std::uintptr_t>(ptr);
}

template <typename T>
inline T* decode_weak_ptr(std::uintptr_t encoded) noexcept {
    return encoded == 0U ? nullptr : reinterpret_cast<T*>(std::uintptr_t{0} - encoded);
}

}  // namespace detail

/// A non-owning handle to a GC-managed object.
/// The pointer is nulled out automatically when the GC reclaims the target.
/// Conservative false positives can delay reclamation and therefore delay expiry.
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

    gc_weak_ptr(const gc_weak_ptr& other) {
        GC_Manager::instance().copy_weak_ref(&other.encoded_ptr_, &encoded_ptr_);
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
        gc_weak_ptr replacement(other);
        return *this = std::move(replacement);
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
        gc_weak_ptr replacement(p);
        return *this = std::move(replacement);
    }

    /// Returns a strong gc_ptr if the target is still alive, nullptr otherwise.
    gc_ptr<T> lock() const noexcept {
        const auto target = GC_Manager::instance().read_weak_ref(&encoded_ptr_);
        return target == 0U ? gc_ptr<T>()
                            : gc_ptr<T>(reinterpret_cast<T*>(target));
    }

    bool expired() const noexcept {
        return GC_Manager::instance().read_weak_ref(&encoded_ptr_) == 0U;
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

/// Array-specialized weak handle matching gc_ptr<T[]>.
/// Conservative false positives can delay reclamation and therefore delay expiry.
template <typename T>
class gc_weak_ptr<T[]> {
    static_assert(!std::is_void_v<T>, "gc_weak_ptr<void[]> is not supported.");

public:
    gc_weak_ptr() noexcept = default;

    explicit gc_weak_ptr(const gc_ptr<T[]>& p)
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

    gc_weak_ptr(const gc_weak_ptr& other) {
        GC_Manager::instance().copy_weak_ref(&other.encoded_ptr_, &encoded_ptr_);
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
        gc_weak_ptr replacement(other);
        return *this = std::move(replacement);
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

    gc_weak_ptr& operator=(const gc_ptr<T[]>& p) {
        gc_weak_ptr replacement(p);
        return *this = std::move(replacement);
    }

    /// Returns a strong gc_ptr if the target array is still alive, nullptr otherwise.
    gc_ptr<T[]> lock() const noexcept {
        const auto target = GC_Manager::instance().read_weak_ref(&encoded_ptr_);
        return target == 0U ? gc_ptr<T[]>()
                            : gc_ptr<T[]>(reinterpret_cast<T*>(target));
    }

    bool expired() const noexcept {
        return GC_Manager::instance().read_weak_ref(&encoded_ptr_) == 0U;
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
