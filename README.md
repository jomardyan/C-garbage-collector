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
- `gc_vector<T>` for collections whose elements remain rooted through growth, copying, and moves.
- `gc_weak_ptr<T>` and `gc_weak_ptr<T[]>` weak handles that are nulled during sweep/shutdown.
- `gc_new<T>()`, `gc_new_nothrow<T>()`, `gc_new_array<T>()`, and `gc_new_array_nothrow<T>()`.
- Fill-copy and indexed generator arrays, including move-only and non-default-constructible elements.
- Over-aligned allocation support.
- Cooperative multi-thread stop-the-world collection using per-thread registration and safepoints.
- Automatic current-thread registration through `gc::register_current_thread()` or `gc::ScopedThreadRegistration`.
- Explicit root-range registration for heap-hosted or external roots.
- Heap inspection APIs through `GC_Manager::live_objects()` and `GC_Manager::dump_heap()`.
- Targeted base/interior-pointer inspection through `object_info()`, allocation totals, peak usage, and registration counts.
- Collection statistics including reclaimed bytes, durations, live bytes, reserved bytes, and fragmentation ratio.
- Dependency-aware finalization ordering during sweep and shutdown.
- Cross-platform global/static scanning on Linux, macOS, and Windows.
- CMake install/export support and CI sanitizer builds.
- Optional CSV benchmarks with correctness checks, throughput, and collection-duration distributions.

## Quick Start

The easiest way to start a single-threaded program is automatic thread registration:

```cpp
#include "gc/GC.hpp"

struct MyNode {
    gc::gc_ptr<MyNode> next;
};

int main() {
    gc::ScopedThreadRegistration registration;

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

## Rooted Collections

Ordinary STL containers keep their elements in native heap memory that the collector does not trace. `gc_vector<T>` stores an exact `gc_root<T>` for each element, so a vector hosted on the stack or ordinary heap protects its targets:

```cpp
gc::gc_vector<MyNode> nodes;
nodes.emplace_new();                  // construct and root a new MyNode
nodes.push_back(gc::gc_new<MyNode>()); // root an existing handle
nodes.reserve(128);
gc::collect();
nodes[0]->next = nodes[1].as_gc_ptr();

for (const auto& root : nodes) {
    use_node(root.as_gc_ptr());
}
nodes.clear(); // release exact roots; future collections may reclaim targets
```

Elements and iterators expose `gc_root<T>` handles. Bounds-checked `at()`, iteration, `erase()`, `pop_back()`, `resize()`, copying, moving, and swapping follow vector conventions. New slots created by `resize()` are null. Iterator/reference invalidation follows `std::vector`; managed target addresses remain stable. `gc_vector<T[]>` also holds array roots. `emplace_new()` constructs scalar objects; use `push_back(gc_new_array<T>(...))` for arrays.

Each element uses one explicit root registration. Growth can temporarily register copies of those roots and can throw on allocation failure. Reserve capacity when the expected size is known. Container operations and target access still require registered mutator threads and normal application synchronization.

Treat `gc_vector` as an external root owner. If it is embedded in a managed object and an element leads back to that owner, the unconditional root can keep the cycle alive until those slots are cleared explicitly.

## Filled and Generated Arrays

```cpp
gc::gc_root<int[]> filled(gc::gc_new_array<int>(8, 42));
gc::gc_root<int[]> squares(gc::gc_generate_array<int>(8, [](std::size_t index) {
    return static_cast<int>(index * index);
}));
```

The fill overload copy-constructs each element from the supplied value. The generator is called once for each index in increasing order and constructs elements directly, supporting non-default-constructible and move-only values. Zero-length arrays return null and never invoke the generator. If construction throws, completed elements are destroyed in reverse order and the block is released. Matching `gc_new_array_nothrow(count, value)` and `gc_generate_array_nothrow(count, generator)` return null on failure.

Root any managed targets referenced by the fill value or captured by the generator; closures and native containers do not automatically become roots.

## Multi-Threaded Use

Every mutator thread must register before using the GC. The RAII helper is the easiest way to do that:

```cpp
void worker() {
    gc::ScopedThreadRegistration registration;

    gc::gc_root<MyNode> object(gc::gc_new<MyNode>());
    while (running) {
        gc::safepoint();
        do_work(object);
    }
}
```

The collector uses a cooperative stop-the-world model. Every registered thread must periodically call `gc::safepoint()` or a manager API so collection can park it and scan its stack. `live_objects()` and `dump_heap()` also park mutators before reading their payloads. Nested `ScopedThreadRegistration` scopes preserve the outer registration, including an existing manual registration.

A registered thread must not block indefinitely in `join()`, a condition-variable wait, or an application mutex while another thread may collect. A collector waits for that thread to reach a safepoint. For a joining thread that holds no GC roots, defer registration until after the join, as in `examples/worker_threads.cpp`. Never unregister a thread that still owns the only roots to live objects.

Application data still needs normal C++ synchronization. Different threads must not concurrently mutate the same handle or payload without synchronization. Weak handles must be used on registered threads; `lock()` promotes a live target to a `gc_ptr`, which should be placed in a `gc_root` if it must survive later safepoints.

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
auto object = gc::GC_Manager::instance().object_info(pointer);
gc::GC_Manager::instance().dump_heap(std::cout);
```

