#include <cstdint>
#include <iostream>
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
    volatile std::uintptr_t noise[256] = {};
    for (std::size_t index = 0; index < 256; ++index) {
        noise[index] = index;
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

    gc::register_stack_bottom(&argc);
    auto& manager = gc::GC_Manager::instance();

    auto root = gc::gc_new<Node>("root");
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

    root.reset();
    scrub_stack();
    manager.collect();
    std::cout << "Managed blocks after dropping every root: "
              << manager.managed_block_count() << '\n';

    gc::shutdown();
    return 0;
}
