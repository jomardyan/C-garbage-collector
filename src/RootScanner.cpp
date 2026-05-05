#include "gc/RootScanner.hpp"

#include <algorithm>
#include <csetjmp>
#include <cstdint>
#include <cstring>

#if defined(__linux__)
#include <fstream>
#include <sstream>
#include <string>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace gc {
namespace {

#if defined(_MSC_VER)
#define GC_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define GC_NOINLINE __attribute__((noinline))
#else
#define GC_NOINLINE
#endif

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

    for (std::uintptr_t cur = start; cur + word_size <= finish; cur += word_size) {
        visitor(*reinterpret_cast<const std::uintptr_t*>(cur));
    }
}

// setjmp flushes caller-saved registers into the jmp_buf on most ABIs (x86-64,
// AArch64, RISC-V).  On some ABIs a register window may be missed; the
// conservative stack scan below provides a second line of defence.
void clear_transient_registers() noexcept {
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
    __asm__ __volatile__(
        "xor %%rax, %%rax\n\t"
        "xor %%rcx, %%rcx\n\t"
        "xor %%rdx, %%rdx\n\t"
        "xor %%rsi, %%rsi\n\t"
        "xor %%r8, %%r8\n\t"
        "xor %%r9, %%r9\n\t"
        "xor %%r10, %%r10\n\t"
        "xor %%r11, %%r11\n\t"
        :
        :
        : "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "memory");
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__aarch64__)
    __asm__ __volatile__(
        "mov x1, xzr\n\t"
        "mov x2, xzr\n\t"
        "mov x3, xzr\n\t"
        "mov x4, xzr\n\t"
        "mov x5, xzr\n\t"
        "mov x6, xzr\n\t"
        "mov x7, xzr\n\t"
        "mov x8, xzr\n\t"
        "mov x9, xzr\n\t"
        "mov x10, xzr\n\t"
        "mov x11, xzr\n\t"
        "mov x12, xzr\n\t"
        "mov x13, xzr\n\t"
        "mov x14, xzr\n\t"
        "mov x15, xzr\n\t"
        "mov x16, xzr\n\t"
        "mov x17, xzr\n\t"
        :
        :
        : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
          "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
#endif
}

GC_NOINLINE void scan_registers_frame(
    const RootScanner::CandidateVisitor& visitor,
    std::uintptr_t zero0,
    std::uintptr_t zero1,
    std::uintptr_t zero2,
    std::uintptr_t zero3,
    std::uintptr_t zero4,
    std::uintptr_t zero5,
    std::uintptr_t zero6,
    std::uintptr_t zero7) {
    (void)zero0;
    (void)zero1;
    (void)zero2;
    (void)zero3;
    (void)zero4;
    (void)zero5;
    (void)zero6;
    (void)zero7;

    clear_transient_registers();

    std::jmp_buf registers;
    std::memset(&registers, 0, sizeof(registers));
    if (setjmp(registers) == 0) {
        const auto* end =
            reinterpret_cast<const unsigned char*>(&registers) + sizeof(registers);
        scan_words(&registers, end, visitor);
    }
}

void scan_registers(const RootScanner::CandidateVisitor& visitor) {
    scan_registers_frame(visitor, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U);
}

void scan_stack(const void* stack_bottom,
                const RootScanner::CandidateVisitor& visitor) {
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

// ---- Platform-specific global/data-segment scanning -------------------------

#if defined(__linux__)

namespace {

std::string trim_left(std::string value) {
    const auto first = value.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    value.erase(0, first);
    return value;
}

bool should_scan_linux_mapping(const std::string& permissions,
                               const std::string& path) {
    if (permissions.size() < 2 || permissions[0] != 'r' || permissions[1] != 'w') {
        return false;
    }
    if (path.empty()) {
        return false;
    }
    // Skip kernel-managed pseudo-regions and the process heap (scanning the
    // heap would treat GC bookkeeping as roots and pin everything).
    if (path == "[heap]" || path == "[stack]" || path == "[vdso]" ||
        path == "[vvar]" || path == "[vsyscall]") {
        return false;
    }
    return path.front() != '[';
}

}  // namespace

void scan_globals(const RootScanner::CandidateVisitor& visitor) {
    std::ifstream maps("/proc/self/maps");
    if (!maps.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream stream(line);
        std::string address_range, permissions, offset, device, inode;

        if (!(stream >> address_range >> permissions >> offset >> device >> inode)) {
            continue;
        }

        std::string path;
        std::getline(stream, path);
        path = trim_left(path);

        if (!should_scan_linux_mapping(permissions, path)) {
            continue;
        }

        const auto sep = address_range.find('-');
        if (sep == std::string::npos) {
            continue;
        }

        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        try {
            begin = static_cast<std::uintptr_t>(
                std::stoull(address_range.substr(0, sep), nullptr, 16));
            end = static_cast<std::uintptr_t>(
                std::stoull(address_range.substr(sep + 1), nullptr, 16));
        } catch (...) {
            continue;
        }

        scan_words(reinterpret_cast<const void*>(begin),
                   reinterpret_cast<const void*>(end),
                   visitor);
    }
}

#elif defined(__APPLE__)

// Scan __DATA and __DATA_CONST segments of every loaded Mach-O image.
// getsectiondata() returns the already-slid (runtime) address, so no manual
// ASLR correction is needed.
void scan_globals(const RootScanner::CandidateVisitor& visitor) {
    const uint32_t image_count = _dyld_image_count();

    static constexpr struct {
        const char* seg;
        const char* sect;
    } kSections[] = {
        {"__DATA",       "__data"},
        {"__DATA",       "__bss"},
        {"__DATA",       "__common"},
        {"__DATA_CONST", "__const"},
    };

    for (uint32_t i = 0U; i < image_count; ++i) {
        const auto* raw_header = _dyld_get_image_header(i);
        if (raw_header == nullptr) {
            continue;
        }

#if defined(__LP64__)
        const auto* mh = reinterpret_cast<const mach_header_64*>(raw_header);
        if (mh->magic != MH_MAGIC_64) {
            continue;
        }
        for (const auto& s : kSections) {
            unsigned long sz = 0U;
            const uint8_t* ptr = getsectiondata(mh, s.seg, s.sect, &sz);
            if (ptr != nullptr && sz > 0U) {
                scan_words(ptr, ptr + sz, visitor);
            }
        }
#else
        const auto* mh = reinterpret_cast<const mach_header*>(raw_header);
        if (mh->magic != MH_MAGIC) {
            continue;
        }
        for (const auto& s : kSections) {
            uint32_t sz = 0U;
            const uint8_t* ptr =
                getsectiondata(reinterpret_cast<const mach_header_64*>(mh),
                               s.seg, s.sect, reinterpret_cast<unsigned long*>(&sz));
            if (ptr != nullptr && sz > 0U) {
                scan_words(ptr, ptr + sz, visitor);
            }
        }
#endif
    }
}

#elif defined(_WIN32)

// Scan committed read-write IMAGE sections (PE .data / .bss).
// MEM_IMAGE restricts scanning to file-backed sections, excluding the heap.
void scan_globals(const RootScanner::CandidateVisitor& visitor) {
    LPVOID address = nullptr;
    MEMORY_BASIC_INFORMATION mbi;

    while (VirtualQuery(address, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const bool readable_writable =
            (mbi.Protect & PAGE_READWRITE) != 0U ||
            (mbi.Protect & PAGE_WRITECOPY) != 0U;

        if (mbi.State == MEM_COMMIT &&
            readable_writable &&
            (mbi.Protect & PAGE_GUARD) == 0U &&
            mbi.Type == MEM_IMAGE) {
            scan_words(mbi.BaseAddress,
                       static_cast<char*>(mbi.BaseAddress) + mbi.RegionSize,
                       visitor);
        }

        const auto next =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next == 0U) {
            break;  // wrapped around top of address space
        }
        address = reinterpret_cast<LPVOID>(next);
    }
}

#else

void scan_globals(const RootScanner::CandidateVisitor& /*visitor*/) {
    // No implementation for this platform; register explicit root ranges to
    // keep global gc_ptrs alive.
}

#endif

}  // namespace

void RootScanner::scan_range(const void* begin,
                             const void* end,
                             const CandidateVisitor& visitor) {
    scan_words(begin, end, visitor);
}

void RootScanner::scan(const void* stack_bottom,
                       const CandidateVisitor& visitor) {
    scan_registers(visitor);
    scan_stack(stack_bottom, visitor);
    scan_globals(visitor);
}

}  // namespace gc
