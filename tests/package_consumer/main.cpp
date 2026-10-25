#if defined(_WIN32)
// Exercise the public headers with the Windows min/max macros already defined.
#include <windows.h>
#endif

#include <gc/GC.hpp>

struct Object {
    static inline int destroyed = 0;
    int value = 42;
    ~Object() { ++destroyed; }
};

int main() {
    gc::ScopedThreadRegistration registration;
    gc::gc_root<Object> root(gc::gc_new<Object>());
    gc::gc_vector<int> numbers;
    numbers.emplace_new(7);
    gc::gc_root<int[]> generated(gc::gc_generate_array<int>(4, [](std::size_t index) {
        return static_cast<int>(index * index);
    }));
    gc::gc_root<int[]> filled(gc::gc_new_array<int>(3, 9));
    gc::collect();
    const auto info = gc::GC_Manager::instance().object_info(&generated[2]);
    const auto stats = gc::GC_Manager::instance().stats();
    if (root->value != 42 || Object::destroyed != 0 || *numbers[0] != 7 ||
        generated[3] != 9 || filled[2] != 9 || !info ||
        stats.total_allocations != 4 || stats.peak_live_blocks != 4) {
        return 1;
    }
    gc::shutdown();
    return Object::destroyed == 1 ? 0 : 2;
}
