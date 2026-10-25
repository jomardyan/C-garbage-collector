#include "gc/GC.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <thread>
#include <utility>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Base {};
struct Derived : Base {};
static_assert(std::is_constructible_v<gc::gc_ptr<void>, gc::gc_ptr<int>>);
static_assert(std::is_constructible_v<gc::gc_ptr<const int[]>, gc::gc_ptr<int[]>>);
static_assert(!std::is_constructible_v<gc::gc_ptr<Base[]>, gc::gc_ptr<Derived[]>>);
static_assert(!std::is_nothrow_move_constructible_v<gc::gc_root<int>>);
static_assert(!std::is_nothrow_move_constructible_v<gc::gc_root<int[]>>);

struct CollectingConstructor {
    static inline int destroyed = 0;
    int value = 42;
    CollectingConstructor() {
        gc::collect();
        expect(destroyed == 0, "an object must not be finalized during its constructor");
        expect(value == 42, "constructor must retain access to its payload");
    }
    ~CollectingConstructor() { ++destroyed; }
};

struct NestedAllocation {
    gc::gc_ptr<int> child;
    NestedAllocation() : child(gc::gc_new<int>(12)) {
        gc::collect();
        expect(child && *child == 12, "a constructing object's child must stay live");
    }
};

struct ThrowingElement {
    static inline int attempts = 0;
    static inline int destroyed = 0;
    int id;
    ThrowingElement() : id(attempts++) {
        if (id == 2) {
            throw std::runtime_error("expected constructor failure");
        }
        gc::collect();
    }
    ~ThrowingElement() { ++destroyed; }
};

struct CustomNew {
    int value = 6;
    static void* operator new(std::size_t) = delete;
};

void test_construction() {
    auto& manager = gc::GC_Manager::instance();
    manager.shutdown();
    CollectingConstructor::destroyed = 0;
    manager.set_collection_threshold_bytes(0);
    gc::gc_root<CollectingConstructor> object(gc::gc_new<CollectingConstructor>());
    gc::collect();
    expect(object->value == 42, "a constructed value must survive threshold collection");
    expect(CollectingConstructor::destroyed == 0, "a rooted object must not be finalized");
    gc::gc_root<NestedAllocation> parent(gc::gc_new<NestedAllocation>());
    gc::collect();
    expect(*parent->child == 12, "nested allocation must be reachable from its parent");
    gc::gc_root<CustomNew> custom(gc::gc_new<CustomNew>());
    auto custom_array = gc::gc_new_array<CustomNew>(2);
    expect(custom->value == 6 && custom_array[1].value == 6,
           "class-specific operator new must not hide global placement new");
    manager.shutdown();
    expect(CollectingConstructor::destroyed == 1, "constructed object must finalize exactly once");
    manager.set_collection_threshold_bytes(10U * 1024U * 1024U);
}

void test_array_failure() {
    auto& manager = gc::GC_Manager::instance();
    manager.shutdown();
    ThrowingElement::attempts = 0;
    ThrowingElement::destroyed = 0;
    bool threw = false;
    try {
        (void)gc::gc_new_array<ThrowingElement>(4);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "array constructor failure must propagate");
    expect(ThrowingElement::destroyed == 2, "only completed array elements must be finalized");
    expect(manager.managed_block_count() == 0, "failed array allocation must release its block");
    expect(!gc::gc_new_array_nothrow<int>(std::numeric_limits<std::size_t>::max()),
           "array byte-count overflow must fail without allocation");
    manager.shutdown();
    expect(ThrowingElement::destroyed == 2, "failed array block must not be finalized again");
}

