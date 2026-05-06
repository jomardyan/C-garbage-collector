#pragma once

#include <cstdint>
#include <functional>

namespace gc {

class RootScanner {
public:
    using CandidateVisitor = std::function<void(std::uintptr_t)>;

    static void* current_fake_stack_handle() noexcept;
    static void scan(const void* stack_bottom, const CandidateVisitor& visitor);
    static void scan_range(const void* begin,
                           const void* end,
                           const CandidateVisitor& visitor);
    static void scan_thread_stack(const void* stack_top,
                                  const void* stack_bottom,
                                  void* fake_stack_handle,
                                  const CandidateVisitor& visitor);
};

}  // namespace gc