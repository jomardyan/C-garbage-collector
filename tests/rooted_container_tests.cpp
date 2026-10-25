#include "gc/GC.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Item {
    static inline int destroyed = 0;
    explicit Item(int v) : value(v) {}
    int value;
    ~Item() { ++destroyed; }
};

// The main thread holds no mutator registration: every collection excludes its
// stale stack values. Only the heap-hosted container's exact roots can retain
// the test objects.
template <typename Action>
void on_worker(Action action) {
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

void collect() { on_worker([] { gc::collect(); }); }

void test_growth_and_reclamation() {
    gc::shutdown();
    Item::destroyed = 0;
    auto items = std::make_unique<gc::gc_vector<Item>>();
    on_worker([&] {
        for (int index = 0; index < 100; ++index) {
            items->emplace_new(index);
            if (index % 7 == 0) gc::collect();
        }
        items->reserve(256);
        items->shrink_to_fit();
    });
    collect();
    expect(items->size() == 100 && Item::destroyed == 0,
           "heap-hosted vector must root every target across reallocation and collection");
    on_worker([&] {
        int value = 0;
        for (const auto& item : *items) expect(item->value == value++, "iteration must preserve order");
        items->erase(items->begin(), items->begin() + 50);
        expect(items->front()->value == 50 && items->back()->value == 99,
               "range erase must retain the remaining roots");
    });
    collect();
    expect(Item::destroyed == 50, "erased roots must stop retaining their targets");
    on_worker([&] { items->clear(); });
    collect();
    expect(Item::destroyed == 100, "clearing a rooted vector must allow every target to be reclaimed");
    expect(gc::GC_Manager::instance().managed_block_count() == 0, "cleared vector must leave no managed blocks");
}

void test_copy_move_and_assignment() {
    gc::shutdown();
    Item::destroyed = 0;
    auto original = std::make_unique<gc::gc_vector<Item>>();
    auto copy = std::make_unique<gc::gc_vector<Item>>();
    auto moved = std::make_unique<gc::gc_vector<Item>>();
    on_worker([&] {
        original->emplace_new(4);
        original->emplace_new(8);
        *copy = *original;
        original->clear();
        *moved = std::move(*copy);
        expect(moved->at(0)->value == 4 && moved->at(1)->value == 8,
               "copy and move must preserve target identity and order");
    });
    collect();
    expect(Item::destroyed == 0, "moved roots must remain registered at their transferred storage");
    on_worker([&] {
        gc::gc_vector<Item> constructed(*moved);
        gc::gc_vector<Item> moved_constructed(std::move(constructed));
        original->swap(moved_constructed);
        moved->clear();
    });
    collect();
    expect(Item::destroyed == 0, "copy/move constructors and swap must preserve independent roots");
    on_worker([&] { original->pop_back(); });
    collect();
    expect(Item::destroyed == 1, "pop_back must unregister exactly one root");
    original.reset();
    collect();
    expect(Item::destroyed == 2, "container destruction must release all remaining roots");
}

void test_null_bounds_and_arrays() {
    gc::shutdown();
    auto arrays = std::make_unique<gc::gc_vector<int[]>>();
    on_worker([&] {
        arrays->resize(2);
        expect(!(*arrays)[0] && !(*arrays)[1], "new slots must contain null handles");
        (*arrays)[0] = gc::gc_new_array<int>(3);
        (*arrays)[0][2] = 19;
        arrays->push_back(gc::gc_new_array<int>(2));
        (*arrays)[2][1] = 23;
        bool threw = false;
        try { (void)arrays->at(3); } catch (const std::out_of_range&) { threw = true; }
        expect(threw, "at must reject an index equal to size");
    });
    collect();
    on_worker([&] {
        const auto& view = *arrays;
        expect(view.at(0)[2] == 19 && view.back()[1] == 23, "array roots must preserve managed elements");
        expect(std::distance(view.cbegin(), view.cend()) == 3, "const iteration must cover every slot");
        arrays->erase(arrays->begin() + 1);
        expect(arrays->size() == 2, "single erase must remove a null slot");
        arrays->resize(0);
    });
    collect();
    expect(gc::GC_Manager::instance().managed_block_count() == 0, "array roots must release both allocations");
}

void test_initializer_list_owns_independent_roots() {
    gc::shutdown();
    Item::destroyed = 0;
    std::unique_ptr<gc::gc_vector<Item>> items;
    on_worker([&] {
        gc::gc_root<Item> first(gc::gc_new<Item>(5));
        gc::gc_root<Item> second(gc::gc_new<Item>(10));
        items = std::make_unique<gc::gc_vector<Item>>(
            std::initializer_list<gc::gc_ptr<Item>>{first.as_gc_ptr(), {}, second.as_gc_ptr()});
        first.reset();
        second.reset();
    });
    collect();
    expect(Item::destroyed == 0 && items->size() == 3,
           "initializer-list targets must stay rooted after source handles disappear");
    on_worker([&] {
        expect((*items)[0]->value == 5 && !(*items)[1] && (*items)[2]->value == 10,
               "initializer-list construction must preserve order and null entries");
    });
    items.reset();
    collect();
    expect(Item::destroyed == 2, "initializer-list root destruction must release both targets");
}

struct Throwing {
    Throwing() { throw std::runtime_error("expected constructor failure"); }
};

void test_failed_emplacement_and_reuse() {
    gc::shutdown();
    on_worker([] {
        gc::gc_vector<Throwing> values;
        bool threw = false;
        try { values.emplace_new(); } catch (const std::runtime_error&) { threw = true; }
        expect(threw && values.empty(), "failed object construction must not insert an element");
        expect(gc::GC_Manager::instance().managed_block_count() == 0, "failed emplacement must release the allocation");
        gc::gc_vector<Item> roots;
        roots.emplace_new(1);
        gc::shutdown();
        roots[0] = gc::gc_new<Item>(2);
        gc::collect();
        expect(roots[0]->value == 2, "container root slots must remain reusable after shutdown");
        roots.clear();
    });
    collect();
}

void test_concurrent_growth_and_collection() {
    gc::shutdown();
    Item::destroyed = 0;
    auto items = std::make_unique<gc::gc_vector<Item>>();
    std::atomic<bool> finished{false};
    std::exception_ptr writer_error;
    std::exception_ptr collector_error;
    std::thread writer([&] {
        try {
            gc::ScopedThreadRegistration registration;
            for (int index = 0; index < 128; ++index) {
                items->emplace_new(index);
                gc::safepoint();
                std::this_thread::yield();
            }
        } catch (...) { writer_error = std::current_exception(); }
        finished.store(true, std::memory_order_release);
    });
    std::thread collector([&] {
        try {
            gc::ScopedThreadRegistration registration;
            for (int iteration = 0; iteration < 32; ++iteration) {
                gc::collect();
                if (finished.load(std::memory_order_acquire)) break;
                std::this_thread::yield();
            }
            while (!finished.load(std::memory_order_acquire)) {
                gc::safepoint();
                std::this_thread::yield();
            }
        } catch (...) { collector_error = std::current_exception(); }
    });
    writer.join();
    collector.join();
    if (writer_error) std::rethrow_exception(writer_error);
    if (collector_error) std::rethrow_exception(collector_error);
    expect(items->size() == 128 && Item::destroyed == 0,
           "concurrent collection during growth must preserve every rooted target");
    items.reset();
    collect();
    expect(Item::destroyed == 128, "concurrent vector use must leave no leaked target roots");
}

}  // namespace

int main() {
    try {
        test_growth_and_reclamation();
        test_copy_move_and_assignment();
        test_null_bounds_and_arrays();
        test_initializer_list_owns_independent_roots();
        test_failed_emplacement_and_reuse();
        test_concurrent_growth_and_collection();
        gc::shutdown();
        std::cout << "[PASS] rooted container tests\n";
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
