#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <csetjmp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
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
    std::size_t live_bytes = 0;
    std::size_t live_blocks = 0;
    std::size_t reserved_bytes = 0;
    double fragmentation_ratio = 0.0;
    std::chrono::nanoseconds duration_last{0};
    std::chrono::nanoseconds duration_total{0};
};

/// Debug snapshot of one live managed object.
struct HeapObjectInfo {
    void* payload = nullptr;
    std::size_t payload_size = 0;
    std::size_t reserved_size = 0;
    std::size_t payload_offset = 0;
    std::size_t allocation_alignment = 0;
    bool marked = false;
    std::vector<void*> outgoing_references;
};

/// Global conservative mark-and-sweep collector.
class GC_Manager {
public:
    static GC_Manager& instance();

    GC_Manager(const GC_Manager&) = delete;
    GC_Manager& operator=(const GC_Manager&) = delete;

    /// Registers the approximate stack bottom for the current thread.
    /// Each mutator thread should call this before allocating or collecting.
    void register_stack_bottom(const void* stack_bottom);

    /// Detects the current thread's stack bounds and registers them automatically.
    void register_current_thread();

    /// Removes the current thread from the collector's registered thread set.
    void unregister_current_thread();

    /// Cooperatively parks the current thread if another thread has started collection.
    void safepoint();

    /// Registers an explicit root memory range for conservative scanning.
    void register_root_range(const void* begin, const void* end);

    /// Removes a previously registered explicit root memory range.
    void unregister_root_range(const void* begin, const void* end);

    /// Sets the payload-byte threshold used by collect_if_needed().
    void set_collection_threshold_bytes(std::size_t threshold_bytes);

    /// Returns the payload-byte threshold used by collect_if_needed().
    std::size_t collection_threshold_bytes() const;

    /// Returns the payload bytes currently managed by the collector.
    std::size_t managed_bytes() const;

    /// Returns the number of currently live managed blocks.
    std::size_t managed_block_count() const;

    /// Returns cumulative collector timing and reclamation statistics.
    GCStats stats() const;

    /// Returns a debug snapshot of every live object currently managed by the GC.
    std::vector<HeapObjectInfo> live_objects() const;

    /// Writes a human-readable heap snapshot including outgoing references.
    void dump_heap(std::ostream& output) const;

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
    // Called by gc_weak_ptr: target is the raw payload pointer value, storage is
    // the encoded word inside the weak handle so the GC can null it out when the
    // target is reclaimed.
    void register_weak_ref(std::uintptr_t target, std::uintptr_t* storage);
    void unregister_weak_ref(std::uintptr_t* storage);
    // Called on move: re-register the same target under a new storage address.
    void update_weak_ref(std::uintptr_t* old_storage, std::uintptr_t* new_storage);

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
    static constexpr std::size_t kRegisterSpillWordCount =
        (sizeof(std::jmp_buf) + sizeof(std::uintptr_t) - 1U) / sizeof(std::uintptr_t);

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
        std::uintptr_t* storage;  // &encoded_ptr_ inside the gc_weak_ptr instance
    };

    struct ThreadState {
        const void* stack_bottom = nullptr;
        const void* parked_stack_top = nullptr;
        void* fake_stack_handle = nullptr;
        std::array<std::uintptr_t, kRegisterSpillWordCount> register_spill{};
        bool paused_for_collection = false;
    };

    GC_Manager() = default;
    ~GC_Manager();

    BlockRecord* find_block_locked(std::uintptr_t address) noexcept;
    const BlockRecord* find_block_locked(std::uintptr_t address) const noexcept;
    void register_block_locked(ChunkHeader* header);
    ThreadState* find_thread_state_locked(std::thread::id thread_id) noexcept;
    const ThreadState* find_thread_state_locked(std::thread::id thread_id) const noexcept;
    ThreadState& require_registered_thread_locked(std::thread::id thread_id);
    void cooperate_with_stop_the_world_locked(std::unique_lock<std::mutex>& lock);
    void wait_for_other_threads_to_pause_locked(std::unique_lock<std::mutex>& lock,
                                                std::thread::id collector_thread);
    void release_stop_the_world();
    void scan_registered_threads_locked(std::thread::id collector_thread,
                                        std::vector<BlockRecord*>& worklist);
    std::vector<void*> collect_outgoing_references_locked(const BlockRecord& block) const;
    void clear_marks_locked() noexcept;
    std::vector<ChunkHeader*> order_blocks_for_finalization_locked(
        const std::vector<BlockRecord>& blocks) const;
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
    static void destroy_blocks(const std::vector<ChunkHeader*>& blocks) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable stop_the_world_cv_;
    std::map<std::uintptr_t, BlockRecord> allocations_;
    std::map<std::thread::id, ThreadState> threads_;
    std::vector<RootRange> registered_root_ranges_;
    std::vector<WeakRefEntry> weak_refs_;
    std::size_t managed_bytes_ = 0;
    std::size_t reserved_bytes_ = 0;
    std::size_t collection_threshold_bytes_ = 10U * 1024U * 1024U;
    std::thread::id collector_thread_;
    bool stop_the_world_requested_ = false;
    // Atomic so it can be reset after destroy_blocks() without re-acquiring the mutex.
    std::atomic<bool> collecting_{false};
    GCStats stats_;
};

