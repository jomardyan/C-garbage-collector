#include "gc/GC.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// The driver deliberately remains unregistered. Each operation gets a separate
// registered stack, so expired builder/observer locals cannot become accidental
// strong roots in the next collection. Explicit roots live in ordinary heap
// storage, and observing handles are weak.
template <typename Action>
void on_registered_thread(Action action) {
    std::exception_ptr error;
    std::thread worker([&] {
        try {
            gc::ScopedThreadRegistration registration;
            action();
        } catch (...) {
            error = std::current_exception();
        }
    });
    worker.join();
    if (error) std::rethrow_exception(error);
}

void collect_without_builder_stack() {
    on_registered_thread([] { gc::collect(); });
}

struct GraphNode {
    static inline std::vector<unsigned> destructions;
    std::array<gc::gc_ptr<GraphNode>, 3> edges{};
    std::array<unsigned char, 17> bytes{};
    std::size_t id;

    explicit GraphNode(std::size_t index) : id(index) {}
    ~GraphNode() { ++destructions.at(id); }
};

using Edges = std::array<int, 3>;

struct GraphFixture {
    std::vector<Edges> model;
    std::vector<int> root_ids;
    std::vector<std::unique_ptr<gc::gc_root<GraphNode>>> roots;
    std::vector<gc::gc_weak_ptr<GraphNode>> observations;
};

std::vector<bool> reachable_nodes(const GraphFixture& graph) {
    std::vector<bool> reachable(graph.model.size(), false);
    std::vector<std::size_t> pending;
    for (int root : graph.root_ids) {
        if (root >= 0) pending.push_back(static_cast<std::size_t>(root));
    }
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (reachable[id]) continue;
        reachable[id] = true;
        for (int target : graph.model[id]) {
            if (target >= 0) pending.push_back(static_cast<std::size_t>(target));
        }
    }
    return reachable;
}

void build_graph(GraphFixture& graph) {
    GraphNode::destructions.assign(graph.model.size(), 0);
    graph.observations.reserve(graph.model.size());
    std::vector<gc::gc_root<GraphNode>> construction_roots;
    construction_roots.reserve(graph.model.size());
    for (std::size_t id = 0; id < graph.model.size(); ++id) {
        construction_roots.emplace_back(gc::gc_new<GraphNode>(id));
        graph.observations.emplace_back(construction_roots.back().as_gc_ptr());
    }
    for (std::size_t id = 0; id < graph.model.size(); ++id) {
        for (std::size_t slot = 0; slot < graph.model[id].size(); ++slot) {
            const int target = graph.model[id][slot];
            if (target >= 0) {
                construction_roots[id]->edges[slot] =
                    construction_roots[static_cast<std::size_t>(target)].as_gc_ptr();
            }
        }
    }
    for (int id : graph.root_ids) {
        graph.roots.push_back(std::make_unique<gc::gc_root<GraphNode>>(
            construction_roots[static_cast<std::size_t>(id)].as_gc_ptr()));
    }
}

void check_model(const GraphFixture& graph, std::uint32_t seed, const char* stage) {
    const auto expected = reachable_nodes(graph);
    std::size_t expected_count = 0;
    for (std::size_t id = 0; id < expected.size(); ++id) {
        const std::string context = "seed=" + std::to_string(seed) + " stage=" + stage +
                                    " node=" + std::to_string(id);
        expect(graph.observations[id].expired() != expected[id],
               context + ": weak lifetime differs from model reachability");
        expect(GraphNode::destructions[id] == (expected[id] ? 0U : 1U),
               context + ": each unreachable node must finalize exactly once");
        if (!expected[id]) continue;
        ++expected_count;
        const auto node = graph.observations[id].lock();
        expect(node && node->id == id, context + ": retained payload must remain readable");
        for (std::size_t slot = 0; slot < graph.model[id].size(); ++slot) {
            const int target = graph.model[id][slot];
            if (target < 0) {
                expect(!node->edges[slot], context + ": absent edge must stay null");
            } else {
                expect(node->edges[slot] &&
                           node->edges[slot]->id == static_cast<std::size_t>(target),
                       context + ": reachable outgoing edge must remain readable");
            }
        }
    }
    expect(gc::GC_Manager::instance().managed_block_count() == expected_count,
           "live block count must equal model reachability");
}

void test_seeded_graphs() {
    constexpr std::array<std::uint32_t, 8> seeds{
        1U, 7U, 42U, 101U, 1024U, 65537U, 0xCAFEU, 0xDEADBEEFU};
    constexpr std::size_t count = 64;
    for (const auto seed : seeds) {
        gc::shutdown();
        auto graph = std::make_unique<GraphFixture>();
        graph->model.resize(count);
        graph->root_ids = {0, 9, 23};
        std::mt19937 random(seed);
        for (std::size_t id = 0; id < count; ++id) {
            // Separate components guarantee both live and dead subgraphs. Sparse
            // random edges add trees, sharing, self references and further cycles.
            const std::size_t component_begin = id < count / 2 ? 0 : count / 2;
            for (auto& target : graph->model[id]) {
                target = random() % 3 == 0
                             ? -1
                             : static_cast<int>(component_begin + random() % (count / 2));
            }
        }
        graph->model[0][0] = 1;
        graph->model[1][0] = 0;
        graph->model[count / 2][0] = static_cast<int>(count / 2 + 1);
        graph->model[count / 2 + 1][0] = static_cast<int>(count / 2);

        on_registered_thread([&] { build_graph(*graph); });
        collect_without_builder_stack();
        on_registered_thread([&] { check_model(*graph, seed, "initial roots"); });

        on_registered_thread([&] {
            graph->roots[1]->reset();
            graph->root_ids[1] = -1;
        });
        collect_without_builder_stack();
        on_registered_thread([&] { check_model(*graph, seed, "one root removed"); });

        on_registered_thread([&] {
            graph->roots.clear();
            graph->root_ids.clear();
        });
        collect_without_builder_stack();
        on_registered_thread([&] { check_model(*graph, seed, "all roots removed"); });
    }
    gc::shutdown();
}

