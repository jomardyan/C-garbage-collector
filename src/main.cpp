#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

#include "gc/GC.hpp"

namespace {

#if defined(_MSC_VER)
#define GC_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define GC_NOINLINE __attribute__((noinline))
#else
#define GC_NOINLINE
#endif

struct Node {
    explicit Node(std::string label_value) : label(std::move(label_value)) {}

    gc::gc_ptr<Node> next;
    std::string label;

    ~Node() {
        std::cout << "Destroying " << label << '\n';
    }
};

GC_NOINLINE void scrub_stack() {
    std::uintptr_t noise[256] = {};
    for (std::size_t index = 0; index < 256; ++index) {
        noise[index] = index;
    }

#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : "g"(noise) : "memory");
#endif

    std::uintptr_t sink = 0;
    for (std::size_t index = 0; index < 256; ++index) {
        sink ^= noise[index];
    }
    if (sink == static_cast<std::uintptr_t>(-1)) {
        std::cout << "unreachable\n";
    }
}

GC_NOINLINE void build_unreachable_cycle() {
    auto alpha = gc::gc_new<Node>("alpha");
    auto beta = gc::gc_new<Node>("beta");
    alpha->next = beta;
    beta->next = alpha;
}

}  // namespace

int main(int argc, char** argv) {
    (void)argv;
    (void)argc;

    gc::register_current_thread();
    auto& manager = gc::GC_Manager::instance();

    gc::gc_root<Node> root(gc::gc_new<Node>("root"));
    root->next = gc::gc_new<Node>("child");

    std::cout << "Managed blocks before collection: "
              << manager.managed_block_count() << '\n';
    manager.collect();
    std::cout << "Managed blocks with a root on the stack: "
              << manager.managed_block_count() << '\n';

    build_unreachable_cycle();
    scrub_stack();
    manager.collect();
    std::cout << "Managed blocks after reclaiming an unreachable cycle: "
              << manager.managed_block_count() << '\n';

    std::ostringstream snapshot;
    manager.dump_heap(snapshot);
    std::cout << snapshot.str();

    root.reset();
    scrub_stack();
    manager.collect();
    std::cout << "Managed blocks after dropping every root: "
              << manager.managed_block_count() << '\n';

    gc::shutdown();
    return 0;
}
