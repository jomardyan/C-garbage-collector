#pragma once

#include <cstdint>
#include <functional>

namespace gc {

class RootScanner {
public:
    using CandidateVisitor = std::function<void(std::uintptr_t)>;

    static void scan(const void* stack_bottom, const CandidateVisitor& visitor);
    static void scan_range(const void* begin,
                           const void* end,
                           const CandidateVisitor& visitor);
};

}  // namespace gc