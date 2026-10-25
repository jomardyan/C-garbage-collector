#pragma once

#include <cstddef>
#include <initializer_list>
#include <type_traits>
#include <utility>
#include <vector>

#include "gc/gc_new.hpp"
#include "gc/gc_root.hpp"

namespace gc {

/// A vector of exact roots, including when the vector itself is heap hosted.
/// Each element is a gc_root<T>; iterators and references follow std::vector's
/// invalidation rules. Managed objects retain their addresses during growth.
template <typename T>
class gc_vector {
    static_assert(!std::is_void_v<T>, "gc_vector<void> is not supported.");
    using storage_type = std::vector<gc_root<T>>;

public:
    using value_type = gc_root<T>;
    using pointer_type = gc_ptr<T>;
    using size_type = typename storage_type::size_type;
    using reference = value_type&;
    using const_reference = const value_type&;
    using iterator = typename storage_type::iterator;
    using const_iterator = typename storage_type::const_iterator;

    gc_vector() = default;
    explicit gc_vector(size_type count) : roots_(count) {}

    gc_vector(std::initializer_list<pointer_type> pointers) {
        roots_.reserve(pointers.size());
        for (const auto& pointer : pointers) roots_.emplace_back(pointer);
    }

    gc_vector(const gc_vector&) = default;
    gc_vector(gc_vector&&) noexcept = default;
    gc_vector& operator=(const gc_vector&) = default;
    gc_vector& operator=(gc_vector&&) noexcept = default;

    [[nodiscard]] bool empty() const noexcept { return roots_.empty(); }
    [[nodiscard]] size_type size() const noexcept { return roots_.size(); }
    [[nodiscard]] size_type capacity() const noexcept { return roots_.capacity(); }

    reference operator[](size_type index) noexcept { return roots_[index]; }
    const_reference operator[](size_type index) const noexcept { return roots_[index]; }
    reference at(size_type index) { return roots_.at(index); }
    const_reference at(size_type index) const { return roots_.at(index); }
    reference front() noexcept { return roots_.front(); }
    const_reference front() const noexcept { return roots_.front(); }
    reference back() noexcept { return roots_.back(); }
    const_reference back() const noexcept { return roots_.back(); }

    iterator begin() noexcept { return roots_.begin(); }
    const_iterator begin() const noexcept { return roots_.begin(); }
    const_iterator cbegin() const noexcept { return roots_.cbegin(); }
    iterator end() noexcept { return roots_.end(); }
    const_iterator end() const noexcept { return roots_.end(); }
    const_iterator cend() const noexcept { return roots_.cend(); }

    void reserve(size_type capacity) { roots_.reserve(capacity); }
    void shrink_to_fit() { roots_.shrink_to_fit(); }
    void resize(size_type count) { roots_.resize(count); }
    void clear() noexcept { roots_.clear(); }
    void push_back(const pointer_type& pointer) { roots_.emplace_back(pointer); }
    void pop_back() { roots_.pop_back(); }
    iterator erase(const_iterator position) { return roots_.erase(position); }
    iterator erase(const_iterator first, const_iterator last) { return roots_.erase(first, last); }

    /// Constructs and roots a new object, returning its lightweight handle.
    template <typename... Args>
        requires (!std::is_array_v<T>)
    pointer_type emplace_new(Args&&... args) {
        // Keep the new target rooted even if vector growth registers more roots
        // and parks this thread before the new element has been inserted.
        gc_root<T> created(gc_new<T>(std::forward<Args>(args)...));
        roots_.push_back(created);
        return created.as_gc_ptr();
    }

    void swap(gc_vector& other) noexcept { roots_.swap(other.roots_); }

private:
    storage_type roots_;
};

template <typename T>
void swap(gc_vector<T>& left, gc_vector<T>& right) noexcept {
    left.swap(right);
}

}  // namespace gc
