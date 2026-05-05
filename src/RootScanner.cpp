#include "gc/RootScanner.hpp"

#include <algorithm>
#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace gc {
namespace {

void scan_words(const void* begin,
                const void* end,
                const RootScanner::CandidateVisitor& visitor) {
    auto start = reinterpret_cast<std::uintptr_t>(begin);
    auto finish = reinterpret_cast<std::uintptr_t>(end);

    if (start > finish) {
        std::swap(start, finish);
    }

    const std::uintptr_t word_size = sizeof(std::uintptr_t);
    start = (start + word_size - 1U) & ~(word_size - 1U);

    for (std::uintptr_t current = start; current + word_size <= finish; current += word_size) {
        visitor(*reinterpret_cast<const std::uintptr_t*>(current));
    }
}

void scan_registers(const RootScanner::CandidateVisitor& visitor) {
    std::jmp_buf registers;
    std::memset(&registers, 0, sizeof(registers));
    if (setjmp(registers) == 0) {
        const auto* end = reinterpret_cast<const unsigned char*>(&registers) + sizeof(registers);
        scan_words(&registers, end, visitor);
    }
}

void scan_stack(const void* stack_bottom, const RootScanner::CandidateVisitor& visitor) {
    if (stack_bottom == nullptr) {
        return;
    }

    std::uintptr_t local_marker = 0;
    const void* stack_top = &local_marker;

#if defined(__GNUC__) || defined(__clang__)
    if (void* frame = __builtin_frame_address(0)) {
        stack_top = frame;
    }
#endif

    scan_words(stack_top, stack_bottom, visitor);
}

#if defined(__linux__)
std::string trim_left(std::string value) {
    const auto first = value.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }

    value.erase(0, first);
    return value;
}

bool should_scan_linux_mapping(const std::string& permissions, const std::string& path) {
    if (permissions.size() < 2 || permissions[0] != 'r' || permissions[1] != 'w') {
        return false;
    }

    if (path.empty()) {
        return false;
    }

    if (path == "[heap]" || path == "[stack]" || path == "[vdso]" ||
        path == "[vvar]" || path == "[vsyscall]") {
        return false;
    }

    // Scanning the process heap would treat GC bookkeeping as roots and pin everything.
    return path.front() != '[';
}

void scan_linux_globals(const RootScanner::CandidateVisitor& visitor) {
    std::ifstream maps("/proc/self/maps");
    if (!maps.is_open()) {
        return;
    }

    std::string line;

    while (std::getline(maps, line)) {
        std::istringstream stream(line);
        std::string address_range;
        std::string permissions;
        std::string offset;
        std::string device;
        std::string inode;

        if (!(stream >> address_range >> permissions >> offset >> device >> inode)) {
            continue;
        }

        std::string path;
        std::getline(stream, path);
        path = trim_left(path);

        if (!should_scan_linux_mapping(permissions, path)) {
            continue;
        }

        const auto separator = address_range.find('-');
        if (separator == std::string::npos) {
            continue;
        }

        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;

        try {
            begin = static_cast<std::uintptr_t>(
                std::stoull(address_range.substr(0, separator), nullptr, 16));
            end = static_cast<std::uintptr_t>(
                std::stoull(address_range.substr(separator + 1), nullptr, 16));
        } catch (...) {
            continue;
        }

        scan_words(reinterpret_cast<const void*>(begin), reinterpret_cast<const void*>(end), visitor);
    }
}
#endif

}  // namespace

void RootScanner::scan_range(const void* begin,
                             const void* end,
                             const CandidateVisitor& visitor) {
    scan_words(begin, end, visitor);
}

void RootScanner::scan(const void* stack_bottom, const CandidateVisitor& visitor) {
    scan_registers(visitor);
    scan_stack(stack_bottom, visitor);

#if defined(__linux__)
    scan_linux_globals(visitor);
#else
    (void)stack_bottom;
#endif
}

}  // namespace gc