`live_objects()` returns per-object payload size, reserved size, alignment, mark state, and outgoing references. `dump_heap()` emits a human-readable heap snapshot and object graph. Snapshot addresses are diagnostic values; they do not extend object lifetimes. Each query is a separate snapshot, so concurrent allocations can change counts between queries.

`object_info()` resolves a payload base or any interior byte to its owning allocation, returning `std::nullopt` for null, unknown, or one-past-the-allocation addresses. Array element pointers resolve to the allocation containing the count prefix. It parks registered mutators before reading outgoing references, just as a whole-heap snapshot does.

`GCStats` also reports `total_allocations`, `bytes_allocated_total`, `peak_live_bytes`, `peak_live_blocks`, `registered_threads`, `registered_root_ranges`, and `registered_weak_refs`. Allocation totals count successful registry insertions, including blocks later released because a constructor throws. Byte counters describe managed payloads, including array prefixes; they exclude native container storage and collector bookkeeping. Allocation totals saturate on overflow. `shutdown()` resets allocation history and peaks while preserving active thread/root registrations.

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

For a library-only build:

```bash
cmake -S . -B build/lib -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DGC_BUILD_DEMO=OFF -DGC_BUILD_EXAMPLES=OFF
cmake --build build/lib --parallel
```

These optional targets default to off when the project is included with `add_subdirectory()`. Use `-DBUILD_SHARED_LIBS=ON` to build a shared library.

The library also exposes install/export metadata for downstream CMake consumers:

```bash
cmake -S . -B build
cmake --build build
cmake --install build --prefix /tmp/gclib-install
```

Downstream projects inherit the C++20 and thread requirements from the exported target:

```cmake
find_package(GCLib 0.1 CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE GCLib::gc_lib)
```

Pass `-DCMAKE_PREFIX_PATH=/tmp/gclib-install` when configuring the consumer.

Version 0.2 adds fields to the public `GCStats` structure. Rebuild downstream binaries when upgrading from 0.1; shared libraries use a separate 0.2 ABI version. Existing source consumers can still request `find_package(GCLib 0.1 CONFIG REQUIRED)` and rebuild against the new headers.

## Validation

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure

cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGC_ENABLE_ASAN=ON
cmake --build build/asan --parallel
ctest --test-dir build/asan --output-on-failure

cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGC_ENABLE_TSAN=ON
cmake --build build/tsan --parallel
ctest --test-dir build/tsan --output-on-failure
```

Sanitizers require GCC or Clang and must use separate build directories; ASan and TSan cannot be enabled together. Instrumentation covers the library, public templates, tests, and examples. The scanner's raw conservative reads are excluded from TSan instrumentation. The standalone, single-threaded allocation-failure test also runs without TSan because its replacement `operator new/delete` conflicts with Clang's TSan runtime; it remains enabled and is instrumented in ASan builds. CTest sets `ASAN_OPTIONS=detect_stack_use_after_return=0`; use the same setting when running ASan examples directly. ASan scanning skips poisoned words. Tests cover construction failures, nested roots, shutdown/reuse, concurrent collection and snapshots, weak promotion, global BSS roots, and installed-package consumption.

CI runs Linux Release, Debug/shared, ASan/UBSan, and TSan builds, plus macOS and Windows builds. A local Linux pass does not validate other operating systems; check the corresponding CI jobs before release.

The runtime suites cover:

| Area | Checks |
| --- | --- |
| Core collection | Reachable/deep graphs, dead cycles, interior pointers, thresholds, arrays, alignment, weak handles, STL compatibility |
| Lifecycle and concurrency | Nested registrations, shutdown/reuse, constructor collection, competing collectors, snapshots, weak promotion |
| New APIs | Vector growth/copy/move/erase, generated/fill arrays, partial failure cleanup, object lookup boundaries, cumulative and peak statistics |
| Property tests | Eight fixed random graph seeds, model-derived reachability through staged root removal, weak expiry, a 4,096-node cycle |
| Failure contracts | Registry/graph allocation failure, rooted-vector growth failure, DAG finalizer order, recursive collection, throwing finalizers in isolated processes |
| Integration | Every public header compiled independently, installed-package consumers, demo/example runs, benchmark output and invalid CLI arguments |

CTest labels support focused runs:

```bash
ctest --test-dir build/release -L unit --output-on-failure
ctest --test-dir build/release -L stress --output-on-failure
ctest --test-dir build/release -L integration --output-on-failure
```

Reclamation assertions use fresh registered collector threads after the allocating thread relinquishes its roots, avoiding reliance on stale conservative stack values disappearing. Retention assertions run with their intended roots registered. Checks remain active in Release builds. Passing these suites does not establish exhaustive coverage of every workload or thread schedule.

## Performance Benchmarks

```bash
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release -DGC_BUILD_BENCHMARKS=ON
cmake --build build/bench --parallel
./build/bench/gc_bench --objects 10000 --iterations 20 > results.csv
```

The optional tool measures rooted allocation, retained and reclaimed graph collection, array construction, and weak registration/promotion. CSV output includes throughput and median/p95/maximum iteration durations. Each workload verifies object counts and finalization; timings have no pass/fail threshold. Compare repeated Release builds on the same machine. These synthetic measurements do not establish production latency or a speedup over earlier versions. See [benchmark methodology](benchmarks/README.md) for timed regions, CLI limits, and interpretation.

## Lifetime and Finalization

`gc_ptr` is a lightweight pointer value. It keeps its target reachable only when its storage is scanned. Prefer `gc_root` for locals that must survive collection in optimized builds, and register ranges for handles stored in ordinary heap allocations. The collector does not follow storage owned by `std::vector`, `std::map`, or other native heap containers automatically. Root each target explicitly or register the container's live element storage, updating ranges whenever it reallocates.

Allocations are pinned while their constructors run, so nested allocation and `collect()` are safe during construction. Failed construction releases the allocation and destroys only completed array elements. Calling `shutdown()` while construction is active throws `std::logic_error`.

Finalizers run on the collecting thread while other mutators are parked. They must not wait for those threads, throw, or resurrect objects being finalized. References discovered between unreachable objects determine finalization order for acyclic dependencies. Cycles have no safe dependency order; a finalizer must not dereference another object in the same unreachable cycle. Recursive `collect()` and `shutdown()` from a finalizer are no-ops.

`shutdown()` forcefully destroys all current allocations, including rooted objects, clears weak handles, and resets statistics. Strong handles become dangling and must be reset before further use. Root registrations and thread registrations survive shutdown, allowing existing root storage to be reused for new allocations. Unregister every explicit range before its backing storage is freed. GC handles must be destroyed before the manager singleton; explicitly shut down after workers have exited and before application resources used by finalizers are destroyed.

## Platform Notes

- Linux scans writable ELF load segments, including anonymous zero-filled BSS tails, while excluding general process heap storage and collector allocation bookkeeping.
- macOS scans selected `__DATA` and `__DATA_CONST` sections from loaded Mach-O images.
- Windows scans committed read-write PE image sections through `VirtualQuery`.
- AddressSanitizer builds retain global scanning, skip poisoned words, and require `detect_stack_use_after_return=0` for conservative stack discovery.
- Unsupported platforms still support stack scanning and explicit root registration, but automatic global scanning is disabled.

## Limitations

- This is still a conservative collector. Non-pointer bit patterns that look like managed addresses can retain objects longer than expected.
- Cooperative stop-the-world is not the same as preemptive OS-level thread suspension. Threads that never hit GC APIs or `gc::safepoint()` can delay collection.
- The collector does not compact, move, or generationally age objects.
- There are no write barriers, incremental barriers, or exact compiler-provided root maps.
- Encoded or disguised pointers, thread-local variables, and pointers stored at unaligned addresses are not discovered automatically. Use exact roots or registered pointer-aligned storage. Interior pointer values may themselves be unaligned.
- Arbitrary stack switching, fibers, coroutines with heap-hosted frames, and unloading modules during collection require application-managed explicit roots and lifetime coordination.

## Project Layout

```text
.
├── CMakeLists.txt
├── cmake/
├── examples/
├── benchmarks/
├── include/gc/
├── src/
├── tests/
└── .github/workflows/
```