/// RAII helper for registering and unregistering a mutator thread.
class ScopedThreadRegistration {
public:
    ScopedThreadRegistration() {
        GC_Manager::instance().register_current_thread();
    }

    explicit ScopedThreadRegistration(const void* stack_bottom) {
        GC_Manager::instance().register_stack_bottom(stack_bottom);
    }

    ~ScopedThreadRegistration() {
        GC_Manager::instance().unregister_current_thread();
    }

    ScopedThreadRegistration(const ScopedThreadRegistration&) = delete;
    ScopedThreadRegistration& operator=(const ScopedThreadRegistration&) = delete;
};

/// RAII helper for registering and unregistering an explicit root memory range.
class ScopedRootRange {
public:
    ScopedRootRange() noexcept = default;

    ScopedRootRange(const void* begin, const void* end) {
        reset(begin, end);
    }

    ~ScopedRootRange() {
        reset();
    }

    ScopedRootRange(const ScopedRootRange&) = delete;
    ScopedRootRange& operator=(const ScopedRootRange&) = delete;

    ScopedRootRange(ScopedRootRange&& other) noexcept
        : begin_(other.begin_), end_(other.end_), registered_(other.registered_) {
        other.begin_ = nullptr;
        other.end_ = nullptr;
        other.registered_ = false;
    }

    ScopedRootRange& operator=(ScopedRootRange&& other) noexcept {
        if (this != &other) {
            reset();
            begin_ = other.begin_;
            end_ = other.end_;
            registered_ = other.registered_;
            other.begin_ = nullptr;
            other.end_ = nullptr;
            other.registered_ = false;
        }
        return *this;
    }

    void reset(const void* begin = nullptr, const void* end = nullptr) {
        if (registered_) {
            GC_Manager::instance().unregister_root_range(begin_, end_);
        }

        begin_ = begin;
        end_ = end;
        registered_ = begin_ != nullptr && end_ != nullptr && begin_ != end_;

        if (registered_) {
            GC_Manager::instance().register_root_range(begin_, end_);
        }
    }

    const void* begin() const noexcept { return begin_; }
    const void* end() const noexcept { return end_; }
    explicit operator bool() const noexcept { return registered_; }

private:
    const void* begin_ = nullptr;
    const void* end_ = nullptr;
    bool registered_ = false;
};

/// RAII helper for registering and unregistering an explicit root object.
template <typename T>
class ScopedRootObject {
    static_assert(!std::is_void_v<T>, "ScopedRootObject<void> is not supported.");

public:
    ScopedRootObject() noexcept = default;

    explicit ScopedRootObject(const T* object) {
        reset(object);
    }

    ~ScopedRootObject() {
        reset();
    }

    ScopedRootObject(const ScopedRootObject&) = delete;
    ScopedRootObject& operator=(const ScopedRootObject&) = delete;

    ScopedRootObject(ScopedRootObject&& other) noexcept : object_(other.object_) {
        other.object_ = nullptr;
    }

    ScopedRootObject& operator=(ScopedRootObject&& other) noexcept {
        if (this != &other) {
            reset();
            object_ = other.object_;
            other.object_ = nullptr;
        }
        return *this;
    }

    void reset(const T* object = nullptr) {
        if (object_ != nullptr) {
            GC_Manager::instance().unregister_root_object(object_);
        }

        object_ = object;

        if (object_ != nullptr) {
            GC_Manager::instance().register_root_object(object_);
        }
    }

    const T* get() const noexcept { return object_; }
    explicit operator bool() const noexcept { return object_ != nullptr; }

private:
    const T* object_ = nullptr;
};

inline void register_stack_bottom(const void* stack_bottom) {
    GC_Manager::instance().register_stack_bottom(stack_bottom);
}

inline void register_current_thread() {
    GC_Manager::instance().register_current_thread();
}

inline void unregister_current_thread() {
    GC_Manager::instance().unregister_current_thread();
}

inline void safepoint() {
    GC_Manager::instance().safepoint();
}

inline void collect() {
    GC_Manager::instance().collect();
}

inline void shutdown() {
    GC_Manager::instance().shutdown();
}

}  // namespace gc
