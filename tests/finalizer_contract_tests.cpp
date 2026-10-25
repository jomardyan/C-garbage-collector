#include "gc/GC.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// The caller never receives a payload address. After join, none of the
// allocating thread's conservative stack/register roots remain registered.
template <typename Action>
void allocate_on_worker(Action action) {
    std::exception_ptr error;
    std::thread worker([&] {
        try {
            gc::ScopedThreadRegistration registration;
            action();
        } catch (...) {
            error = std::current_exception();
        }
    });
    worker.join();
    if (error) std::rethrow_exception(error);
}

struct DependencyNode {
    static constexpr std::size_t count = 5;
    static inline std::array<bool, count> destroyed{};
    static inline std::array<std::size_t, count> finalization_order{};
    static inline std::size_t finalizations = 0;
    static inline bool invalid_order = false;

    std::array<gc::gc_ptr<DependencyNode>, 2> children{};
    std::array<std::size_t, 2> child_ids{count, count};
    std::size_t id;
    int value = 123;

    explicit DependencyNode(std::size_t identifier) : id(identifier) {}

    ~DependencyNode() {
        for (std::size_t index = 0; index < children.size(); ++index) {
            const auto child_id = child_ids[index];
            if (child_id == count) continue;
            // Consult independent metadata first, so an ordering failure never
            // makes the test dereference an object whose lifetime has ended.
            if (destroyed[child_id] || !children[index]) {
                invalid_order = true;
                continue;
            }
            if (children[index]->value != 123 || children[index]->id != child_id) {
                invalid_order = true;
            }
        }
        if (destroyed[id] || finalizations >= count) {
            invalid_order = true;
            return;
        }
        destroyed[id] = true;
        finalization_order[finalizations++] = id;
    }

    static void reset() {
        destroyed.fill(false);
        finalization_order.fill(count);
        finalizations = 0;
        invalid_order = false;
    }
};

void make_dependency_dag() {
    std::array<gc::gc_root<DependencyNode>, DependencyNode::count> nodes;
    // Deliberately mix allocation order with graph order. The diamond has a
    // shared leaf, and the top also has a direct edge to that leaf:
    // 4 -> 0 -> {1, 2} -> 3, and 4 -> 3.
    for (const auto id : std::array<std::size_t, 5>{3, 1, 4, 2, 0}) {
        nodes[id] = gc::gc_new<DependencyNode>(id);
    }
    const auto connect = [&](std::size_t parent, std::size_t slot, std::size_t child) {
        nodes[parent]->children[slot] = nodes[child].as_gc_ptr();
        nodes[parent]->child_ids[slot] = child;
    };
    connect(4, 0, 0);
    connect(4, 1, 3);
    connect(0, 0, 1);
    connect(0, 1, 2);
    connect(1, 0, 3);
    connect(2, 0, 3);
}

void verify_dependency_order() {
    expect(DependencyNode::finalizations == DependencyNode::count,
           "every DAG node must finalize exactly once");
    expect(!DependencyNode::invalid_order,
           "a DAG finalizer must see all of its child objects still alive");
    std::array<std::size_t, DependencyNode::count> position{};
    for (std::size_t index = 0; index < DependencyNode::count; ++index) {
        position[DependencyNode::finalization_order[index]] = index;
    }
    expect(position[4] < position[0] && position[0] < position[1] &&
               position[0] < position[2] && position[1] < position[3] &&
               position[2] < position[3],
           "dependency edges must precede their targets in finalization order");
    expect(gc::GC_Manager::instance().managed_block_count() == 0,
           "finalization must release all DAG blocks");
}

void test_dependency_order_during_collection() {
    gc::shutdown();
    DependencyNode::reset();
    allocate_on_worker(make_dependency_dag);
    gc::ScopedThreadRegistration registration;
    gc::collect();
    verify_dependency_order();
}

void test_dependency_order_during_unregistered_shutdown() {
    gc::shutdown();
    DependencyNode::reset();
    allocate_on_worker(make_dependency_dag);
    expect(!gc::GC_Manager::instance().is_current_thread_registered(),
           "this shutdown scenario must execute from an unregistered thread");
    gc::shutdown();
    verify_dependency_order();
}

struct ReentrantFinalizer {
    static inline int entered = 0;
    static inline int completed = 0;
    static inline bool failed = false;

    ~ReentrantFinalizer() {
        ++entered;
        try {
            gc::collect();
            gc::shutdown();
        } catch (...) {
            failed = true;
        }
        ++completed;
    }

