#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "gc/GC.hpp"

namespace {

#if defined(_MSC_VER)
#define GC_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define GC_NOINLINE __attribute__((noinline))
#else
#define GC_NOINLINE
#endif

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

GC_NOINLINE std::uintptr_t consume_stack_noise(const std::uintptr_t* values,
                                              std::size_t size) {
    std::uintptr_t sink = 0;
    for (std::size_t index = 0; index < size; ++index) {
        sink ^= values[index];
    }
    return sink;
}

GC_NOINLINE void scrub_stack(int depth = 2) {
    std::uintptr_t noise[512] = {};
    for (std::size_t index = 0; index < 512; ++index) {
        noise[index] = static_cast<std::uintptr_t>(index + depth);
    }

#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : "g"(noise) : "memory");
#endif

    const std::uintptr_t sink = consume_stack_noise(noise, 512);
    if (sink == static_cast<std::uintptr_t>(-1)) {
        std::cerr << "unreachable\n";
    }

    if (depth > 0) {
        scrub_stack(depth - 1);
    }
}

struct Node {
    static inline int destroyed = 0;

    explicit Node(int value_value) : value(value_value) {}

    gc::gc_ptr<Node> next;
    int value = 0;

    ~Node() {
        ++destroyed;
    }
};

struct Buffer {
    static inline int destroyed = 0;

    std::array<int, 8> values{};

    ~Buffer() {
        ++destroyed;
    }
};

struct LargeBlob {
    static inline int destroyed = 0;

    std::array<std::byte, 1024 * 1024> payload{};

    ~LargeBlob() {
        ++destroyed;
    }
};

struct alignas(64) OverAlignedBlob {
    static inline int destroyed = 0;

    std::array<std::byte, 64> payload{};

    ~OverAlignedBlob() {
        ++destroyed;
    }
};

void reset_counts() {
    Node::destroyed = 0;
    Buffer::destroyed = 0;
    LargeBlob::destroyed = 0;
    OverAlignedBlob::destroyed = 0;
}

void reset_gc() {
    gc::GC_Manager::instance().shutdown();
    reset_counts();
    gc::GC_Manager::instance().set_collection_threshold_bytes(10U * 1024U * 1024U);
    scrub_stack();
}

void collect_until_stable() {
    auto& manager = gc::GC_Manager::instance();
    for (int pass = 0; pass < 8; ++pass) {
        scrub_stack();
        manager.collect();
        if (manager.managed_block_count() == 0U) {
            return;
        }
    }
}

GC_NOINLINE void make_cycle() {
    auto first = gc::gc_new<Node>(1);
    auto second = gc::gc_new<Node>(2);
    first->next = second;
    second->next = first;
}

void test_cycle_reclamation() {
    reset_gc();
    make_cycle();
    collect_until_stable();

    expect(gc::GC_Manager::instance().managed_block_count() == 0U,
           "unreachable cycle should be reclaimed");
    expect(Node::destroyed >= 2, "cycle destructors should run");
}

void test_reachable_chain() {
    reset_gc();

    auto root = gc::gc_new<Node>(1);
    root->next = gc::gc_new<Node>(2);
    root->next->next = gc::gc_new<Node>(3);

    gc::GC_Manager::instance().collect();
    expect(gc::GC_Manager::instance().managed_block_count() == 3U,
           "reachable chain should stay alive while rooted");

    root.reset();
    collect_until_stable();

    expect(gc::GC_Manager::instance().managed_block_count() == 0U,
           "chain should be reclaimed after the last root is dropped");
    expect(Node::destroyed >= 3, "chain destructors should run");
}

void test_deep_reachable_chain() {
    reset_gc();

    constexpr int depth = 50000;
    auto root = gc::gc_new<Node>(0);
    auto cursor = root;

    for (int value = 1; value < depth; ++value) {
        cursor->next = gc::gc_new<Node>(value);
        cursor = cursor->next;
    }

    gc::GC_Manager::instance().collect();
    expect(gc::GC_Manager::instance().managed_block_count() == static_cast<std::size_t>(depth),
           "deep reachable chain should survive collection without recursive stack overflow");

    root.reset();
    collect_until_stable();

    expect(gc::GC_Manager::instance().managed_block_count() == 0U,
           "deep chain should be reclaimed after its last root is dropped");
}

void test_interior_pointer() {
    reset_gc();

    auto buffer = gc::gc_new<Buffer>();
    volatile std::uintptr_t interior = reinterpret_cast<std::uintptr_t>(&buffer->values[3]);
    buffer.reset();

    gc::GC_Manager::instance().collect();
    expect(interior != 0U, "interior pointer should remain on the stack");
    expect(gc::GC_Manager::instance().managed_block_count() == 1U,
           "interior pointer should keep the parent block alive");

    interior = 0U;
    collect_until_stable();

    expect(gc::GC_Manager::instance().managed_block_count() == 0U,
           "object should be reclaimed once the interior pointer disappears");
}

GC_NOINLINE void make_unreachable_blob() {
    auto blob = gc::gc_new<LargeBlob>();
    (void)blob;
}

void test_threshold_trigger() {
    reset_gc();

    auto& manager = gc::GC_Manager::instance();
    manager.set_collection_threshold_bytes(sizeof(LargeBlob));

    make_unreachable_blob();
    scrub_stack();

    auto live = gc::gc_new<LargeBlob>();
    expect(static_cast<bool>(live), "gc_new should return a live pointer");
    expect(LargeBlob::destroyed >= 1,
           "threshold pressure should trigger a collection before allocating again");
    expect(manager.managed_block_count() == 1U,
           "threshold collection should leave only the new rooted allocation");

    live.reset();
    collect_until_stable();
}

