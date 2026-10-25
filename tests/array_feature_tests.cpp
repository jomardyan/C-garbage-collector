#include "gc/GC.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct NonDefault {
    int value;
    explicit NonDefault(int initial) : value(initial) {}
};

struct Immovable {
    static inline int destroyed = 0;
    int value;
    explicit Immovable(int initial) : value(initial) {}
    Immovable(const Immovable&) = delete;
    Immovable(Immovable&&) = delete;
    ~Immovable() { ++destroyed; }
};

struct DestructionOrder {
    static inline std::array<int, 16> values{};
    static inline std::size_t count = 0;
    int id;
    explicit DestructionOrder(int identifier) : id(identifier) {}
    DestructionOrder(const DestructionOrder&) = delete;
    DestructionOrder(DestructionOrder&&) = delete;
    ~DestructionOrder() { values[count++] = id; }
};

struct ThrowingCopy {
    static inline int attempts = 0;
    static inline std::array<int, 16> destroyed{};
    static inline std::size_t destroyed_count = 0;
    int id = -1;
    ThrowingCopy() = default;
    ThrowingCopy(const ThrowingCopy&) : id(attempts++) {
        if (id == 2) throw std::runtime_error("expected copy failure");
    }
    ~ThrowingCopy() {
        if (id >= 0) destroyed[destroyed_count++] = id;
    }
};

struct alignas(256) Aligned {
    std::size_t value;
    explicit Aligned(std::size_t initial) : value(initial) {}
};

struct CollectingElement {
    static inline int destroyed = 0;
    gc::gc_ptr<int> child;
    std::size_t index;
    explicit CollectingElement(std::size_t value) : child(gc::gc_new<int>(17)), index(value) {
        gc::collect();
        expect(*child == 17 && destroyed == 0,
               "generation must pin the array and completed elements during nested collection");
    }
    CollectingElement(const CollectingElement&) = delete;
    CollectingElement(CollectingElement&&) = delete;
    ~CollectingElement() { ++destroyed; }
};

struct MoveOnlyGenerator {
    std::unique_ptr<int> offset = std::make_unique<int>(11);
    MoveOnlyGenerator() = default;
    MoveOnlyGenerator(MoveOnlyGenerator&&) = default;
    MoveOnlyGenerator(const MoveOnlyGenerator&) = delete;
    int operator()(std::size_t index) & { return *offset + static_cast<int>(index); }
    int operator()(std::size_t) && = delete;
};

void test_fill_and_default_arrays() {
    gc::shutdown();
    gc::gc_root<int[]> defaults(gc::gc_new_array<int>(4));
    for (std::size_t index = 0; index < 4; ++index) {
        expect(defaults[index] == 0, "default arrays must remain value-initialized");
    }
    NonDefault initial(42);
    gc::gc_root<NonDefault[]> filled(gc::gc_new_array<NonDefault>(5, initial));
    for (std::size_t index = 0; index < 5; ++index) {
        expect(filled[index].value == 42, "fill arrays must copy-construct every element");
    }
    filled[0].value = 9;
    expect(initial.value == 42 && filled[1].value == 42,
           "fill arrays must contain independent object copies");
    gc::gc_root<const int[]> constants(gc::gc_new_array<const int>(3, 7));
    expect(constants[0] == 7 && constants[2] == 7, "fill arrays must support const elements");
    expect(gc::gc_new_array_nothrow<NonDefault>(2, initial)[1].value == 42,
           "the nothrow fill overload must construct usable elements");
    gc::shutdown();
}

void test_generated_arrays() {
    gc::shutdown();
    gc::gc_root<std::unique_ptr<int>[]> move_only(gc::gc_generate_array<std::unique_ptr<int>>(4,
        [](std::size_t index) { return std::make_unique<int>(static_cast<int>(index * index)); }));
    expect(*move_only[3] == 9, "generation must support move-only elements without default construction");

    Immovable::destroyed = 0;
    gc::gc_root<Immovable[]> immovable(gc::gc_generate_array<Immovable>(3,
        [](std::size_t index) { return Immovable(static_cast<int>(index)); }));
    expect(immovable[2].value == 2 && Immovable::destroyed == 0,
           "direct generation must avoid temporary element copies and moves");

    gc::gc_root<int[]> from_move_only(gc::gc_generate_array<int>(3, MoveOnlyGenerator{}));
    expect(from_move_only[0] == 11 && from_move_only[2] == 13,
           "generation must preserve move-only callable state across ordered invocations");
    std::size_t next = 0;
    auto stateful = [&next](std::size_t index) {
        expect(index == next++, "generation must invoke indices in ascending order");
        return static_cast<int>(index + 1);
    };
    gc::gc_root<const int[]> const_values(gc::gc_generate_array<const int>(4, std::ref(stateful)));
    expect(next == 4 && const_values[3] == 4,
           "generation must support const elements and reference-wrapper callables");
    gc::gc_root<int[]> nothrow_values(gc::gc_generate_array_nothrow<int>(2,
        [](std::size_t index) { return static_cast<int>(index + 10); }));
    expect(nothrow_values && nothrow_values[1] == 11,
           "nothrow generation must return usable values on success");
    gc::shutdown();
    expect(Immovable::destroyed == 3, "each generated immovable element must finalize exactly once");
}

