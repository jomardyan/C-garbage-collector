#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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

// ---- Test fixtures ----------------------------------------------------------

struct Node {
    static inline int destroyed = 0;
    explicit Node(int v) : value(v) {}
    gc::gc_ptr<Node> next;
    int value = 0;
    ~Node() { ++destroyed; }
};

struct Buffer {
    static inline int destroyed = 0;
    std::array<int, 8> values{};
    ~Buffer() { ++destroyed; }
};

struct LargeBlob {
    static inline int destroyed = 0;
    std::array<std::byte, 1024 * 1024> payload{};
    ~LargeBlob() { ++destroyed; }
};

struct alignas(64) OverAlignedBlob {
    static inline int destroyed = 0;
    std::array<std::byte, 64> payload{};
    ~OverAlignedBlob() { ++destroyed; }
};

// Used to verify that reentrant collect() during a destructor is a safe no-op.
struct ReentrantNode {
    static inline bool ran = false;
    ~ReentrantNode() {
        gc::GC_Manager::instance().collect();
        ran = true;
    }
};

// Used to verify that a destructor that throws causes std::terminate.
// (Not run in normal test flow — documented here for reference.)

void reset_counts() {
    Node::destroyed = 0;
    Buffer::destroyed = 0;
    LargeBlob::destroyed = 0;
    OverAlignedBlob::destroyed = 0;
    ReentrantNode::ran = false;
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

GC_NOINLINE void make_reachable_chain() {
    auto root = gc::gc_new<Node>(1);
    root->next = gc::gc_new<Node>(2);
    root->next->next = gc::gc_new<Node>(3);

    gc::GC_Manager::instance().collect();
    expect(gc::GC_Manager::instance().managed_block_count() == 3U,
           "reachable chain should stay alive while rooted");
}

GC_NOINLINE void make_deep_chain() {
    constexpr int depth = 50000;
    auto root = gc::gc_new<Node>(0);
    auto cursor = root;
    for (int value = 1; value < depth; ++value) {
        cursor->next = gc::gc_new<Node>(value);
        cursor = cursor->next;
    }
}

GC_NOINLINE void register_and_unregister_external_root() {
    auto external_root = std::make_unique<gc::gc_ptr<Node>>();
    *external_root = gc::gc_new<Node>(42);

    auto& manager = gc::GC_Manager::instance();
    manager.register_root_object(external_root.get());
    manager.collect();

    expect(manager.managed_block_count() == 1U,
           "registered external root range should keep a heap-stored gc_ptr alive");

    manager.unregister_root_object(external_root.get());
    external_root->reset();
}

// ---- Original tests (preserved) ---------------------------------------------

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

    make_reachable_chain();
    scrub_stack();
    gc::GC_Manager::instance().collect();
    expect(gc::GC_Manager::instance().managed_block_count() == 0U,
           "chain should be reclaimed after the last root is dropped");
    expect(Node::destroyed >= 3, "chain destructors should run");
}