void test_pointer_utilities() {
    gc::gc_ptr<void> erased;
    auto first = gc::gc_new<int>(4);
    erased = first;
    expect(erased.get() == first.get(), "void handles must support type erasure");
    auto values = gc::gc_new_array<int>(2);
    values[1] = 9;
    gc::gc_ptr<const int[]> readonly(values);
    expect(readonly[1] == 9, "array cv conversion must preserve element access");
    gc::gc_ptr<int[]> empty;
    gc::swap(values, empty);
    expect(!values && empty[1] == 9, "array swap must exchange element pointers");
    readonly = empty;
    auto cast = gc::const_gc_ptr_cast<int[]>(readonly);
    expect(cast.get() == empty.get(), "array pointer casts must operate on the element type");
    const auto const_values = gc::gc_new_array<const int>(2);
    expect(const_values[1] == 0, "const arrays must value initialize their elements");
    gc::shutdown();
}

void test_weak_interior_addresses() {
    gc::gc_root<std::array<unsigned char, 16>> owner(gc::gc_new<std::array<unsigned char, 16>>());
    auto* odd = owner->data() + 1;
    *odd = 73;
    gc::gc_ptr<unsigned char> interior(odd);
    gc::gc_weak_ptr<unsigned char> weak(interior);
    expect(weak.lock().get() == odd && *weak.lock() == 73,
           "weak pointers must preserve odd interior addresses");
    gc::gc_weak_ptr<unsigned char> copy(weak);
    expect(copy.lock().get() == odd, "copying a weak pointer must preserve every address bit");
    gc::gc_weak_ptr<unsigned char> moved(std::move(copy));
    expect(copy.expired() && moved.lock().get() == odd,
           "moving a weak pointer must transfer its registration");
    copy = moved;
    moved = weak;
    expect(copy.lock().get() == odd && moved.lock().get() == odd,
           "weak assignment must preserve odd interior addresses");
    gc::gc_weak_ptr<unsigned char[]> array{gc::gc_ptr<unsigned char[]>(odd)};
    expect(array.lock().get() == odd, "weak arrays must preserve interior addresses");
    gc::shutdown();
    expect(weak.expired() && copy.expired() && moved.expired() && array.expired(),
           "shutdown must invalidate all weak pointer registrations");
    int unmanaged = 2;
    gc::gc_weak_ptr<int> invalid{gc::gc_ptr<int>(&unmanaged)};
    expect(invalid.expired() && !invalid.lock(), "unmanaged addresses must not create valid weak handles");
}

void test_weak_safepoint_transfers() {
    gc::gc_root<int> owner(gc::gc_new<int>(91));
    gc::gc_weak_ptr<int> source(owner.as_gc_ptr());
    std::atomic<bool> ready{false};
    std::atomic<bool> done{false};
    std::atomic<bool> invalid_value{false};
    std::thread worker([&] {
        gc::ScopedThreadRegistration registration;
        ready.store(true, std::memory_order_release);
        while (!done.load(std::memory_order_acquire)) {
            gc::gc_weak_ptr<int> copy(source);
            gc::gc_weak_ptr<int> moved(std::move(copy));
            copy = moved;
            moved = std::move(copy);
            auto locked = moved.lock();
            if (locked && *locked != 91) {
                invalid_value.store(true, std::memory_order_release);
            }
            locked.reset();
            gc::safepoint();
        }
    });
    while (!ready.load(std::memory_order_acquire)) {
        gc::safepoint();
        std::this_thread::yield();
    }
    for (int iteration = 0; iteration < 100; ++iteration) {
        if (iteration == 50) owner.reset();
        gc::collect();
    }
    done.store(true, std::memory_order_release);
    worker.join();
    expect(!invalid_value.load(std::memory_order_acquire),
           "weak copy/move/lock across safepoints must only return a live target");
    gc::shutdown();
    expect(source.expired(), "the shared weak source must expire after shutdown");
}

}  // namespace

int main() {
    try {
        gc::ScopedThreadRegistration registration;
        test_construction();
        test_array_failure();
        test_pointer_utilities();
        test_weak_interior_addresses();
        test_weak_safepoint_transfers();
        gc::shutdown();
        std::cout << "[PASS] handle and construction regressions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