    static void reset() {
        entered = 0;
        completed = 0;
        failed = false;
    }
};

void make_reentrant_object() {
    gc::gc_root<ReentrantFinalizer> object(gc::gc_new<ReentrantFinalizer>());
}

void verify_reentrant_finalizer() {
    expect(ReentrantFinalizer::entered == 1 && ReentrantFinalizer::completed == 1,
           "recursive collect/shutdown must return without repeating finalization");
    expect(!ReentrantFinalizer::failed,
           "recursive collect/shutdown must not throw from a finalizer");
    expect(gc::GC_Manager::instance().managed_block_count() == 0,
           "reentrant finalization must leave no detached blocks in the heap");
}

void test_reentrant_collect_and_shutdown() {
    gc::shutdown();
    ReentrantFinalizer::reset();
    allocate_on_worker(make_reentrant_object);
    {
        gc::ScopedThreadRegistration registration;
        gc::collect();
        verify_reentrant_finalizer();
        expect(gc::GC_Manager::instance().stats().total_collections == 1,
               "a recursive collection must not start an additional sweep");
    }

    ReentrantFinalizer::reset();
    allocate_on_worker(make_reentrant_object);
    expect(!gc::GC_Manager::instance().is_current_thread_registered(),
           "explicit shutdown must also support an unregistered caller");
    gc::shutdown();
    verify_reentrant_finalizer();
    expect(!gc::GC_Manager::instance().is_current_thread_registered(),
           "shutdown must preserve the caller's unregistered state");

    // Exercise another collection to ensure recursion did not leave the
    // collector's finalization or stop-the-world state set.
    gc::ScopedThreadRegistration registration;
    gc::gc_root<int> next(gc::gc_new<int>(64));
    gc::collect();
    expect(next && *next == 64, "the collector must remain usable after reentrant finalization");
    gc::shutdown();
}

struct FinalizerException {};
struct ConstructionException {};

struct ThrowingFinalizer {
    ~ThrowingFinalizer() noexcept(false) { throw FinalizerException{}; }
};

[[noreturn]] void finalizer_terminate_handler() noexcept {
    // A distinctive code proves terminate ran with the expected destructor
    // exception; a generic crash or some other terminate path does not pass.
    const auto error = std::current_exception();
    if (!error) std::_Exit(74);
    try {
        std::rethrow_exception(error);
    } catch (const FinalizerException&) {
        std::_Exit(73);
    } catch (...) {
        std::_Exit(75);
    }
}

[[noreturn]] void test_throwing_finalizer(bool during_collection) {
    allocate_on_worker([] {
        gc::gc_root<ThrowingFinalizer> object(gc::gc_new<ThrowingFinalizer>());
    });
    gc::ScopedThreadRegistration registration;
    std::set_terminate(finalizer_terminate_handler);
    try {
        if (during_collection) {
            gc::collect();
        } else {
            gc::shutdown();
        }
    } catch (...) {
        // Do not let singleton teardown produce a false pass after a failure
        // in the collection/shutdown operation under test.
        std::_Exit(76);
    }
    std::_Exit(77);
}

[[noreturn]] void test_throwing_cleanup_finalizer() {
    gc::ScopedThreadRegistration registration;
    std::set_terminate(finalizer_terminate_handler);
    try {
        (void)gc::gc_generate_array<ThrowingFinalizer>(2, [](std::size_t index) {
            if (index == 1) throw ConstructionException{};
            return ThrowingFinalizer{};
        });
    } catch (...) {
        // Neither the original generator exception nor the cleanup destructor
        // exception may escape after a finalizer has thrown.
        std::_Exit(76);
    }
    std::_Exit(77);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--throwing") {
            test_throwing_finalizer(false);
        }
        if (argc == 2 && std::string_view(argv[1]) == "--throwing-collect") {
            test_throwing_finalizer(true);
        }
        if (argc == 2 && std::string_view(argv[1]) == "--throwing-cleanup") {
            test_throwing_cleanup_finalizer();
        }
        if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--normal")) {
            std::cerr << "usage: gc_finalizer_contract_tests [--normal|--throwing|--throwing-collect|--throwing-cleanup]\n";
            return 2;
        }
        test_dependency_order_during_collection();
        test_dependency_order_during_unregistered_shutdown();
        test_reentrant_collect_and_shutdown();
        gc::shutdown();
        std::cout << "Finalizer contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Finalizer contract failure: " << error.what() << '\n';
        return 1;
    }
}
