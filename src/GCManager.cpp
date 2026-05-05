#include "gc/GCManager.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>

#include "gc/RootScanner.hpp"

namespace gc {
namespace {

std::size_t normalize_alignment(std::size_t alignment) noexcept {
    return std::max(alignment, alignof(std::max_align_t));
}

bool add_would_overflow(std::size_t lhs, std::size_t rhs) noexcept {
    return lhs > std::numeric_limits<std::size_t>::max() - rhs;
}

std::size_t round_up_to_alignment(std::size_t value, std::size_t alignment) {
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) {
        throw std::bad_alloc();
    }

    if (add_would_overflow(value, alignment - 1U)) {
        throw std::bad_alloc();
    }

    return (value + alignment - 1U) & ~(alignment - 1U);
}

std::uintptr_t payload_begin(const ChunkHeader* header) noexcept {
    const auto* base = reinterpret_cast<const unsigned char*>(header);
    return reinterpret_cast<std::uintptr_t>(base + header->payload_offset);
}

void* payload_pointer(ChunkHeader* header) noexcept {
    auto* base = reinterpret_cast<unsigned char*>(header);
    return static_cast<void*>(base + header->payload_offset);
}

void free_block_memory(ChunkHeader* header) noexcept {
    ::operator delete(static_cast<void*>(header),
                      std::align_val_t(header->allocation_alignment()));
}

}  // namespace

GC_Manager& GC_Manager::instance() {
    static GC_Manager manager;
    return manager;
}

GC_Manager::~GC_Manager() {
    shutdown();
}

void GC_Manager::register_stack_bottom(const void* stack_bottom) {
    std::lock_guard<std::mutex> lock(mutex_);
    bind_or_reject_thread_locked();
    stack_bottom_ = stack_bottom;
}

void GC_Manager::register_root_range(const void* begin, const void* end) {
    if (begin == nullptr || end == nullptr || begin == end) {
        return;
    }

    auto start = reinterpret_cast<std::uintptr_t>(begin);
    auto finish = reinterpret_cast<std::uintptr_t>(end);
    if (start > finish) {
        std::swap(start, finish);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    bind_or_reject_thread_locked();
    registered_root_ranges_.push_back(
        RootRange{reinterpret_cast<const void*>(start), reinterpret_cast<const void*>(finish)});
}

void GC_Manager::unregister_root_range(const void* begin, const void* end) {
    if (begin == nullptr || end == nullptr || begin == end) {
        return;
    }

    auto start = reinterpret_cast<std::uintptr_t>(begin);
    auto finish = reinterpret_cast<std::uintptr_t>(end);
    if (start > finish) {
        std::swap(start, finish);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    bind_or_reject_thread_locked();
    const auto* normalized_begin = reinterpret_cast<const void*>(start);
    const auto* normalized_end = reinterpret_cast<const void*>(finish);

    registered_root_ranges_.erase(
        std::remove_if(registered_root_ranges_.begin(),
                       registered_root_ranges_.end(),
                       [normalized_begin, normalized_end](const RootRange& range) {
                           return range.begin == normalized_begin && range.end == normalized_end;
                       }),
        registered_root_ranges_.end());
}

void GC_Manager::set_collection_threshold_bytes(std::size_t threshold_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    bind_or_reject_thread_locked();
    collection_threshold_bytes_ = threshold_bytes;
}

std::size_t GC_Manager::collection_threshold_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return collection_threshold_bytes_;
}

std::size_t GC_Manager::managed_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return managed_bytes_;
}

std::size_t GC_Manager::managed_block_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocations_.size();
}

