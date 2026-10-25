#include <array>
#include <atomic>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "gc/GC.hpp"

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Item {
    static inline int destroyed = 0;
    int value = 42;
    ~Item() { ++destroyed; }
};

// Run the scan on a different stack while the allocating thread is unregistered.
// This excludes stale pointer words in the caller from these exact-root tests.
void collect_from_fresh_thread() {
    gc::unregister_current_thread();
    std::exception_ptr error;
    std::thread collector([&] {
        try {
            gc::ScopedThreadRegistration registration;
            gc::collect();
        } catch (...) {
            error = std::current_exception();
        }
    });
    collector.join();
    gc::register_current_thread();
    if (error) {
        std::rethrow_exception(error);
    }
}

void test_duplicate_root_registrations_are_independent() {
    gc::ScopedThreadRegistration registration;
    gc::shutdown();
    Item::destroyed = 0;

    auto slot = std::make_unique<gc::gc_ptr<Item>>(gc::gc_new<Item>());
    auto& manager = gc::GC_Manager::instance();
    manager.register_root_object(slot.get());
    {
        gc::ScopedRootObject<gc::gc_ptr<Item>> nested(slot.get());
    }

    collect_from_fresh_thread();
    expect(Item::destroyed == 0,
           "removing a nested root must preserve the original registration");
    expect(manager.managed_block_count() == 1,
           "the remaining root must keep its target alive");

    manager.unregister_root_object(slot.get());
    slot->reset();
    collect_from_fresh_thread();
    expect(Item::destroyed == 1,
           "removing both duplicate root registrations must allow reclamation");
    gc::shutdown();
}

struct LargeItem {
    static inline int destroyed = 0;
    std::array<unsigned char, 8192> contents{};
    ~LargeItem() { ++destroyed; }
};

void test_roots_survive_shutdown_and_reuse() {
    gc::ScopedThreadRegistration registration;
    gc::shutdown();
    LargeItem::destroyed = 0;

    auto root = std::make_unique<gc::gc_root<LargeItem>>();
    *root = gc::gc_new<LargeItem>();
    gc::shutdown();
    expect(LargeItem::destroyed == 1, "shutdown must finalize the first allocation");

    // The root object itself survives shutdown and must retain its registration.
    *root = gc::gc_new<LargeItem>();
    collect_from_fresh_thread();
    expect(LargeItem::destroyed == 1,
           "a surviving gc_root must protect allocations after shutdown/reuse");
    expect(gc::GC_Manager::instance().managed_block_count() == 1,
           "a surviving explicit root must still participate in marking");

    root->reset();
    collect_from_fresh_thread();
    expect(LargeItem::destroyed == 2, "resetting the surviving root must release its target");
    gc::shutdown();
}