    void test_over_aligned_allocation() {
        reset_gc();

        auto object = gc::gc_new<OverAlignedBlob>();
        expect(object.get() != nullptr, "over-aligned allocation should succeed");
        expect(reinterpret_cast<std::uintptr_t>(object.get()) % alignof(OverAlignedBlob) == 0U,
            "gc_new should preserve requested payload alignment");

        object.reset();
        collect_until_stable();

        expect(gc::GC_Manager::instance().managed_block_count() == 0U,
            "over-aligned object should be reclaimed normally");
        expect(OverAlignedBlob::destroyed >= 1,
            "over-aligned object destructor should run before free");
    }

    void test_registered_external_root_range() {
        reset_gc();

        auto external_root = std::make_unique<gc::gc_ptr<Node>>();
        *external_root = gc::gc_new<Node>(42);

        auto& manager = gc::GC_Manager::instance();
        manager.register_root_object(external_root.get());
        manager.collect();

        expect(manager.managed_block_count() == 1U,
            "registered external root range should keep a heap-stored gc_ptr alive");

        manager.unregister_root_object(external_root.get());
        external_root->reset();
        external_root.reset();

        collect_until_stable();

        expect(manager.managed_block_count() == 0U,
            "object should be reclaimed after unregistering the external root range");
    }

    void test_cross_thread_use_is_rejected() {
        reset_gc();

        std::exception_ptr worker_error;
        std::thread worker([&worker_error]() {
            try {
                auto object = gc::gc_new<Node>(77);
                (void)object;
            } catch (...) {
                worker_error = std::current_exception();
            }
        });

        worker.join();

        bool saw_logic_error = false;
        if (worker_error != nullptr) {
            try {
                std::rethrow_exception(worker_error);
            } catch (const std::logic_error&) {
                saw_logic_error = true;
            } catch (...) {
            }
        }

        expect(saw_logic_error,
               "collector should reject cross-thread use until a real stop-the-world implementation exists");
    }

int run_all_tests() {
    struct TestCase {
        const char* name;
        void (*function)();
    };

    const TestCase tests[] = {
        {"cycle reclamation", &test_cycle_reclamation},
        {"reachable chain", &test_reachable_chain},
        {"deep reachable chain", &test_deep_reachable_chain},
        {"interior pointer", &test_interior_pointer},
        {"threshold trigger", &test_threshold_trigger},
        {"over-aligned allocation", &test_over_aligned_allocation},
        {"registered external root range", &test_registered_external_root_range},
        {"cross-thread use is rejected", &test_cross_thread_use_is_rejected},
    };

    int failures = 0;

    for (const TestCase& test : tests) {
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }

    reset_gc();
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    (void)argv;
    gc::register_stack_bottom(&argc);
    return run_all_tests();
}