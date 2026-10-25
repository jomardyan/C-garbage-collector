#include "gc/GC.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Node {
    gc::gc_ptr<Node> child;
    std::size_t value = 0;
};

void test_object_lookup() {
    auto& manager = gc::GC_Manager::instance();
    gc::shutdown();
    expect(!manager.object_info(nullptr), "null must not resolve to an allocation");
    int unmanaged = 0;
    expect(!manager.object_info(&unmanaged), "native storage must not resolve to an allocation");

    gc::gc_root<Node> parent(gc::gc_new<Node>());
    gc::gc_root<Node> child(gc::gc_new<Node>());
    parent->child = child.as_gc_ptr();
    const auto info = manager.object_info(parent.get());
    expect(info && info->payload == parent.get() && info->payload_size == sizeof(Node),
           "base lookup must report the owning payload and exact payload size");
    expect(info->reserved_size == info->payload_offset + sizeof(Node),
           "reserved size must include the hidden header and alignment padding");
    expect(info->allocation_alignment >= alignof(Node), "reported alignment must support the type");
    expect(info->outgoing_references == std::vector<void*>{child.get()},
           "object lookup must report the same outgoing graph as a heap snapshot");
    const auto* begin = reinterpret_cast<const unsigned char*>(parent.get());
    const auto interior = manager.object_info(begin + 1);
    expect(interior && interior->payload == parent.get(),
           "odd interior addresses must resolve to the base allocation");
    const auto final_byte = manager.object_info(begin + sizeof(Node) - 1);
    expect(final_byte && final_byte->payload == parent.get(),
           "the final payload byte must belong to its allocation");
    expect(!manager.object_info(begin + sizeof(Node)),
           "a one-past payload pointer must not belong to its allocation");
    expect(!manager.object_info(reinterpret_cast<const void*>(
               reinterpret_cast<std::uintptr_t>(parent.get()) - 1U)),
           "hidden header and alignment padding must not count as payload");

    const auto objects = manager.live_objects();
    const auto match = std::find_if(objects.begin(), objects.end(), [&](const auto& object) {
        return object.payload == info->payload;
    });
    expect(match != objects.end() && match->outgoing_references == info->outgoing_references,
           "targeted inspection must agree with the whole-heap graph");
    gc::shutdown();
    expect(!manager.object_info(begin), "reclaimed storage must no longer resolve");
    parent.reset();
    child.reset();
}

struct alignas(128) AlignedElement {
    unsigned char value = 7;
};

void test_array_and_alignment_lookup() {
    auto& manager = gc::GC_Manager::instance();
    gc::shutdown();
    gc::gc_root<AlignedElement[]> array(gc::gc_new_array<AlignedElement>(3));
    const auto first = manager.object_info(array.get());
    const auto last = manager.object_info(&array[2]);
    expect(first && last && first->payload == last->payload,
           "array elements must resolve through the allocation prefix");
    expect(first->allocation_alignment == alignof(AlignedElement),
           "targeted diagnostics must retain over-alignment metadata");
    const auto prefix_bytes = reinterpret_cast<std::uintptr_t>(array.get()) -
                              reinterpret_cast<std::uintptr_t>(first->payload);
    expect(prefix_bytes > 0 && first->payload_size == prefix_bytes + 3U * sizeof(AlignedElement),
           "array payload size must include its element-count prefix");
    expect(!manager.object_info(array.get() + 3), "one-past the full array must be rejected");
    gc::shutdown();
    array.reset();
}

struct Throwing {
    Throwing() { throw std::runtime_error("expected construction failure"); }
};

template <typename Action>
void on_registered_worker(Action action) {
    gc::unregister_current_thread();
    std::exception_ptr error;
    try {
        std::thread worker([&] {
            try {
                gc::ScopedThreadRegistration registration;
                action();
            } catch (...) {
                error = std::current_exception();
            }
        });
        worker.join();
    } catch (...) {
        gc::register_current_thread();
        throw;
    }
    gc::register_current_thread();
    if (error) std::rethrow_exception(error);
}

