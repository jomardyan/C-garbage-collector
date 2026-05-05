# Copilot Instructions for C-garbage-collector

## Project Goals
- Build `GC-Lib`, a compact educational conservative garbage collector in modern C++.
- Keep the default collector single-threaded and stop-the-world during `gc_new` threshold checks.
- Preserve a clean public API: `gc_ptr<T>`, `gc_new<T>()`, and `GC_Manager` for explicit control.

## Architecture Guidelines
- Keep public headers in `include/gc/` and implementation in `src/`.
- `GCManager` owns the Global Allocation Map and collection lifecycle.
- `RootScanner` owns platform-specific register, stack, and global-segment scanning.
- Avoid scanning process-heap bookkeeping as roots; that would pin all managed allocations.

## Coding Conventions
- Use C++20.
- Compile cleanly with `-Wall -Wextra -Wpedantic` (or MSVC equivalent).
- Favor small classes with focused responsibilities.
- Add comments only around non-obvious algorithms or invariants.

## GC Behavior Expectations
- Allocation uses a hidden `ChunkHeader` before every payload.
- The Global Allocation Map must support interior-pointer lookup in `O(log n)`.
- Collector phases remain: clear marks, mark conservatively from roots, sweep unreachable.
- Call destructors before freeing memory.

## Testing Expectations
- Add tests for:
  - Reachability through multi-hop references.
  - Interior-pointer retention.
  - Cycles that are unreachable from roots.
  - Threshold-triggered collection.

## Copilot Contribution Style
- Propose minimal diffs.
- Keep APIs stable unless explicitly changing design.
- Update README when build/test steps or architecture changes.
