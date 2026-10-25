#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

#include "gc/GC.hpp"

namespace allocation_failure {

// Fail one ordinary allocation on this thread, then disable the injection.
// Managed payloads use aligned new, which deliberately remains unaffected.
thread_local std::ptrdiff_t remaining = -1;

void arm(std::ptrdiff_t allocations_before_failure) noexcept {
    remaining = allocations_before_failure;
}

void disarm() noexcept {
    remaining = -1;
}

void* allocate(std::size_t size) {
    if (remaining == 0) {
        disarm();
        throw std::bad_alloc();
    }
    if (remaining > 0) --remaining;
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
    throw std::bad_alloc();
}

}  // namespace allocation_failure

void* operator new(std::size_t size) {
    return allocation_failure::allocate(size);
}

void* operator new[](std::size_t size) {
    return allocation_failure::allocate(size);
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Node {
    static inline int constructed = 0;
    static inline int destroyed = 0;
    gc::gc_ptr<Node> next;

    Node() { ++constructed; }
    ~Node() { ++destroyed; }
};

void test_failed_allocation_registration() {
    auto& manager = gc::GC_Manager::instance();
    gc::shutdown();
    Node::constructed = 0;
    Node::destroyed = 0;

    // Allocating the aligned payload succeeds; recording it in the registry
    // fails. ASan/LeakSanitizer also verifies that these payloads are released.
    for (int attempt = 0; attempt < 32; ++attempt) {
        bool failed = false;
        allocation_failure::arm(0);
        try {
            (void)gc::gc_new<Node>();
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        allocation_failure::disarm();
        expect(failed, "a registry allocation failure must propagate std::bad_alloc");
        expect(manager.managed_block_count() == 0 && manager.managed_bytes() == 0,
               "failed allocation registration must not change heap accounting");
        expect(manager.stats().reserved_bytes == 0,
               "failed allocation registration must not reserve a managed block");
    }
    expect(Node::constructed == 0 && Node::destroyed == 0,
           "failed allocation registration must not construct or finalize an object");

    (void)gc::gc_new<Node>();
    gc::shutdown();
    expect(Node::constructed == 1 && Node::destroyed == 1,
           "allocation and shutdown must still work after registry failures");
}

void test_shutdown_is_transactional_on_allocation_failure() {
    auto& manager = gc::GC_Manager::instance();
    constexpr std::size_t block_count = 6;
    bool reached_success = false;
    bool saw_failure = false;

    // Advance the single failure through shutdown's temporary graph storage.
    // Stop at the first successful shutdown rather than depending on a fixed
    // number of internal containers or allocation calls.
    for (std::ptrdiff_t offset = 0; offset < 128 && !reached_success; ++offset) {
        gc::shutdown();
        Node::constructed = 0;
        Node::destroyed = 0;
        std::array<gc::gc_ptr<Node>, block_count> nodes;
        for (auto& node : nodes) node = gc::gc_new<Node>();
        for (std::size_t index = 0; index + 1 < block_count; ++index) {
            nodes[index]->next = nodes[index + 1];
        }
        const auto before = manager.stats();

        bool failed = false;
        allocation_failure::arm(offset);
        try {
            gc::shutdown();
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        allocation_failure::disarm();

        if (failed) {
            saw_failure = true;
            const auto after = manager.stats();
            expect(after.live_blocks == before.live_blocks &&
                       after.live_bytes == before.live_bytes &&
                       after.reserved_bytes == before.reserved_bytes,
                   "failed shutdown must preserve every allocation and its accounting");
            expect(Node::destroyed == 0,
                   "failed shutdown must not partially finalize the object graph");
            gc::shutdown();
        } else {
            reached_success = true;
        }
        expect(Node::destroyed == static_cast<int>(block_count),
               "retrying shutdown must finalize every object exactly once");
        expect(manager.managed_block_count() == 0 && manager.managed_bytes() == 0 &&
                   manager.stats().reserved_bytes == 0,
               "successful shutdown must release the complete object graph");
    }
    expect(saw_failure, "the fault injection must exercise a failing shutdown");
    expect(reached_success, "shutdown must succeed after all allocation sites are traversed");
}

}  // namespace

int main() {
    try {
        gc::ScopedThreadRegistration registration;
        test_failed_allocation_registration();
        test_shutdown_is_transactional_on_allocation_failure();
        std::cout << "Allocation failure tests passed\n";
    } catch (const std::exception& error) {
        allocation_failure::disarm();
        std::cerr << "Allocation failure regression: " << error.what() << '\n';
        return 1;
    }
}
