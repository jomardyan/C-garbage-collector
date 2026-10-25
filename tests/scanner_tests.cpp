#include "gc/RootScanner.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

// This intentionally extends beyond the file-backed portion of an executable's
// BSS. The old /proc/self/maps scanner missed roots in the anonymous tail.
alignas(4096) volatile std::uintptr_t large_bss[32768];
constexpr std::uintptr_t bss_marker = 0x47434C4242535355ULL;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_ranges() {
    std::array<std::uintptr_t, 3> words{17, 29, 43};
    std::vector<std::uintptr_t> seen;
    auto visit = [&seen](std::uintptr_t word) { seen.push_back(word); };
    gc::RootScanner::scan_range(words.data(), words.data() + words.size(), visit);
    expect(seen == std::vector<std::uintptr_t>({17, 29, 43}),
           "scanner should visit every complete aligned word");

    seen.clear();
    gc::RootScanner::scan_range(words.data() + words.size(), words.data(), visit);
    expect(seen == std::vector<std::uintptr_t>({17, 29, 43}),
           "reversed range endpoints should be normalized");

    seen.clear();
    const auto* bytes = reinterpret_cast<const unsigned char*>(words.data());
    gc::RootScanner::scan_range(bytes + 1, bytes + sizeof(words) - 1, visit);
    expect(seen == std::vector<std::uintptr_t>({29}),
           "scanner should skip incomplete words at both endpoints");

    seen.clear();
    gc::RootScanner::scan_range(nullptr, nullptr, visit);
    gc::RootScanner::scan_range(nullptr, words.data(), visit);
    gc::RootScanner::scan_range(words.data(), nullptr, visit);
    gc::RootScanner::scan_range(words.data(), words.data(), visit);
    const auto limit = std::numeric_limits<std::uintptr_t>::max();
    gc::RootScanner::scan_range(reinterpret_cast<const void*>(limit - 1),
                                reinterpret_cast<const void*>(limit), visit);
    expect(seen.empty(), "empty and partial ranges should perform no reads");
}

void test_global_bss() {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    // Write in another thread so the marker is not left in this thread's saved
    // registers. Its only data storage here is the final word in large_bss.
    std::thread([] { large_bss[32767] = bss_marker; }).join();
    bool found = false;
    gc::RootScanner::scan(nullptr, [&found](std::uintptr_t candidate) {
        if (candidate == bss_marker) {
            found = true;
        }
    });
    expect(found, "global scanning must cover the anonymous tail of large BSS");
    large_bss[32767] = 0;
#endif
}

}  // namespace

int main() {
    try {
        test_ranges();
        test_global_bss();
        std::cout << "Root scanner tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