void expect_unregistered_allocation_rejected() {
    bool rejected = false;
    try {
        (void)gc::gc_new<Item>();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    expect(rejected, "allocating without a registration must be rejected");
}

void test_nested_thread_registration() {
    expect_unregistered_allocation_rejected();
    {
        gc::ScopedThreadRegistration outer;
        {
            gc::ScopedThreadRegistration inner;
            (void)gc::gc_new<Item>();
        }
        auto item = gc::gc_new<Item>();
        expect(item->value == 42, "an inner registration must not unregister its outer scope");
        gc::shutdown();
    }
    expect_unregistered_allocation_rejected();
}

void test_scoped_registration_preserves_manual_registration() {
    gc::register_current_thread();
    {
        gc::ScopedThreadRegistration scoped;
    }
    auto item = gc::gc_new<Item>();
    expect(item->value == 42, "a scoped registration must preserve a pre-existing manual registration");
    gc::shutdown();
    gc::unregister_current_thread();
    expect_unregistered_allocation_rejected();
}

struct CollectingConstructor {
    static inline int destroyed = 0;
    gc::gc_ptr<Item> child;
    int value = 0;

    CollectingConstructor() {
        gc::collect();
        child = gc::gc_new<Item>();
        gc::collect();
        value = 123;
    }
    ~CollectingConstructor() { ++destroyed; }
};

void test_collection_during_construction() {
    gc::ScopedThreadRegistration registration;
    gc::shutdown();
    CollectingConstructor::destroyed = 0;
    Item::destroyed = 0;

    gc::gc_root<CollectingConstructor> object(gc::gc_new<CollectingConstructor>());
    expect(object->value == 123 && object->child->value == 42,
           "collection in a constructor must preserve the object and its children");
    expect(CollectingConstructor::destroyed == 0 && Item::destroyed == 0,
           "constructing allocations must not be finalized");

    gc::gc_root<CollectingConstructor[]> array(gc::gc_new_array<CollectingConstructor>(3));
    for (std::size_t index = 0; index < 3; ++index) {
        expect(array[index].value == 123 && array[index].child->value == 42,
               "collection during array construction must preserve completed elements and children");
    }
    expect(CollectingConstructor::destroyed == 0 && Item::destroyed == 0,
           "no array element may be finalized during construction");
    gc::shutdown();
    expect(CollectingConstructor::destroyed == 4 && Item::destroyed == 4,
           "constructed objects and array elements must be finalized exactly once");
}

void test_concurrent_collection_and_snapshots() {
    constexpr std::size_t worker_count = 4;
    std::array<std::thread, worker_count> workers;
    std::array<std::exception_ptr, worker_count> errors{};
    std::atomic<std::size_t> ready{0};

    for (std::size_t index = 0; index < worker_count; ++index) {
        workers[index] = std::thread([&, index] {
            try {
                gc::ScopedThreadRegistration registration;
                gc::gc_root<Item> item(gc::gc_new<Item>());
                ready.fetch_add(1, std::memory_order_release);
                while (ready.load(std::memory_order_acquire) < worker_count) {
                    gc::safepoint();
                    std::this_thread::yield();
                }
                for (int iteration = 0; iteration < 40; ++iteration) {
                    item->value = iteration;
                    if (iteration % 2 == 0) {
                        gc::collect();
                    } else {
                        auto snapshot = gc::GC_Manager::instance().live_objects();
                        expect(!snapshot.empty(), "a heap snapshot must contain the worker's rooted object");
                    }
                    expect(item->value == iteration,
                           "concurrent collections and snapshots must preserve worker roots");
                }
            } catch (...) {
                errors[index] = std::current_exception();
                // Avoid leaving other workers waiting at the startup barrier.
                ready.store(worker_count, std::memory_order_release);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    for (const auto& error : errors) {
        if (error) std::rethrow_exception(error);
    }
    gc::shutdown();
}

struct ShutdownConstructor {
    static inline int rejected = 0;
    static inline int destroyed = 0;
    ShutdownConstructor() {
        try {
            gc::shutdown();
        } catch (const std::logic_error&) {
            ++rejected;
        }
    }
    ~ShutdownConstructor() { ++destroyed; }
};

void test_shutdown_during_construction_is_rejected() {
    gc::ScopedThreadRegistration registration;
    gc::shutdown();
    ShutdownConstructor::rejected = 0;
    ShutdownConstructor::destroyed = 0;

    gc::gc_root<ShutdownConstructor> object(gc::gc_new<ShutdownConstructor>());
    gc::gc_root<ShutdownConstructor[]> array(gc::gc_new_array<ShutdownConstructor>(3));
    expect(ShutdownConstructor::rejected == 4,
           "shutdown during object or array construction must throw std::logic_error");
    expect(ShutdownConstructor::destroyed == 0,
           "rejected shutdown must not finalize partially constructed objects");
    gc::shutdown();
    expect(ShutdownConstructor::destroyed == 4,
           "shutdown after successful construction must finalize each object once");
}

}  // namespace

int main() {
    try {
        test_duplicate_root_registrations_are_independent();
        test_roots_survive_shutdown_and_reuse();
        test_nested_thread_registration();
        test_scoped_registration_preserves_manual_registration();
        test_collection_during_construction();
        test_shutdown_during_construction_is_rejected();
        test_concurrent_collection_and_snapshots();
        std::cout << "Core regression tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Core regression failure: " << error.what() << '\n';
        return 1;
    }
}
