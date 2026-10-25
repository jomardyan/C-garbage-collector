#include "gc/RootScanner.hpp"

#include <algorithm>
#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GC_HAS_ASAN 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define GC_HAS_ASAN 1
#endif

#if defined(GC_HAS_ASAN)
#include <sanitizer/asan_interface.h>
#endif

#if defined(__linux__)
#include <link.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
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

// A conservative scan intentionally reads opaque words, including writable
// runtime globals which can change outside the registered mutator threads.
// Instrument application access normally; exclude only these raw root reads
// from TSan's C++ object-access model.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((no_sanitize_thread))
#endif
void scan_words(const void* begin,
                const void* end,
                const RootScanner::CandidateVisitor& visitor) {
    if (begin == nullptr || end == nullptr) {
        return;
    }

    auto start = reinterpret_cast<std::uintptr_t>(begin);
    auto finish = reinterpret_cast<std::uintptr_t>(end);

    if (start > finish) {
        std::swap(start, finish);
    }

    const std::uintptr_t word_size = sizeof(std::uintptr_t);
    const auto padding = (word_size - start % word_size) % word_size;
    if (finish - start < padding) {
        return;
    }
    start += padding;

    // Subtraction avoids wrapping at the top of the address space, both when
    // aligning the start and when deciding whether a complete word remains.
    for (std::uintptr_t cur = start; finish - cur >= word_size; cur += word_size) {
        const void* word_address = reinterpret_cast<const void*>(cur);
#if defined(GC_HAS_ASAN)
        if (__asan_region_is_poisoned(const_cast<void*>(word_address), word_size) != nullptr) {
            continue;
        }
#endif

        std::uintptr_t candidate = 0U;
        std::memcpy(&candidate, word_address, sizeof(candidate));
        visitor(candidate);
    }
}

// setjmp captures the callee-saved registers on supported ABIs. Caller-saved
// registers do not reliably retain roots across calls; use gc_root for exact
// local rooting in optimized code.
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

    std::jmp_buf registers;
    std::memset(&registers, 0, sizeof(registers));
#if defined(_MSC_VER)
    // Capture register roots only; this buffer is never passed to longjmp.
#pragma warning(suppress : 4611)
#endif
    if (setjmp(registers) == 0) {
        const auto* end =
            reinterpret_cast<const unsigned char*>(&registers) + sizeof(registers);
        scan_words(&registers, end, visitor);
    }
}

void scan_registers(const RootScanner::CandidateVisitor& visitor) {
    scan_registers_frame(visitor, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U);
}

void scan_stack_range(const void* stack_top,
                      const void* stack_bottom,
                      void* fake_stack_handle,
                      const RootScanner::CandidateVisitor& visitor) {
    if (stack_top == nullptr || stack_bottom == nullptr) {
        return;
    }

#if defined(GC_HAS_ASAN)
    if (fake_stack_handle != nullptr) {
        void* fake_frame_begin = nullptr;
        void* fake_frame_end = nullptr;
        if (void* real_stack_top = __asan_addr_is_in_fake_stack(
                fake_stack_handle, const_cast<void*>(stack_top),
                &fake_frame_begin, &fake_frame_end)) {
            scan_words(fake_frame_begin, fake_frame_end, visitor);
            stack_top = real_stack_top;
        }
    }
#else
    (void)fake_stack_handle;
#endif

    scan_words(stack_top, stack_bottom, visitor);
}

// ---- Platform-specific global/data-segment scanning -------------------------

#if defined(__linux__)

// ELF segment sizes include anonymous zero-filled BSS tails, which cannot be
// associated with their image by looking only at named /proc/self/maps lines.
// The loader also keeps images mapped while dl_iterate_phdr's callback runs.
struct LinuxScanContext {
    const RootScanner::CandidateVisitor& visitor;
    std::exception_ptr error;
};

int scan_linux_image(dl_phdr_info* image, std::size_t, void* opaque) noexcept {
    auto& context = *static_cast<LinuxScanContext*>(opaque);
    try {
        for (std::size_t i = 0; i < image->dlpi_phnum; ++i) {
            const auto& segment = image->dlpi_phdr[i];
            if (segment.p_type != PT_LOAD ||
                (segment.p_flags & (PF_R | PF_W)) != (PF_R | PF_W) ||
                segment.p_memsz == 0) {
                continue;
            }
            const auto begin = static_cast<std::uintptr_t>(image->dlpi_addr) +
                               static_cast<std::uintptr_t>(segment.p_vaddr);
            if (segment.p_memsz > (std::numeric_limits<std::uintptr_t>::max)() - begin) {
                continue;
            }
            scan_words(reinterpret_cast<const void*>(begin),
                       reinterpret_cast<const void*>(begin + segment.p_memsz),
                       context.visitor);
        }
    } catch (...) {
        // Never unwind a throwing visitor through the loader's C callback.
        context.error = std::current_exception();
        return 1;
    }
    return 0;
}

void scan_globals(const RootScanner::CandidateVisitor& visitor) {
    LinuxScanContext context{visitor, {}};
    dl_iterate_phdr(scan_linux_image, &context);
    if (context.error) {
        std::rethrow_exception(context.error);
    }
}

#elif defined(__APPLE__)

// Scan __DATA and __DATA_CONST segments of every loaded Mach-O image.
// getsectiondata() computes the image slide from __TEXT and returns the runtime
// address. Applying _dyld_get_image_vmaddr_slide() again would double-slide it.
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
            unsigned long sz = 0U;
            const uint8_t* ptr = getsectiondata(mh, s.seg, s.sect, &sz);
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
            (mbi.Protect & PAGE_WRITECOPY) != 0U ||
            (mbi.Protect & PAGE_EXECUTE_READWRITE) != 0U ||
            (mbi.Protect & PAGE_EXECUTE_WRITECOPY) != 0U;

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
        if (next <= reinterpret_cast<std::uintptr_t>(mbi.BaseAddress)) {
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

void* RootScanner::current_fake_stack_handle() noexcept {
#if defined(GC_HAS_ASAN)
    return __asan_get_current_fake_stack();
#else
    return nullptr;
#endif
}

void RootScanner::scan_range(const void* begin,
                             const void* end,
                             const CandidateVisitor& visitor) {
    scan_words(begin, end, visitor);
}

void RootScanner::scan_thread_stack(const void* stack_top,
                                    const void* stack_bottom,
                                    void* fake_stack_handle,
                                    const CandidateVisitor& visitor) {
    scan_stack_range(stack_top, stack_bottom, fake_stack_handle, visitor);
}

void RootScanner::scan(const void* stack_bottom,
                       const CandidateVisitor& visitor) {
    std::uintptr_t local_marker = 0;

    scan_registers(visitor);
    scan_stack_range(&local_marker, stack_bottom, current_fake_stack_handle(), visitor);
    scan_globals(visitor);
}

}  // namespace gc
