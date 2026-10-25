#include "gc/GC.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

[[noreturn]] void immediate_exit(int code) noexcept {
#if defined(_WIN32)
    // The collector may be a DLL with a live singleton. CRT process exit can
    // run DLL detach and finalize that singleton again after this deliberate
    // termination path. TerminateProcess skips detach and other exit callbacks.
    ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(code));
    std::abort();  // Terminating the current process must not return.
#else
    std::_Exit(code);
#endif
}

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

bool cleanup_termination_test = false;

void cleanup_trace(const char* stage) noexcept {
    std::fputs("cleanup contract: ", stderr);
    std::fputs(stage, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

struct ThrowingFinalizer {
    ~ThrowingFinalizer() noexcept(false) { throw FinalizerException{}; }
};

struct ThrowingCleanupElement {
    static inline std::size_t attempts = 0;
    // Retain a real return path in the destructor when compiling with MSVC;
    // the contract sets this runtime flag before entering partial cleanup.
    static inline volatile bool throw_from_destructor = false;
    std::size_t index;

    ThrowingCleanupElement() : index(attempts++) {
        if (index == 1) {
            cleanup_trace("element one constructor throws");
            throw ConstructionException{};
        }
        cleanup_trace("element zero constructor completed");
    }
    ThrowingCleanupElement(const ThrowingCleanupElement&) = delete;
    ThrowingCleanupElement(ThrowingCleanupElement&&) = delete;

    ~ThrowingCleanupElement() noexcept(false) {
        cleanup_trace("completed element destructor entered");
        // Verify this is cleanup of element zero after element one failed,
        // rather than termination from destroying an unexpected temporary.
        if (attempts != 2 || index != 0) immediate_exit(78);
        if (throw_from_destructor) {
            cleanup_trace("completed element destructor throws");
            throw FinalizerException{};
        }
        cleanup_trace("completed element destructor returned");
    }
};

[[noreturn]] void finalizer_terminate_handler() noexcept {
    // A distinctive code proves terminate ran with the expected destructor
    // exception; a generic crash or some other terminate path does not pass.
    if (cleanup_termination_test) cleanup_trace("terminate handler entered");
    const auto error = std::current_exception();
    if (cleanup_termination_test) cleanup_trace("current_exception returned");
    if (!error) immediate_exit(74);
    try {
        if (cleanup_termination_test) cleanup_trace("rethrowing current exception");
        std::rethrow_exception(error);
    } catch (const FinalizerException&) {
        if (cleanup_termination_test) cleanup_trace("expected finalizer exception caught");
        immediate_exit(73);
    } catch (...) {
        if (cleanup_termination_test) cleanup_trace("unexpected exception caught");
        immediate_exit(75);
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
        immediate_exit(76);
    }
    immediate_exit(77);
}

[[noreturn]] void test_throwing_cleanup_finalizer() {
    gc::ScopedThreadRegistration registration;
    cleanup_termination_test = true;
    std::set_terminate(finalizer_terminate_handler);
    ThrowingCleanupElement::attempts = 0;
    ThrowingCleanupElement::throw_from_destructor = true;
    cleanup_trace("array construction starting");
    try {
        // Exercise the shared partial-array cleanup directly. Using a
        // noncopyable, nonmovable element avoids generator return temporaries
        // and tests precisely the destructor of the completed array element.
        (void)gc::gc_new_array<ThrowingCleanupElement>(2);
    } catch (...) {
        // Neither the original constructor exception nor the cleanup destructor
        // exception may escape after a finalizer has thrown.
        cleanup_trace("an exception escaped partial-array cleanup");
        immediate_exit(76);
    }
    immediate_exit(77);
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