void test_odd_interior_root_retains_graph() {
    gc::shutdown();
    auto graph = std::make_unique<GraphFixture>();
    graph->model = {{1, -1, -1}, {2, -1, -1}, {0, -1, -1}};
    graph->root_ids = {0};
    std::unique_ptr<gc::gc_root<unsigned char>> interior;
    on_registered_thread([&] {
        build_graph(*graph);
        auto* odd_address = graph->roots[0]->get()->bytes.data() + 1;
        expect(reinterpret_cast<std::uintptr_t>(odd_address) % 2 == 1,
               "the interior root fixture must use an odd address");
        *odd_address = 73;
        interior = std::make_unique<gc::gc_root<unsigned char>>(
            gc::gc_ptr<unsigned char>(odd_address));
        graph->roots.clear();
    });
    collect_without_builder_stack();
    on_registered_thread([&] {
        check_model(*graph, 0, "odd interior root");
        expect(**interior == 73, "the odd interior pointer must retain its byte value");
        interior.reset();
        graph->root_ids.clear();
    });
    collect_without_builder_stack();
    on_registered_thread([&] { check_model(*graph, 0, "interior root removed"); });
    gc::shutdown();
}

void test_managed_array_traces_outgoing_handles() {
    gc::shutdown();
    auto graph = std::make_unique<GraphFixture>();
    graph->model = {{1, -1, -1}, {0, -1, -1}, {-1, -1, -1}};
    graph->root_ids = {0};
    std::unique_ptr<gc::gc_root<gc::gc_ptr<GraphNode>[]>> array;
    on_registered_thread([&] {
        build_graph(*graph);
        array = std::make_unique<gc::gc_root<gc::gc_ptr<GraphNode>[]>>(
            gc::gc_new_array<gc::gc_ptr<GraphNode>>(3));
        (*array)[0] = graph->roots[0]->as_gc_ptr();
        graph->roots.clear();
    });
    collect_without_builder_stack();
    on_registered_thread([&] {
        expect(gc::GC_Manager::instance().managed_block_count() == 3,
               "the rooted array and its two-node cycle must survive");
        expect(!graph->observations[0].expired() && !graph->observations[1].expired() &&
                   graph->observations[2].expired(),
               "array elements must trace their targets without retaining unrelated nodes");
        expect((*array)[0]->edges[0]->id == 1 && !(*array)[1] && !(*array)[2],
               "managed array handle elements must remain usable and value initialized");
        (*array)[0].reset();
        graph->root_ids.clear();
    });
    collect_without_builder_stack();
    on_registered_thread([&] {
        expect(gc::GC_Manager::instance().managed_block_count() == 1,
               "clearing the array element must release its formerly reachable cycle");
        for (std::size_t id = 0; id < graph->observations.size(); ++id) {
            expect(graph->observations[id].expired() && GraphNode::destructions[id] == 1,
                   "every graph node must finalize once after the array drops its edges");
        }
        array.reset();
    });
    collect_without_builder_stack();
    expect(gc::GC_Manager::instance().managed_block_count() == 0,
           "the managed array must be collectible after its root disappears");
    gc::shutdown();
}

void test_deep_graph_uses_iterative_tracing() {
    gc::shutdown();
    constexpr std::size_t depth = 4096;
    auto graph = std::make_unique<GraphFixture>();
    graph->model.resize(depth, Edges{-1, -1, -1});
    graph->root_ids = {0};
    for (std::size_t id = 0; id + 1 < depth; ++id) {
        graph->model[id][0] = static_cast<int>(id + 1);
    }
    graph->model.back()[0] = 0;
    on_registered_thread([&] { build_graph(*graph); });
    collect_without_builder_stack();
    on_registered_thread([&] {
        check_model(*graph, 0, "deep cycle");
        graph->roots.clear();
        graph->root_ids.clear();
    });
    collect_without_builder_stack();
    on_registered_thread([&] { check_model(*graph, 0, "deep cycle released"); });
    gc::shutdown();
}

}  // namespace

int main() {
    try {
        gc::GC_Manager::instance().set_collection_threshold_bytes(
            (std::numeric_limits<std::size_t>::max)());
        test_seeded_graphs();
        test_odd_interior_root_retains_graph();
        test_managed_array_traces_outgoing_handles();
        test_deep_graph_uses_iterative_tracing();
        std::cout << "[PASS] 8 seeded graph models, interior roots, managed arrays, deep cycle\n";
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] graph property test: " << error.what() << '\n';
        return 1;
    }
}