void* GC_Manager::malloc_internal(std::size_t payload_size,
                                  std::size_t payload_alignment,
                                  DestructorFn destructor) {
    const std::size_t alignment = normalize_alignment(payload_alignment);
    const std::size_t payload_offset = round_up_to_alignment(sizeof(ChunkHeader), alignment);

    if (payload_offset > std::numeric_limits<std::uint16_t>::max() ||
        add_would_overflow(payload_offset, payload_size)) {
        throw std::bad_alloc();
    }

    const std::size_t total_size = payload_offset + payload_size;
    void* raw = nullptr;

    try {
        raw = ::operator new(total_size, std::align_val_t(alignment));
    } catch (const std::bad_alloc&) {
        collect();
        raw = ::operator new(total_size, std::align_val_t(alignment));
    }

    auto* header = static_cast<ChunkHeader*>(raw);
    header->size = payload_size;
    header->destructor = destructor;
    header->magic_number = kChunkMagicNumber;
    header->payload_offset = static_cast<std::uint16_t>(payload_offset);
    header->set_marked(false);
    header->set_allocation_alignment(alignment);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        bind_or_reject_thread_locked();
        register_block_locked(header);
    }

    return payload_pointer(header);
}

void GC_Manager::release_unconstructed(void* payload) {
    if (payload == nullptr) {
        return;
    }

    ChunkHeader* header = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        bind_or_reject_thread_locked();
        const auto payload_address = reinterpret_cast<std::uintptr_t>(payload);
        auto it = allocations_.find(payload_address);
        if (it != allocations_.end()) {
            header = it->second.header;
            managed_bytes_ -= header->size;
            allocations_.erase(it);
        }
    }

    if (header != nullptr) {
        free_block_memory(header);
    }
}

void GC_Manager::collect_if_needed(std::size_t incoming_payload_bytes) {
    bool should_collect = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        bind_or_reject_thread_locked();
        const std::size_t projected_bytes =
            add_would_overflow(managed_bytes_, incoming_payload_bytes)
                ? std::numeric_limits<std::size_t>::max()
                : managed_bytes_ + incoming_payload_bytes;
        should_collect = !collecting_ && projected_bytes >= collection_threshold_bytes_;
    }

    if (should_collect) {
        collect();
    }
}

void GC_Manager::collect() {
    std::vector<ChunkHeader*> unreachable;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        bind_or_reject_thread_locked();
        if (collecting_) {
            return;
        }

        collecting_ = true;
        try {
            clear_marks_locked();
            std::vector<BlockRecord*> worklist;
            worklist.reserve(allocations_.size());

            RootScanner::scan(stack_bottom_, [this, &worklist](std::uintptr_t candidate) {
                mark_candidate_locked(candidate, worklist);
            });
            scan_registered_roots_locked(worklist);
            drain_mark_worklist_locked(worklist);
            unreachable = sweep_locked();
        } catch (...) {
            collecting_ = false;
            throw;
        }
    }

    destroy_blocks(unreachable);

    std::lock_guard<std::mutex> lock(mutex_);
    collecting_ = false;
}

void GC_Manager::shutdown() {
    std::vector<ChunkHeader*> blocks;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        bind_or_reject_thread_locked();
        if (collecting_) {
            return;
        }

        collecting_ = true;
        try {
            blocks = detach_all_blocks_locked();
        } catch (...) {
            collecting_ = false;
            throw;
        }
    }

    destroy_blocks(blocks);

    std::lock_guard<std::mutex> lock(mutex_);
    collecting_ = false;
}

GC_Manager::BlockRecord* GC_Manager::find_block_locked(std::uintptr_t address) noexcept {
    auto it = allocations_.upper_bound(address);
    if (it == allocations_.begin()) {
        return nullptr;
    }

    --it;
    if (address >= it->second.payload_begin && address < it->second.payload_end) {
        return &it->second;
    }

    return nullptr;
}

const GC_Manager::BlockRecord* GC_Manager::find_block_locked(std::uintptr_t address) const noexcept {
    auto it = allocations_.upper_bound(address);
    if (it == allocations_.begin()) {
        return nullptr;
    }

    --it;
    if (address >= it->second.payload_begin && address < it->second.payload_end) {
        return &it->second;
    }

    return nullptr;
}

