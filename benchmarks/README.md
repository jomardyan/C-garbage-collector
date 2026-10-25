# Collector benchmarks

Build the optional benchmark executable with a Release build:

```bash
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release -DGC_BUILD_BENCHMARKS=ON
cmake --build build/bench --target gc_bench --parallel
./build/bench/gc_bench --objects 10000 --iterations 20 > results.csv
```

`--help` describes the options. Counts must be positive integers; duplicate,
unknown, missing, malformed, and excessive values exit with status 2. Defaults
are 1000 objects and 5 iterations. Limits are 1,000,000 objects, 200 iterations,
and 10,000,000 object-iterations. A correctness failure exits with status 1.

Each successful run writes one CSV header and five workload rows:

| Workload | Timed work | One reported operation |
| --- | --- | --- |
| `rooted_allocation` | Construct objects and register an exact root for each | One object plus root |
| `linked_graph_retained_collection` | Collect a reachable singly linked graph | One live graph node |
| `linked_graph_reclaimed_collection` | Collect and finalize the unreachable graph | One reclaimed graph node |
| `array_element_construction` | Allocate and default-construct one GC array | One array element |
| `weak_registration_and_promotion` | Register weak handles, then lock and read every handle | One registration plus promotion |

Each row includes total elapsed nanoseconds, median sample duration, nearest-rank
95th percentile, maximum duration, and aggregate operations per second. A sample
is one whole workload iteration, rather than the duration of one object operation.
The collection sample durations include finalizers and GC bookkeeping.

Container reservation, graph/weak-target setup, explicit root preparation for
arrays, result validation, CSV formatting, and cleanup are outside the timers.
Root creation is included in `rooted_allocation`; construction of the entire
array is included in `array_element_construction`. Automatic threshold
collection is disabled so allocation rows do not mix allocation and collection.
The weak row includes its liveness check and checksum read.

Every iteration verifies live block counts and exact finalizer counts. Array cleanup
uses explicit shutdown outside its allocation timer. Other reclamation
runs on a fresh registered collector thread while the allocating thread is
temporarily unregistered. This excludes stale caller-stack pointer words from
the conservative root set. Thread creation, registration, and joining are outside
the collection timer. Only null roots and weak handles remain at that point.

These are synthetic, single-mutator measurements. There is no warmup or automatic
environment normalization. Small sample counts do not support strong percentile
claims. Compare repeated Release runs on the same machine and build settings;
sanitizer timings are useful for verification, not production performance estimates.
The harness does not impose timing-based pass/fail thresholds.
