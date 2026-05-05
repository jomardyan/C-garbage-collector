# C-garbage-collector

A C++20 workspace for `GC-Lib`, a conservative mark-and-sweep garbage collector with a thread-safe Global Allocation Map, a `gc_ptr<T>` handle, and a `gc_new<T>()` factory.

This codebase is now a hardened single-owner conservative collector implementation. It is substantially stronger than the original demo, but it is not yet a fully production-grade multi-threaded garbage collector.

## Features

- Hidden `ChunkHeader` metadata on every managed allocation.
- Thread-safe Global Allocation Map backed by `std::map` for `O(log n)` interior-pointer lookup.
- Conservative root scanning through register dumps, stack scanning, and Linux global/static segment scanning.
- Explicit root-range registration for heap-hosted or platform-specific roots.
- `gc_ptr<T>` pointer wrapper with zero reference counting.
- `gc_new<T>()` factory that triggers collection when managed memory reaches 10 MB.
- Iterative mark worklist to avoid recursive collector stack overflows on deep object graphs.
- Over-aligned payload allocation support.
- VS Code build/debug configuration.
- Test suite covering cycles, reachability, interior pointers, and threshold-triggered collection.

## Project Layout

```text
.
├── CMakeLists.txt
├── include/
│   └── gc/
│       ├── ChunkHeader.hpp
│       ├── GC.hpp
│       ├── GCManager.hpp
│       ├── RootScanner.hpp
│       ├── gc_new.hpp
│       └── gc_ptr.hpp
├── src/
│   ├── GCManager.cpp
│   ├── RootScanner.cpp
│   └── main.cpp
├── tests/
│   └── gc_tests.cpp
└── .vscode/
│   ├── extensions.json
│   ├── launch.json
│   ├── settings.json
│   └── tasks.json
```

## Quick Start

Call `gc::register_stack_bottom(&argc);` once near the top of `main`, then allocate through `gc::gc_new<T>()`.

```cpp
#include "gc/GC.hpp"

int main(int argc, char** argv) {
    (void)argv;
    gc::register_stack_bottom(&argc);

    auto node = gc::gc_new<MyNode>();
    gc::GC_Manager::instance().collect();
}
```

Objects stay alive while a raw pointer value remains visible to the conservative root scan, including through `gc_ptr<T>` values stored on the stack or embedded inside other GC-managed objects.

For roots stored outside scanned stack/global memory, explicitly register their address range:

```cpp
auto external_root = std::make_unique<gc::gc_ptr<MyNode>>();
*external_root = gc::gc_new<MyNode>();

gc::GC_Manager::instance().register_root_object(external_root.get());
gc::GC_Manager::instance().collect();
gc::GC_Manager::instance().unregister_root_object(external_root.get());
```

## Collector Design

1. Allocation prefixes each payload with a compact `ChunkHeader`.
2. The Global Allocation Map stores `[payload_begin, payload_end)` ranges in a `std::map`.
3. The mark phase scans registers, the active stack, and supported global/static regions.
4. The sweep phase calls destructors first and frees memory afterward.

## Cross-Platform Notes

- Linux: global/static scanning is implemented by parsing `/proc/self/maps` and scanning writable file-backed segments.
- Non-Linux targets: stack and register scanning still work, and explicit root-range registration remains available, but automatic global/static scanning currently falls back to a no-op.
- The collector deliberately skips `[heap]` during root discovery so GC bookkeeping does not keep every allocation alive.

## Limitations

- Conservative scanning may retain objects longer than expected if a non-pointer word looks like a managed address.
- Bit-masking or otherwise disguising pointers is not supported.
- Collection is intentionally restricted to a single owner thread until a real multi-threaded stop-the-world implementation exists.
- Automatic root scanning for globals is Linux-specific today.
- The collector still lacks a true multi-thread suspend/resume mechanism, write barriers, compaction, and generational policies.

## Build and Run

### Command line

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/gc_demo
```

### VS Code

- Install recommended extensions when prompted.
- Run `Terminal -> Run Task -> build`.
- Run `Terminal -> Run Task -> test` to execute the suite.
- Press `F5` and choose `Debug gc_demo`.

## Next Steps

- Add per-thread stack registration for multi-threaded stop-the-world support.
- Add optional explicit root handles to reduce false-positive retention.
- Add a real multi-thread thread-registration and suspension layer instead of single-owner enforcement.
