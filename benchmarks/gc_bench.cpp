#include "gc/GC.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

struct Options {
    std::size_t objects = 1000;
    std::size_t iterations = 5;
};

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::size_t positive_integer(std::string_view input, std::string_view option) {
    std::size_t result = 0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || result == 0) {
        throw UsageError(std::string(option) + " requires a positive integer");
    }
    return result;
}

Options parse_options(int argc, char** argv) {
    Options options;
    bool saw_objects = false;
    bool saw_iterations = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument != "--objects" && argument != "--iterations") {
            throw UsageError("unknown argument: " + std::string(argument));
        }
        if (index + 1 == argc) {
            throw UsageError("missing value for " + std::string(argument));
        }
        bool& seen = argument == "--objects" ? saw_objects : saw_iterations;
        if (seen) throw UsageError("duplicate option: " + std::string(argument));
        seen = true;
        const auto value = positive_integer(argv[++index], argument);
        if (argument == "--objects") options.objects = value;
        else options.iterations = value;
    }
    if (options.objects > 1'000'000 || options.iterations > 200 ||
        options.objects > 10'000'000 / options.iterations) {
        throw UsageError("limits: objects <= 1000000, iterations <= 200, objects * iterations <= 10000000");
    }
    return options;
}

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Node {
    static inline std::size_t destroyed = 0;
    explicit Node(std::size_t identifier) : value(identifier) {}
    gc::gc_ptr<Node> next;
    std::size_t value;
    ~Node() { ++destroyed; }
};

struct Element {
    static inline std::size_t destroyed = 0;
    std::uint64_t value = 17;
    ~Element() { ++destroyed; }
};

struct Samples {
    std::string_view name;
    std::vector<std::uint64_t> times;

    void add(Clock::time_point start, Clock::time_point finish) {
        times.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<Nanoseconds>(finish - start).count()));
    }

    void print(const Options& options) const {
        auto ordered = times;
        std::sort(ordered.begin(), ordered.end());
        std::uint64_t total = 0;
        for (auto value : ordered) total += value;
        const auto count = ordered.size();
        const auto median = count % 2 == 0
            ? static_cast<double>(ordered[count / 2 - 1]) / 2.0 +
                  static_cast<double>(ordered[count / 2]) / 2.0
            : static_cast<double>(ordered[count / 2]);
        const auto p95 = ordered[(count * 95 + 99) / 100 - 1];
        const auto operations = options.objects * options.iterations;
        const double throughput = total == 0 ? 0.0
            : static_cast<double>(operations) * 1'000'000'000.0 / static_cast<double>(total);
        std::cout << name << ',' << options.objects << ',' << options.iterations << ','
                  << operations << ',' << total << ',' << median << ',' << p95 << ','
                  << ordered.back() << ',' << throughput << '\n';
    }
};

void reset_heap() {
    gc::shutdown();
    Node::destroyed = 0;
    Element::destroyed = 0;
    // Each workload controls when to collect. Threshold pressure must not add
    // unreported collections to allocation measurements.
    gc::GC_Manager::instance().set_collection_threshold_bytes(
        (std::numeric_limits<std::size_t>::max)());
}

Nanoseconds collect_without_caller_stack() {
    gc::unregister_current_thread();
    std::exception_ptr error;
    Nanoseconds elapsed{0};
    try {
        std::thread collector([&] {
            try {
                gc::ScopedThreadRegistration registration;
                const auto start = Clock::now();
                gc::collect();
                elapsed = std::chrono::duration_cast<Nanoseconds>(Clock::now() - start);
            } catch (...) {
                error = std::current_exception();
            }
        });
        collector.join();
    } catch (...) {
        gc::register_current_thread();
        throw;
    }
    gc::register_current_thread();
    if (error) std::rethrow_exception(error);
    return elapsed;
}

void benchmark_rooted_allocation(const Options& options, Samples& samples) {
    for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
        reset_heap();
        std::vector<gc::gc_root<Node>> roots;
        roots.reserve(options.objects);
        const auto start = Clock::now();
        for (std::size_t index = 0; index < options.objects; ++index) {
            roots.emplace_back(gc::gc_new<Node>(index));
        }
        samples.add(start, Clock::now());
        expect(gc::GC_Manager::instance().managed_block_count() == options.objects &&
                   Node::destroyed == 0 && roots.back()->value == options.objects - 1,
               "rooted allocation did not retain every object");
        roots.clear();
        (void)collect_without_caller_stack();
        expect(Node::destroyed == options.objects &&
                   gc::GC_Manager::instance().managed_block_count() == 0,
               "rooted allocation cleanup did not reclaim every object");
    }
}