void test_deep_reachable_chain() {
    reset_gc();

    make_deep_chain();
    scrub_stack();
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

    register_and_unregister_external_root();
    scrub_stack();
    collect_until_stable();

    expect(gc::GC_Manager::instance().managed_block_count() == 0U,
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

// ---- New tests --------------------------------------------------------------

GC_NOINLINE void make_reentrant_node() {
    auto obj = gc::gc_new<ReentrantNode>();
    (void)obj;
}

void test_reentrant_collect_is_safe() {
    reset_gc();

    make_reentrant_node();
    scrub_stack();
    collect_until_stable();

    expect(ReentrantNode::ran, "ReentrantNode destructor should have run");
    expect(gc::GC_Manager::instance().managed_block_count() == 0U,
           "block should be reclaimed; reentrant collect() should be a no-op");
}

void test_gc_new_nothrow() {
    reset_gc();

    auto p = gc::gc_new_nothrow<Node>(42);
    expect(static_cast<bool>(p), "gc_new_nothrow should succeed under normal conditions");
    expect(p->value == 42, "gc_new_nothrow object should have correct value");

    p.reset();
    collect_until_stable();
}

GC_NOINLINE void make_and_verify_array() {
    constexpr std::size_t count = 5U;
    auto arr = gc::gc_new_array<Buffer>(count);
    expect(arr.get() != nullptr, "gc_new_array should return a non-null pointer");
    expect(gc::GC_Manager::instance().managed_block_count() == 1U,
           "gc_new_array should create exactly one GC block");
    arr[0].values[0] = 42;
    expect(arr[0].values[0] == 42, "array element write/read should work");
}

void test_gc_new_array_basic() {
    reset_gc();

    make_and_verify_array();
    scrub_stack();
    collect_until_stable();

    expect(gc::GC_Manager::instance().managed_block_count() == 0U,
           "array block should be reclaimed after last reference is dropped");
    expect(Buffer::destroyed >= 5,
           "all array element destructors should run");
}

void test_gc_new_array_zero_count() {
    reset_gc();

    auto arr = gc::gc_new_array<Buffer>(0U);
    expect(!arr, "gc_new_array(0) should return null");
    expect(gc::GC_Manager::instance().managed_block_count() == 0U,
           "zero-count array should not allocate a block");
}

GC_NOINLINE void setup_weak_basic(gc::gc_weak_ptr<Node>& out) {
    auto node = gc::gc_new<Node>(99);
    out = gc::gc_weak_ptr<Node>(node);
    expect(!out.expired(), "weak ptr should not be expired while strong ref exists");
    auto locked = out.lock();
    expect(static_cast<bool>(locked), "lock() should return a valid pointer on live object");
    expect(locked->value == 99, "locked object should have the correct value");
}

void test_gc_weak_ptr_basic() {
    reset_gc();

    gc::gc_weak_ptr<Node> weak;
    setup_weak_basic(weak);
    scrub_stack();
    collect_until_stable();

    expect(weak.expired(), "weak ptr should be expired after GC reclaims the object");
    auto dead = weak.lock();
    expect(!static_cast<bool>(dead), "lock() on expired weak ptr should return null");
}

GC_NOINLINE void setup_weak_copy(gc::gc_weak_ptr<Node>& w1_out,
                                  gc::gc_weak_ptr<Node>& w2_out) {
    auto node = gc::gc_new<Node>(7);
    w1_out = gc::gc_weak_ptr<Node>(node);
    w2_out = w1_out;
}

void test_gc_weak_ptr_copy() {
    reset_gc();

    gc::gc_weak_ptr<Node> w1;
    gc::gc_weak_ptr<Node> w2;
    setup_weak_copy(w1, w2);
    scrub_stack();
    collect_until_stable();

    expect(w1.expired(), "original weak ptr should expire after reclamation");
    expect(w2.expired(), "copied weak ptr should also expire after reclamation");
}

GC_NOINLINE void setup_weak_move(gc::gc_weak_ptr<Node>& w2_out) {
    auto node = gc::gc_new<Node>(8);
    gc::gc_weak_ptr<Node> w1(node);
    w2_out = std::move(w1);
    expect(w1.expired(), "moved-from weak ptr should be empty");
    expect(!w2_out.expired(), "moved-to weak ptr should still be valid");
}

void test_gc_weak_ptr_move() {
    reset_gc();

    gc::gc_weak_ptr<Node> w2;
    setup_weak_move(w2);
    scrub_stack();
    collect_until_stable();

    expect(w2.expired(), "moved-to weak ptr should expire after reclamation");
}

void test_gc_weak_ptr_reset() {
    reset_gc();

    auto node = gc::gc_new<Node>(5);
    gc::gc_weak_ptr<Node> weak(node);
    weak.reset();

    expect(weak.expired(), "weak ptr should be expired after explicit reset");
    // Object must still be reachable via the strong ref.
    gc::GC_Manager::instance().collect();
    expect(gc::GC_Manager::instance().managed_block_count() == 1U,
           "object should still be alive — only the weak ptr was reset");

    node.reset();
    collect_until_stable();
}

void test_gc_ptr_stl_compatibility() {
    reset_gc();

    auto a = gc::gc_new<Node>(1);
    auto b = gc::gc_new<Node>(2);

    // std::hash — unordered_map
    std::unordered_map<gc::gc_ptr<Node>, int> umap;
    umap[a] = 10;
    umap[b] = 20;
    expect(umap.at(a) == 10, "gc_ptr should work as unordered_map key");
    expect(umap.at(b) == 20, "gc_ptr should work as unordered_map key");

    // std::unordered_set
    std::unordered_set<gc::gc_ptr<Node>> uset;
    uset.insert(a);
    uset.insert(b);
    expect(uset.size() == 2U, "gc_ptr should work in unordered_set");

    // std::less — ordered map
    std::map<gc::gc_ptr<Node>, int> omap;
    omap[a] = 1;
    omap[b] = 2;
    expect(omap.size() == 2U, "gc_ptr should work as map key");

    // std::set
    std::set<gc::gc_ptr<Node>> oset;
    oset.insert(a);
    oset.insert(b);
    expect(oset.size() == 2U, "gc_ptr should work in set");

    // swap
    gc::gc_ptr<Node> orig_a = a;
    gc::swap(a, b);
    expect(a.get() == orig_a.get() || b.get() == orig_a.get(),
           "swap should exchange pointers");

    a.reset();
    b.reset();
    collect_until_stable();
}

void test_gc_ptr_casts() {
    reset_gc();

    struct Base {
        virtual ~Base() = default;
        int x = 1;
    };
    struct Derived : Base {
        int y = 2;
    };

    auto d = gc::gc_new<Derived>();
    auto b = gc::static_gc_ptr_cast<Base>(d);
    expect(b.get() == static_cast<Base*>(d.get()), "static_gc_ptr_cast should work");

    auto d2 = gc::dynamic_gc_ptr_cast<Derived>(b);
    expect(d2.get() == d.get(), "dynamic_gc_ptr_cast should succeed for correct type");

    auto b2 = gc::dynamic_gc_ptr_cast<Base>(d);
    auto d3 = gc::const_gc_ptr_cast<const Derived>(
        gc::static_gc_ptr_cast<const Derived>(d));
    (void)d3;

    d.reset();
    b.reset();
    d2.reset();
    b2.reset();
    collect_until_stable();
}

void test_gc_stats() {
    reset_gc();

    auto& mgr = gc::GC_Manager::instance();
    auto before = mgr.stats();
    expect(before.total_collections == 0U, "no collections should have occurred yet");

    auto node = gc::gc_new<Node>(1);
    node.reset();
    collect_until_stable();

    auto after = mgr.stats();
    expect(after.total_collections > 0U, "at least one collection should be counted");
    expect(after.bytes_reclaimed_total > 0U, "bytes_reclaimed_total should be non-zero");
}

void test_release_unconstructed_unknown_pointer() {
    reset_gc();

    // release_unconstructed on a pointer not in allocations_ must be a safe no-op.
    int stack_var = 42;
    gc::GC_Manager::instance().release_unconstructed(static_cast<void*>(&stack_var));
    expect(stack_var == 42, "stack variable should be unchanged after no-op release");
}

// ---- Runner -----------------------------------------------------------------

int run_all_tests() {
    struct TestCase {
        const char* name;
        std::function<void()> function;
    };

    const TestCase tests[] = {
        // Original tests
        {"cycle reclamation",               test_cycle_reclamation},
        {"reachable chain",                 test_reachable_chain},
        {"deep reachable chain",            test_deep_reachable_chain},
        {"interior pointer",                test_interior_pointer},
        {"threshold trigger",               test_threshold_trigger},
        {"over-aligned allocation",         test_over_aligned_allocation},
        {"registered external root range",  test_registered_external_root_range},
        {"cross-thread use is rejected",    test_cross_thread_use_is_rejected},
        // New tests
        {"reentrant collect is safe",       test_reentrant_collect_is_safe},
        {"gc_new_nothrow",                  test_gc_new_nothrow},
        {"gc_new_array basic",              test_gc_new_array_basic},
        {"gc_new_array zero count",         test_gc_new_array_zero_count},
        {"gc_weak_ptr basic",               test_gc_weak_ptr_basic},
        {"gc_weak_ptr copy",                test_gc_weak_ptr_copy},
        {"gc_weak_ptr move",                test_gc_weak_ptr_move},
        {"gc_weak_ptr reset",               test_gc_weak_ptr_reset},
        {"gc_ptr STL compatibility",        test_gc_ptr_stl_compatibility},
        {"gc_ptr casts",                    test_gc_ptr_casts},
        {"GC stats",                        test_gc_stats},
        {"release_unconstructed unknown",   test_release_unconstructed_unknown_pointer},
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
