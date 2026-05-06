#include <cstddef>
#include <iostream>

#include "gc/GC.hpp"

namespace {

struct Buffer {
    int value = 0;
};

}  // namespace

int main() {
    gc::register_current_thread();

    gc::gc_weak_ptr<Buffer[]> weak_array;

    {
        // Arrays are first-class GC allocations, so they can also be referenced weakly.
        auto array = gc::gc_new_array<Buffer>(3U);
        array[0].value = 7;
        array[1].value = 11;
        weak_array = gc::gc_weak_ptr<Buffer[]>(array);

        auto locked = weak_array.lock();
        if (locked) {
            std::cout << "Locked weak array values: "
                      << locked[0].value << ", " << locked[1].value << '\n';
        }
    }

    // shutdown() deterministically reclaims remaining blocks and clears weak handles.
    gc::shutdown();

    std::cout << "Weak array expired after shutdown: "
              << std::boolalpha << weak_array.expired() << '\n';
    return 0;
}