void GC_Manager::register_block_locked(ChunkHeader* header) {
    const std::uintptr_t begin = payload_begin(header);
    allocations_.emplace(begin, BlockRecord{header, begin, begin + header->size});
    managed_bytes_ += header->size;
}

void GC_Manager::bind_or_reject_thread_locked() {
    const std::thread::id current = std::this_thread::get_id();
    if (owner_thread_ == std::thread::id()) {
        owner_thread_ = current;
        return;
    }

    if (owner_thread_ != current) {
        throw std::logic_error(
            "GC-Lib currently requires all GC API calls to originate from a single owner thread.");
    }
}

void GC_Manager::clear_marks_locked() noexcept {
    for (auto& [address, block] : allocations_) {
        (void)address;
        block.header->set_marked(false);
    }
}

void GC_Manager::mark_candidate_locked(std::uintptr_t candidate,
                                       std::vector<BlockRecord*>& worklist) {
    if (!is_candidate_aligned(candidate)) {
        return;
    }

    BlockRecord* block = find_block_locked(candidate);
    if (block == nullptr) {
        return;
    }

    ChunkHeader* header = block->header;
    if (header->magic_number != kChunkMagicNumber || header->is_marked()) {
        return;
    }

    header->set_marked(true);
    worklist.push_back(block);
}

void GC_Manager::drain_mark_worklist_locked(std::vector<BlockRecord*>& worklist) {
    while (!worklist.empty()) {
        BlockRecord* block = worklist.back();
        worklist.pop_back();

        if (block != nullptr) {
            scan_payload_locked(*block, worklist);
        }
    }
}

void GC_Manager::scan_registered_roots_locked(std::vector<BlockRecord*>& worklist) {
    for (const RootRange& range : registered_root_ranges_) {
        RootScanner::scan_range(range.begin, range.end, [this, &worklist](std::uintptr_t candidate) {
            mark_candidate_locked(candidate, worklist);
        });
    }
}

void GC_Manager::scan_payload_locked(const BlockRecord& block,
                                     std::vector<BlockRecord*>& worklist) {
    constexpr std::size_t word_size = sizeof(std::uintptr_t);

    for (std::uintptr_t current = block.payload_begin;
         current + word_size <= block.payload_end;
         current += word_size) {
        const auto value = *reinterpret_cast<const std::uintptr_t*>(current);
        mark_candidate_locked(value, worklist);
    }
}

std::vector<ChunkHeader*> GC_Manager::sweep_locked() {
    std::vector<ChunkHeader*> unreachable;

    for (auto it = allocations_.begin(); it != allocations_.end();) {
        ChunkHeader* header = it->second.header;
        if (header->is_marked()) {
            header->set_marked(false);
            ++it;
            continue;
        }

        managed_bytes_ -= header->size;
        unreachable.push_back(header);
        it = allocations_.erase(it);
    }

    return unreachable;
}

std::vector<ChunkHeader*> GC_Manager::detach_all_blocks_locked() {
    std::vector<ChunkHeader*> blocks;
    blocks.reserve(allocations_.size());

    for (auto& [address, block] : allocations_) {
        (void)address;
        blocks.push_back(block.header);
    }

    allocations_.clear();
    managed_bytes_ = 0;
    return blocks;
}

bool GC_Manager::is_candidate_aligned(std::uintptr_t candidate) noexcept {
    return candidate % alignof(ChunkHeader) == 0U;
}

void GC_Manager::destroy_blocks(const std::vector<ChunkHeader*>& blocks) noexcept {
    for (ChunkHeader* header : blocks) {
        if (header == nullptr || header->destructor == nullptr) {
            continue;
        }

        try {
            header->destructor(payload_pointer(header));
        } catch (...) {
            std::terminate();
        }
    }

    for (ChunkHeader* header : blocks) {
        free_block_memory(header);
    }
}

}  // namespace gc