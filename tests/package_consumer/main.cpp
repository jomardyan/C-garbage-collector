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
    gc::collect();
    if (root->value != 42 || Object::destroyed != 0) {
        return 1;
    }
    gc::shutdown();
    return Object::destroyed == 1 ? 0 : 2;
}
