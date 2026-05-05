#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include "gc/ChunkHeader.hpp"

namespace gc {

/// Snapshot of collection statistics returned by GC_Manager::stats().
struct GCStats {
    std::size_t total_collections = 0;
    std::size_t bytes_reclaimed_last = 0;
    std::size_t blocks_reclaimed_last = 0;
    std::size_t bytes_reclaimed_total = 0;
    std::chrono::nanoseconds duration_last{0};
    std::chrono::nanoseconds duration_total{0};
};

class GC_Manager {
public:
    static GC_Manager& instance();

    GC_Manager(const GC_Manager&) = delete;
    GC_Manager& operator=(const GC_Manager&) = delete;

    void register_stack_bottom(const void* stack_bottom);
    void register_root_range(const void* begin, const void* end);
    void unregister_root_range(const void* begin, const void* end);
    void set_collection_threshold_bytes(std::size_t threshold_bytes);

    std::size_t collection_threshold_bytes() const;
    std::size_t managed_bytes() const;
    std::size_t managed_block_count() const;
    GCStats stats() const;

    /// Returns true if the given payload pointer belongs to a live GC block.
    bool is_live(const void* payload) const noexcept;

    void* malloc_internal(std::size_t payload_size,
                          std::size_t payload_alignment,
                          DestructorFn destructor);
    void release_unconstructed(void* payload);

    void collect();
    void collect_if_needed(std::size_t incoming_payload_bytes = 0);
    void shutdown();

    // --- Weak reference support ---
    // Called by gc_weak_ptr: target is the ptr stored inside the weak handle,
    // storage is &ptr_ so the GC can null it out when the target is reclaimed.
    void register_weak_ref(std::uintptr_t target, void** storage);
    void unregister_weak_ref(void** storage);
    // Called on move: re-register the same target under a new storage address.
    void update_weak_ref(void** old_storage, void** new_storage);

    template <typename T>
    void register_root_object(const T* object) {
        static_assert(!std::is_void_v<T>);
        auto* begin = static_cast<const void*>(object);
        auto* end = static_cast<const void*>(
            reinterpret_cast<const unsigned char*>(object) + sizeof(T));
        register_root_range(begin, end);
    }

    template <typename T>
    void unregister_root_object(const T* object) {
        static_assert(!std::is_void_v<T>);
        auto* begin = static_cast<const void*>(object);
        auto* end = static_cast<const void*>(
            reinterpret_cast<const unsigned char*>(object) + sizeof(T));
        unregister_root_range(begin, end);
    }

    template <typename T>
    static void destroy_object(void* payload) {
        static_assert(!std::is_void_v<T>);
        static_cast<T*>(payload)->~T();
    }

private:
    struct BlockRecord {
        ChunkHeader* header = nullptr;
        std::uintptr_t payload_begin = 0;
        std::uintptr_t payload_end = 0;
    };

    struct RootRange {
        const void* begin = nullptr;
        const void* end = nullptr;
    };

    // Flat list; sweep does O(n*m) invalidation — acceptable while weak refs are few.
    struct WeakRefEntry {
        std::uintptr_t target;  // pointer value held inside gc_weak_ptr
        void** storage;         // &ptr_ inside the gc_weak_ptr instance
    };

    GC_Manager() = default;
    ~GC_Manager();

    BlockRecord* find_block_locked(std::uintptr_t address) noexcept;
    const BlockRecord* find_block_locked(std::uintptr_t address) const noexcept;
    void register_block_locked(ChunkHeader* header);
    void bind_or_reject_thread_locked();
    void clear_marks_locked() noexcept;
    void mark_candidate_locked(std::uintptr_t candidate,
                               std::vector<BlockRecord*>& worklist);
    void drain_mark_worklist_locked(std::vector<BlockRecord*>& worklist);
    void scan_registered_roots_locked(std::vector<BlockRecord*>& worklist);
    void scan_payload_locked(const BlockRecord& block,
                             std::vector<BlockRecord*>& worklist);
    std::vector<ChunkHeader*> sweep_locked();
    std::vector<ChunkHeader*> detach_all_blocks_locked();
    void invalidate_weak_refs_for_block_locked(std::uintptr_t payload_begin,
                                               std::uintptr_t payload_end) noexcept;

    static bool is_candidate_aligned(std::uintptr_t candidate) noexcept;
    static void destroy_blocks(const std::vector<ChunkHeader*>& blocks) noexcept;

    mutable std::mutex mutex_;
    std::map<std::uintptr_t, BlockRecord> allocations_;
    std::vector<RootRange> registered_root_ranges_;
    std::vector<WeakRefEntry> weak_refs_;
    const void* stack_bottom_ = nullptr;
    std::size_t managed_bytes_ = 0;
    std::size_t collection_threshold_bytes_ = 10U * 1024U * 1024U;
    std::thread::id owner_thread_;
    // Atomic so it can be reset after destroy_blocks() without re-acquiring the mutex.
    std::atomic<bool> collecting_{false};
    GCStats stats_;
};

inline void register_stack_bottom(const void* stack_bottom) {
    GC_Manager::instance().register_stack_bottom(stack_bottom);
}

inline void collect() {
    GC_Manager::instance().collect();
}

inline void shutdown() {
    GC_Manager::instance().shutdown();
}

}  // namespace gc