void test_alignment_and_nested_collection() {
    gc::shutdown();
    gc::gc_root<Aligned[]> aligned(gc::gc_generate_array<Aligned>(3,
        [](std::size_t index) { return Aligned(index); }));
    expect(reinterpret_cast<std::uintptr_t>(aligned.get()) % alignof(Aligned) == 0,
           "generated arrays must preserve requested overalignment");
    expect(aligned[2].value == 2, "overaligned generated elements must remain usable");
    aligned.reset();
    gc::shutdown();

    CollectingElement::destroyed = 0;
    auto& manager = gc::GC_Manager::instance();
    manager.set_collection_threshold_bytes(0);
    gc::gc_root<CollectingElement[]> generated(gc::gc_generate_array<CollectingElement>(4,
        [](std::size_t index) { return CollectingElement(index); }));
    gc::collect();
    for (std::size_t index = 0; index < 4; ++index) {
        expect(generated[index].index == index && *generated[index].child == 17,
               "every generated element and its managed child must remain reachable");
    }
    expect(manager.managed_block_count() == 5,
           "the generated array and its four child allocations must all remain alive");
    gc::shutdown();
    expect(CollectingElement::destroyed == 4, "completed generated elements must finalize once");
    manager.set_collection_threshold_bytes(10U * 1024U * 1024U);
}

void test_generator_failure() {
    gc::shutdown();
    DestructionOrder::count = 0;
    std::size_t calls = 0;
    bool threw = false;
    try {
        (void)gc::gc_generate_array<DestructionOrder>(6, [&calls](std::size_t index) {
            ++calls;
            if (index == 3) throw std::runtime_error("expected generator failure");
            return DestructionOrder(static_cast<int>(index));
        });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw && calls == 4, "a generator exception must stop generation and propagate");
    expect(DestructionOrder::count == 3 && DestructionOrder::values[0] == 2 &&
               DestructionOrder::values[1] == 1 && DestructionOrder::values[2] == 0,
           "generator failure must destroy only completed elements in reverse order");
    expect(gc::GC_Manager::instance().managed_block_count() == 0,
           "generator failure must release the array allocation");
    gc::shutdown();
    expect(DestructionOrder::count == 3, "failed generation must never double-finalize elements");

    DestructionOrder::count = 0;
    (void)gc::gc_generate_array<DestructionOrder>(3,
        [](std::size_t index) { return DestructionOrder(static_cast<int>(index)); });
    gc::shutdown();
    expect(DestructionOrder::count == 3 && DestructionOrder::values[0] == 2 &&
               DestructionOrder::values[1] == 1 && DestructionOrder::values[2] == 0,
           "successful generated arrays must also finalize in reverse element order");

    auto failed = gc::gc_generate_array_nothrow<int>(2, [](std::size_t) -> int {
        throw std::runtime_error("expected nothrow generator failure");
    });
    expect(!failed && gc::GC_Manager::instance().managed_block_count() == 0,
           "nothrow generation must return null and release storage on callable failure");
}

void test_fill_failure_and_boundaries() {
    gc::shutdown();
    ThrowingCopy original;
    ThrowingCopy::attempts = 0;
    ThrowingCopy::destroyed_count = 0;
    bool threw = false;
    try {
        (void)gc::gc_new_array<ThrowingCopy>(5, original);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw && ThrowingCopy::attempts == 3 && ThrowingCopy::destroyed_count == 2 &&
               ThrowingCopy::destroyed[0] == 1 && ThrowingCopy::destroyed[1] == 0,
           "failed fill copies must clean up only completed elements in reverse order");
    expect(gc::GC_Manager::instance().managed_block_count() == 0,
           "fill-copy failure must release the array allocation");
    ThrowingCopy::attempts = 0;
    ThrowingCopy::destroyed_count = 0;
    expect(!gc::gc_new_array_nothrow<ThrowingCopy>(5, original),
           "the nothrow fill overload must return null for element copy failure");
    expect(ThrowingCopy::destroyed_count == 2,
           "the nothrow fill overload must still destroy completed copies");

    std::size_t calls = 0;
    auto generator = [&calls](std::size_t) { ++calls; return 1; };
    expect(!gc::gc_generate_array<int>(0, generator) && calls == 0,
           "zero-count generation must return null without invoking the callable");
    expect(!gc::gc_new_array<ThrowingCopy>(0, original) && ThrowingCopy::attempts == 3,
           "zero-count fill must not copy the source");
    bool overflow_threw = false;
    try {
        (void)gc::gc_generate_array<int>((std::numeric_limits<std::size_t>::max)(), generator);
    } catch (const std::bad_alloc&) {
        overflow_threw = true;
    }
    expect(overflow_threw && calls == 0, "throwing array generation must report overflow as std::bad_alloc");
    expect(!gc::gc_generate_array_nothrow<int>((std::numeric_limits<std::size_t>::max)(), generator) &&
               calls == 0,
           "overflowing generation must fail before invoking the callable");
    expect(!gc::gc_new_array_nothrow<int>((std::numeric_limits<std::size_t>::max)(), 2),
           "overflowing fill arrays must fail without allocation");
    expect(gc::GC_Manager::instance().managed_block_count() == 0,
           "failed and empty array factories must leave no managed storage");
}

}  // namespace

int main() {
    try {
        gc::ScopedThreadRegistration registration;
        test_fill_and_default_arrays();
        test_generated_arrays();
        test_alignment_and_nested_collection();
        test_generator_failure();
        test_fill_failure_and_boundaries();
        gc::shutdown();
        std::cout << "Array feature tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Array feature failure: " << error.what() << '\n';
        return 1;
    }
}
