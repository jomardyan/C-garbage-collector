# C-garbage-collector

`GC-Lib` is a C++20 conservative mark-and-sweep collector with interior-pointer lookup, weak references, array allocation, cooperative multi-thread stop-the-world collection, installable CMake package metadata, and a test/CI setup suitable for library development.

It is substantially more capable than the original demo collector. It now provides automatic thread registration helpers, weak-pointer invalidation, array allocation, heap inspection APIs, dependency-aware finalization ordering, and cross-platform global scanning on Linux, macOS, and Windows.

Because the collector is conservative, ambiguous stack or global words can delay reclamation. Weak handles are cleared when their targets are actually reclaimed, but false-positive roots can defer that reclamation.

## Features

- Conservative mark-and-sweep collection with iterative marking.
- Hidden `ChunkHeader` metadata on every managed allocation.
- Interior-pointer lookup through a global allocation map.
- Automatic threshold-triggered collection via `collect_if_needed()`.
- `gc_ptr<T>` strong handles with STL ordering/hash support.
- `gc_root<T>` exact RAII roots for optimization-robust local rooting.
- `gc_weak_ptr<T>` and `gc_weak_ptr<T[]>` weak handles that are nulled during sweep/shutdown.
- `gc_new<T>()`, `gc_new_nothrow<T>()`, `gc_new_array<T>()`, and `gc_new_array_nothrow<T>()`.
- Over-aligned allocation support.
- Cooperative multi-thread stop-the-world collection using per-thread registration and safepoints.
- Automatic current-thread registration through `gc::register_current_thread()` or `gc::ScopedThreadRegistration`.
- Explicit root-range registration for heap-hosted or external roots.
- Heap inspection APIs through `GC_Manager::live_objects()` and `GC_Manager::dump_heap()`.
- Collection statistics including reclaimed bytes, durations, live bytes, reserved bytes, and fragmentation ratio.
- Dependency-aware finalization ordering during sweep and shutdown.
- Cross-platform global/static scanning on Linux, macOS, and Windows.
- CMake install/export support and CI sanitizer builds.

## Quick Start

The easiest way to start a single-threaded program is automatic thread registration:

```cpp
#include "gc/GC.hpp"

int main() {
    gc::register_current_thread();

    gc::gc_root<MyNode> root(gc::gc_new<MyNode>());
    root->next = gc::gc_new<MyNode>();

    gc::collect();
    gc::shutdown();
}
```

If you already know the stack anchor you want to use, you can still register it explicitly:

```cpp
int main(int argc, char** argv) {
    (void)argv;
    gc::register_stack_bottom(&argc);
}
```

For local variables that must stay alive across optimized builds without relying on conservative register discovery, prefer `gc_root<T>` over a plain stack `gc_ptr<T>`.

## Multi-Threaded Use

Every mutator thread must register before using the GC. The RAII helper is the easiest way to do that:

```cpp
void worker() {
    gc::ScopedThreadRegistration registration;

    auto object = gc::gc_new<MyNode>();
    while (running) {
        gc::safepoint();
        do_work(object);
    }
}
```

The collector uses a cooperative stop-the-world model. Registered worker threads must either be inside GC API calls or periodically call `gc::safepoint()` so collection can park them and scan their stacks.

## Explicit Roots

For roots stored outside scanned stack or global memory, register the containing range explicitly:

```cpp
auto external_root = std::make_unique<gc::gc_ptr<MyNode>>();
*external_root = gc::gc_new<MyNode>();

gc::GC_Manager::instance().register_root_object(external_root.get());
gc::collect();
gc::GC_Manager::instance().unregister_root_object(external_root.get());
```

For production code, prefer the RAII helpers so explicit roots cannot be leaked accidentally:

```cpp
auto external_root = std::make_unique<gc::gc_ptr<MyNode>>();
*external_root = gc::gc_new<MyNode>();

gc::ScopedRootObject<gc::gc_ptr<MyNode>> rooted_external(external_root.get());
gc::collect();
```

## Debugging and Introspection

```cpp
auto stats = gc::GC_Manager::instance().stats();
auto live = gc::GC_Manager::instance().live_objects();
gc::GC_Manager::instance().dump_heap(std::cout);
```

`live_objects()` returns per-object payload size, reserved size, alignment, mark state, and outgoing references. `dump_heap()` emits a human-readable heap snapshot and object graph.

## Build and Run

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/gc_demo
```

Sample programs are built by default under `build/examples/`:

```bash
./build/examples/gc_example_quick_start
./build/examples/gc_example_explicit_roots
./build/examples/gc_example_weak_arrays
./build/examples/gc_example_worker_threads
```

Set `-DGC_BUILD_EXAMPLES=OFF` if you want a smaller library-only build.

The library also exposes install/export metadata for downstream CMake consumers:

```bash
cmake -S . -B build
cmake --build build
cmake --install build --prefix /tmp/gclib-install
```

## Platform Notes

- Linux scans writable file-backed mappings from `/proc/self/maps` and skips `[heap]` so collector bookkeeping does not self-root the heap.
- macOS scans selected `__DATA` and `__DATA_CONST` sections from loaded Mach-O images.
- Windows scans committed read-write PE image sections through `VirtualQuery`.
- AddressSanitizer builds disable Linux writable-mapping global scanning and run tests with `detect_stack_use_after_return=0`, because ASan fake-stack instrumentation conflicts with conservative stack scanning.
- Unsupported platforms still support stack scanning and explicit root registration, but automatic global scanning is disabled.

## Limitations

- This is still a conservative collector. Non-pointer bit patterns that look like managed addresses can retain objects longer than expected.
- Cooperative stop-the-world is not the same as preemptive OS-level thread suspension. Threads that never hit GC APIs or `gc::safepoint()` can delay collection.
- The collector does not compact, move, or generationally age objects.
- There are no write barriers, incremental barriers, or exact compiler-provided root maps.
- Encoded or disguised pointers are not tracked.

## Project Layout

```text
.
├── CMakeLists.txt
├── cmake/
├── examples/
├── include/gc/
├── src/
├── tests/
└── .github/workflows/
```
