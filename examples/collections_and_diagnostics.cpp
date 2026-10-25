#include <cstddef>
#include <iostream>
#include <string>
#include <utility>

#include "gc/GC.hpp"

namespace {
struct Sample {
    Sample(std::string name_value, int reading_value)
        : name(std::move(name_value)), reading(reading_value) {}
    std::string name;
    int reading;
};
}  // namespace

int main() {
    gc::ScopedThreadRegistration registration;
    gc::gc_vector<Sample> samples;
    samples.emplace_new("temperature", 21);
    samples.emplace_new("humidity", 48);
    gc::gc_root<int[]> squares(gc::gc_generate_array<int>(8, [](std::size_t index) {
        return static_cast<int>(index * index);
    }));
    gc::gc_root<int[]> defaults(gc::gc_new_array<int>(4, -1));
    gc::collect();

    for (const auto& sample : samples) {
        std::cout << sample->name << ": " << sample->reading << '\n';
    }
    const auto info = gc::GC_Manager::instance().object_info(&squares[3]);
    const auto stats = gc::GC_Manager::instance().stats();
    if (!info || squares[7] != 49 || defaults[3] != -1 || stats.total_allocations != 4) return 1;
    std::cout << "Generated array allocation: " << info->payload_size << " payload bytes\n"
              << "Allocations: " << stats.total_allocations
              << ", peak payload bytes: " << stats.peak_live_bytes << '\n';
    samples.clear();
    squares.reset();
    defaults.reset();
    gc::shutdown();
}