void benchmark_graph_collection(const Options& options, Samples& retained, Samples& reclaimed) {
    for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
        reset_heap();
        gc::gc_root<Node> head;
        for (std::size_t index = 0; index < options.objects; ++index) {
            auto node = gc::gc_new<Node>(index);
            node->next = head.as_gc_ptr();
            head = node;
        }
        const auto start = Clock::now();
        gc::collect();
        retained.add(start, Clock::now());
        expect(gc::GC_Manager::instance().managed_block_count() == options.objects &&
                   Node::destroyed == 0 && head->value == options.objects - 1,
               "retained graph collection lost reachable objects");
        head.reset();
        reclaimed.times.push_back(static_cast<std::uint64_t>(collect_without_caller_stack().count()));
        expect(Node::destroyed == options.objects &&
                   gc::GC_Manager::instance().managed_block_count() == 0,
               "unreachable graph collection did not reclaim every object");
    }
}

void benchmark_array(const Options& options, Samples& samples) {
    for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
        reset_heap();
        gc::gc_root<Element[]> array;
        const auto start = Clock::now();
        array = gc::gc_new_array<Element>(options.objects);
        samples.add(start, Clock::now());
        expect(gc::GC_Manager::instance().managed_block_count() == 1 &&
                   Element::destroyed == 0 && array[options.objects - 1].value == 17,
               "array allocation returned invalid storage");
        array.reset();
        gc::shutdown();
        expect(Element::destroyed == options.objects &&
                   gc::GC_Manager::instance().managed_block_count() == 0,
               "array cleanup did not finalize every element");
    }
}

void benchmark_weak_references(const Options& options, Samples& samples) {
    for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
        reset_heap();
        std::vector<gc::gc_root<Node>> roots;
        std::vector<gc::gc_weak_ptr<Node>> weak;
        roots.reserve(options.objects);
        weak.reserve(options.objects);
        for (std::size_t index = 0; index < options.objects; ++index) {
            roots.emplace_back(gc::gc_new<Node>(index));
        }
        std::uint64_t checksum = 0;
        const auto start = Clock::now();
        for (const auto& root : roots) weak.emplace_back(root.as_gc_ptr());
        for (const auto& handle : weak) {
            const auto locked = handle.lock();
            expect(static_cast<bool>(locked), "weak promotion lost a rooted target");
            checksum += locked->value;
        }
        samples.add(start, Clock::now());
        const auto objects = static_cast<std::uint64_t>(options.objects);
        expect(checksum == objects * (objects - 1) / 2 && Node::destroyed == 0,
               "weak promotion returned the wrong targets");
        roots.clear();
        (void)collect_without_caller_stack();
        expect(Node::destroyed == options.objects &&
                   gc::GC_Manager::instance().managed_block_count() == 0,
               "weak references prevented target reclamation");
        for (const auto& handle : weak) {
            expect(handle.expired(), "a reclaimed target still has a live weak handle");
        }
    }
}

void run(const Options& options) {
    gc::ScopedThreadRegistration registration;
    Samples allocation{"rooted_allocation", {}};
    Samples retained{"linked_graph_retained_collection", {}};
    Samples reclaimed{"linked_graph_reclaimed_collection", {}};
    Samples array{"array_element_construction", {}};
    Samples weak{"weak_registration_and_promotion", {}};
    for (auto* samples : {&allocation, &retained, &reclaimed, &array, &weak}) {
        samples->times.reserve(options.iterations);
    }
    benchmark_rooted_allocation(options, allocation);
    benchmark_graph_collection(options, retained, reclaimed);
    benchmark_array(options, array);
    benchmark_weak_references(options, weak);
    gc::shutdown();
    std::cout << "workload,objects,iterations,operations,total_ns,median_ns,p95_ns,max_ns,operations_per_second\n"
              << std::fixed << std::setprecision(3);
    for (const auto* samples : {&allocation, &retained, &reclaimed, &array, &weak}) {
        samples->print(options);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: gc_bench [--objects N] [--iterations N]\n"
                     "Defaults: 1000 objects, 5 iterations. Output: CSV.\n"
                     "Limits: 1000000 objects, 200 iterations, 10000000 object-iterations.\n"
                     "Reports rooted allocation, retained/reclaimed graph collection, arrays, and weak handles.\n";
        return 0;
    }
    try {
        const auto options = parse_options(argc, argv);
        run(options);
    } catch (const UsageError& error) {
        std::cerr << "gc_bench: " << error.what() << "\nUse --help for usage.\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "gc_bench failed: " << error.what() << '\n';
        return 1;
    }
}