void test_allocation_counters_and_peaks() {
    auto& manager = gc::GC_Manager::instance();
    gc::shutdown();
    manager.set_collection_threshold_bytes(10U * 1024U * 1024U);
    constexpr auto expected_live = sizeof(int) + sizeof(std::array<char, 33>);
    // Keep managed addresses off the collecting stack. The worker destroys its
    // exact roots and unregisters before main collects; main has never held
    // these payload pointers or inspected their object graph.
    on_registered_worker([] {
        auto& manager = gc::GC_Manager::instance();
        gc::gc_root<int> number(gc::gc_new<int>(4));
        gc::gc_root<std::array<char, 33>> buffer(gc::gc_new<std::array<char, 33>>());
        auto snapshot = manager.stats();
        expect(snapshot.total_allocations == 2 && snapshot.bytes_allocated_total == expected_live,
               "allocation counters must count exact registered payload sizes");
        expect(snapshot.peak_live_bytes == expected_live && snapshot.peak_live_blocks == 2,
               "high-water values must track simultaneous payloads and block count");
        expect(!gc::gc_new_nothrow<Throwing>(), "throwing construction must return null through nothrow");
        snapshot = manager.stats();
        expect(snapshot.total_allocations == 3 &&
                   snapshot.bytes_allocated_total == expected_live + sizeof(Throwing),
               "a registered block must count even if construction later fails");
        expect(snapshot.live_bytes == expected_live && snapshot.live_blocks == 2,
               "failed construction must release live accounting");
        expect(snapshot.peak_live_bytes == expected_live + sizeof(Throwing) &&
                   snapshot.peak_live_blocks == 3,
               "construction storage must contribute to high-water values");
    });
    gc::collect();
    auto snapshot = manager.stats();
    expect(snapshot.live_blocks == 0 && snapshot.live_bytes == 0,
           "collection must reclaim unrooted payloads");
    expect(snapshot.total_allocations == 3 && snapshot.peak_live_blocks == 3 &&
               snapshot.bytes_allocated_total == expected_live + sizeof(Throwing),
           "collection must preserve cumulative counters and peaks");
    gc::shutdown();
    snapshot = manager.stats();
    expect(snapshot.total_allocations == 0 && snapshot.bytes_allocated_total == 0 &&
               snapshot.peak_live_bytes == 0 && snapshot.peak_live_blocks == 0,
           "shutdown must reset allocation history and peaks");
    auto next = gc::gc_new<int>(9);
    expect(manager.stats().total_allocations == 1 && manager.stats().peak_live_blocks == 1,
           "the next allocation must begin a new diagnostics history");
    next.reset();
    gc::shutdown();
}

void test_registration_counts() {
    auto& manager = gc::GC_Manager::instance();
    gc::shutdown();
    const auto baseline = manager.stats();
    expect(baseline.registered_threads == 1 && baseline.registered_root_ranges == 0 &&
               baseline.registered_weak_refs == 0,
           "the empty collector must report only the test thread");
    {
        gc::ScopedThreadRegistration nested;
        gc::gc_root<Node> node(gc::gc_new<Node>());
        gc::ScopedRootObject<gc::gc_root<Node>> duplicate(&node);
        gc::gc_weak_ptr<Node> weak(node.as_gc_ptr());
        gc::gc_weak_ptr<Node> copy(weak);
        auto moved = std::move(copy);
        auto snapshot = manager.stats();
        expect(snapshot.registered_threads == 1 && snapshot.registered_root_ranges == 2 &&
                   snapshot.registered_weak_refs == 2,
               "nested thread registration and weak moves must not create phantom entries");
        moved.reset();
        expect(manager.stats().registered_weak_refs == 1,
               "resetting a weak handle must remove its diagnostics entry");
        gc::shutdown();
        snapshot = manager.stats();
        expect(snapshot.registered_root_ranges == 2 && snapshot.registered_weak_refs == 0 &&
                   snapshot.registered_threads == 1,
               "shutdown must preserve explicit registrations and invalidate weak ones");
        node.reset();
    }
    expect(manager.stats().registered_root_ranges == 0 && manager.stats().registered_weak_refs == 0,
           "scope exit must remove every explicit diagnostics registration");
}

void test_concurrent_targeted_inspection() {
    auto& manager = gc::GC_Manager::instance();
    gc::shutdown();
    gc::gc_root<Node> parent(gc::gc_new<Node>());
    parent->child = gc::gc_new<Node>();
    std::atomic<bool> complete{false};
    std::exception_ptr error;
    std::thread worker([&] {
        try {
            gc::ScopedThreadRegistration registration;
            for (std::size_t iteration = 0; iteration < 40; ++iteration) {
                parent->value = iteration;
                if (iteration % 4U == 0U) gc::collect();
                else gc::safepoint();
            }
        } catch (...) {
            error = std::current_exception();
        }
        complete.store(true, std::memory_order_release);
    });
    bool valid = true;
    while (!complete.load(std::memory_order_acquire)) {
        const auto object = manager.object_info(parent.get());
        valid = valid && object && object->payload == parent.get() &&
                object->outgoing_references.size() == 1;
        gc::safepoint();
    }
    worker.join();
    if (error) std::rethrow_exception(error);
    expect(valid && parent->value == 39,
           "targeted snapshots must safely inspect a mutating registered worker's graph");
    parent.reset();
    gc::shutdown();
}

}  // namespace

int main() {
    try {
        gc::ScopedThreadRegistration registration;
        // Run exact reclamation accounting before this stack has seen any
        // managed addresses which a conservative scan could later retain.
        test_allocation_counters_and_peaks();
        test_object_lookup();
        test_array_and_alignment_lookup();
        test_registration_counts();
        test_concurrent_targeted_inspection();
        gc::shutdown();
        std::cout << "[PASS] heap diagnostics and allocation statistics\n";
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
