#include <iostream>
#include <memory>

#include "gc/GC.hpp"

namespace {

struct Node {
    explicit Node(int value_in) : value(value_in) {}

    int value = 0;
};

}  // namespace

int main() {
    gc::register_current_thread();

    auto external_root = std::make_unique<gc::gc_ptr<Node>>();
    *external_root = gc::gc_new<Node>(42);

    {
        // ScopedRootObject is the safe way to keep a heap-hosted gc_ptr visible to the collector.
        gc::ScopedRootObject<gc::gc_ptr<Node>> rooted_external(external_root.get());
        gc::collect();

        std::cout << "The externally stored node survived collection with value "
                  << (*external_root)->value << '\n';
    }

    // After the explicit root goes away, the collector is free to reclaim the object later.
    external_root->reset();
    gc::collect();
    gc::shutdown();
    return 0;
}