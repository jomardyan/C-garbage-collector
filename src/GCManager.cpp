#include "gc/GCManager.hpp"

#include <algorithm>
#include <chrono>
#include <csetjmp>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <new>
#include <ostream>
#include <queue>
#include <stdexcept>

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "gc/RootScanner.hpp"

namespace gc {
namespace {

std::size_t normalize_alignment(std::size_t alignment) noexcept {
    return (std::max)(alignment, alignof(std::max_align_t));
}

bool add_would_overflow(std::size_t lhs, std::size_t rhs) noexcept {
    return lhs > (std::numeric_limits<std::size_t>::max)() - rhs;
}

std::size_t saturating_add(std::size_t lhs, std::size_t rhs) noexcept {
    return add_would_overflow(lhs, rhs)
               ? (std::numeric_limits<std::size_t>::max)()
               : lhs + rhs;
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
    const std::size_t alignment = header->allocation_alignment();
    header->~ChunkHeader();
    ::operator delete(static_cast<void*>(header), std::align_val_t(alignment));
}

std::size_t block_allocation_size(const ChunkHeader* header) noexcept {
    return static_cast<std::size_t>(header->payload_offset) + header->size;
}

const void* detect_current_thread_stack_bottom() {
#if defined(__linux__)
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) != 0) {
        throw std::logic_error("GC-Lib could not query the current thread stack bounds.");
    }

    void* stack_base = nullptr;
    std::size_t stack_size = 0U;
    const int rc = pthread_attr_getstack(&attributes, &stack_base, &stack_size);
    pthread_attr_destroy(&attributes);
    if (rc != 0 || stack_base == nullptr || stack_size == 0U) {
        throw std::logic_error("GC-Lib could not determine the current thread stack bounds.");
    }

    const auto high = reinterpret_cast<std::uintptr_t>(stack_base) + stack_size;
    return reinterpret_cast<const void*>(high);
#elif defined(__APPLE__)
    const void* stack_high = pthread_get_stackaddr_np(pthread_self());
    if (stack_high == nullptr) {
        throw std::logic_error("GC-Lib could not determine the current thread stack bounds.");
    }
    return stack_high;
#elif defined(_WIN32)
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    (void)low;
    if (high == 0U) {
        throw std::logic_error("GC-Lib could not determine the current thread stack bounds.");
    }
    return reinterpret_cast<const void*>(high);
#else
    throw std::logic_error(
        "GC-Lib does not support automatic thread stack detection on this platform; use gc::register_stack_bottom().");
#endif
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
    if (stack_bottom == nullptr) {
        throw std::invalid_argument("GC-Lib stack anchor must not be null.");
    }
    std::unique_lock<std::mutex> lock(mutex_);
    cooperate_with_stop_the_world_locked(lock);
    threads_[std::this_thread::get_id()].stack_bottom = stack_bottom;
}

void GC_Manager::register_current_thread() {
    register_stack_bottom(detect_current_thread_stack_bottom());
}

void GC_Manager::unregister_current_thread() {
    std::unique_lock<std::mutex> lock(mutex_);
    cooperate_with_stop_the_world_locked(lock);
    threads_.erase(std::this_thread::get_id());
}

bool GC_Manager::is_current_thread_registered() const {
    std::unique_lock<std::mutex> lock(mutex_);
    const_cast<GC_Manager*>(this)->cooperate_with_stop_the_world_locked(lock);
    const auto* state = find_thread_state_locked(std::this_thread::get_id());
    return state != nullptr && state->stack_bottom != nullptr;
}

void GC_Manager::safepoint() {
    std::unique_lock<std::mutex> lock(mutex_);
    cooperate_with_stop_the_world_locked(lock);
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

    std::unique_lock<std::mutex> lock(mutex_);
    registered_root_ranges_.push_back(
        RootRange{reinterpret_cast<const void*>(start),
                  reinterpret_cast<const void*>(finish)});
    // Publish heap-hosted roots before this mutator parks. The new range may
    // contain the only pointer to its target, outside the scanned stack.
    cooperate_with_stop_the_world_locked(lock);
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

    std::unique_lock<std::mutex> lock(mutex_);
    cooperate_with_stop_the_world_locked(lock);
    const auto* nb = reinterpret_cast<const void*>(start);
    const auto* ne = reinterpret_cast<const void*>(finish);

    const auto it = std::find_if(registered_root_ranges_.begin(), registered_root_ranges_.end(),
                                [nb, ne](const RootRange& r) {
                                    return r.begin == nb && r.end == ne;
                                });
    if (it != registered_root_ranges_.end()) registered_root_ranges_.erase(it);
}

void GC_Manager::set_collection_threshold_bytes(std::size_t threshold_bytes) {
    std::unique_lock<std::mutex> lock(mutex_);
    cooperate_with_stop_the_world_locked(lock);
    collection_threshold_bytes_ = threshold_bytes;
}

std::size_t GC_Manager::collection_threshold_bytes() const {
    std::unique_lock<std::mutex> lock(mutex_);
    const_cast<GC_Manager*>(this)->cooperate_with_stop_the_world_locked(lock);
    return collection_threshold_bytes_;
}

std::size_t GC_Manager::managed_bytes() const {
    std::unique_lock<std::mutex> lock(mutex_);
    const_cast<GC_Manager*>(this)->cooperate_with_stop_the_world_locked(lock);
    return managed_bytes_;
}

std::size_t GC_Manager::managed_block_count() const {
    std::unique_lock<std::mutex> lock(mutex_);
    const_cast<GC_Manager*>(this)->cooperate_with_stop_the_world_locked(lock);
    return allocations_.size();
}

GCStats GC_Manager::stats() const {
    std::unique_lock<std::mutex> lock(mutex_);
    const_cast<GC_Manager*>(this)->cooperate_with_stop_the_world_locked(lock);
    GCStats snapshot = stats_;
    snapshot.live_bytes = managed_bytes_;
    snapshot.live_blocks = allocations_.size();
    snapshot.reserved_bytes = reserved_bytes_;
    snapshot.registered_threads = threads_.size();
    snapshot.registered_root_ranges = registered_root_ranges_.size();
    snapshot.registered_weak_refs = weak_refs_.size();
    snapshot.fragmentation_ratio =
        reserved_bytes_ == 0U
            ? 0.0
            : static_cast<double>(reserved_bytes_ - managed_bytes_) /
                  static_cast<double>(reserved_bytes_);
    return snapshot;
}

std::vector<HeapObjectInfo> GC_Manager::live_objects() const {
    auto* self = const_cast<GC_Manager*>(this);
    bool owns_stop_the_world = false;
    std::vector<HeapObjectInfo> objects;
    try {
        std::unique_lock<std::mutex> lock(mutex_);
        self->cooperate_with_stop_the_world_locked(lock);
        // The allocation map mutex alone cannot protect ordinary user fields.
        // Park mutators before reading payloads to construct the object graph.
        if (!collecting_.load(std::memory_order_relaxed)) {
            self->collecting_.store(true, std::memory_order_relaxed);
            self->collector_thread_ = std::this_thread::get_id();
            self->stop_the_world_requested_ = true;
            owns_stop_the_world = true;
            self->wait_for_other_threads_to_pause_locked(lock, self->collector_thread_);
        }
        objects.reserve(allocations_.size());
        for (const auto& [payload_addr, block] : allocations_) {
            (void)payload_addr;
            objects.push_back(describe_block_locked(block));
        }
    } catch (...) {
        if (owns_stop_the_world) self->release_stop_the_world();
        throw;
    }
    if (owns_stop_the_world) self->release_stop_the_world();
    return objects;
}

std::optional<HeapObjectInfo> GC_Manager::object_info(const void* pointer) const {
    if (pointer == nullptr) return std::nullopt;

    auto* self = const_cast<GC_Manager*>(this);
    bool owns_stop_the_world = false;
    std::optional<HeapObjectInfo> object;
    try {
        std::unique_lock<std::mutex> lock(mutex_);
        self->cooperate_with_stop_the_world_locked(lock);
        if (!collecting_.load(std::memory_order_relaxed)) {
            self->collecting_.store(true, std::memory_order_relaxed);
            self->collector_thread_ = std::this_thread::get_id();
            self->stop_the_world_requested_ = true;
            owns_stop_the_world = true;
            self->wait_for_other_threads_to_pause_locked(lock, self->collector_thread_);
        }
        const auto* block = find_block_locked(reinterpret_cast<std::uintptr_t>(pointer));
        if (block != nullptr) object = describe_block_locked(*block);
    } catch (...) {
        if (owns_stop_the_world) self->release_stop_the_world();
        throw;
    }
    if (owns_stop_the_world) self->release_stop_the_world();
    return object;
}

void GC_Manager::dump_heap(std::ostream& output) const {
    const GCStats snapshot = stats();
    const auto objects = live_objects();

    output << "GC heap snapshot: live_objects=" << objects.size()
           << " live_bytes=" << snapshot.live_bytes
           << " reserved_bytes=" << snapshot.reserved_bytes
           << " fragmentation_ratio=" << snapshot.fragmentation_ratio << '\n';

    for (const HeapObjectInfo& object : objects) {
        output << "object payload=" << object.payload
               << " payload_size=" << object.payload_size
               << " reserved_size=" << object.reserved_size
               << " alignment=" << object.allocation_alignment
               << " refs=" << object.outgoing_references.size() << '\n';
        for (void* reference : object.outgoing_references) {
            output << "  -> " << reference << '\n';
        }
    }
}

bool GC_Manager::is_live(const void* payload) const noexcept {
    if (payload == nullptr) {
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    const_cast<GC_Manager*>(this)->cooperate_with_stop_the_world_locked(lock);
    const auto addr = reinterpret_cast<std::uintptr_t>(payload);
    return find_block_locked(addr) != nullptr;
}

void* GC_Manager::malloc_internal(std::size_t payload_size,
                                  std::size_t payload_alignment,
                                  DestructorFn destructor,
                                  bool constructing) {
    if (payload_size == 0U || !std::has_single_bit(payload_alignment)) {
        throw std::bad_alloc();
    }
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cooperate_with_stop_the_world_locked(lock);
        require_registered_thread_locked(std::this_thread::get_id());
    }

    const std::size_t alignment = normalize_alignment(payload_alignment);
    const std::size_t p_offset = round_up_to_alignment(sizeof(ChunkHeader), alignment);

    if (p_offset > (std::numeric_limits<std::uint16_t>::max)() ||
        add_would_overflow(p_offset, payload_size)) {
        throw std::bad_alloc();
    }

    const std::size_t total_size = p_offset + payload_size;
    void* raw = nullptr;

    try {
        raw = ::operator new(total_size, std::align_val_t(alignment));
    } catch (const std::bad_alloc&) {
        collect();
        raw = ::operator new(total_size, std::align_val_t(alignment));
    }

    auto* header = new (raw) ChunkHeader();
    header->size = payload_size;
    header->destructor = destructor;
    header->magic_number = kChunkMagicNumber;
    header->payload_offset = static_cast<std::uint16_t>(p_offset);
    header->set_marked(false);
    header->set_allocation_alignment(alignment);

    try {
        std::unique_lock<std::mutex> lock(mutex_);
        cooperate_with_stop_the_world_locked(lock);
        require_registered_thread_locked(std::this_thread::get_id());
        if (add_would_overflow(managed_bytes_, payload_size) ||
            add_would_overflow(reserved_bytes_, total_size)) {
            throw std::bad_alloc();
        }
        // Tracing may run recursively from a constructor before every member
        // has been initialized. Never expose allocator residue as pointers.
        std::memset(payload_pointer(header), 0, payload_size);
        register_block_locked(header);
        allocations_.find(payload_begin(header))->second.constructing = constructing;
    } catch (...) {
        free_block_memory(header);
        throw;
    }

    return payload_pointer(header);
}

void GC_Manager::finish_construction(void* payload) {
    std::unique_lock<std::mutex> lock(mutex_);
    cooperate_with_stop_the_world_locked(lock);
    const auto it = allocations_.find(reinterpret_cast<std::uintptr_t>(payload));
    if (it == allocations_.end()) {
        throw std::logic_error("GC-Lib construction refers to an unknown allocation.");
    }
    it->second.constructing = false;
}

void GC_Manager::release_unconstructed(void* payload) {
    if (payload == nullptr) {
        return;
    }

    ChunkHeader* header = nullptr;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        cooperate_with_stop_the_world_locked(lock);
        require_registered_thread_locked(std::this_thread::get_id());
        const auto addr = reinterpret_cast<std::uintptr_t>(payload);
        auto it = allocations_.find(addr);
        if (it != allocations_.end()) {
            header = it->second.header;
            managed_bytes_ -= header->size;
            reserved_bytes_ -= block_allocation_size(header);
            invalidate_weak_refs_for_block_locked(it->second.payload_begin,
                                                  it->second.payload_end);
            allocations_.erase(it);
        }
    }

    if (header != nullptr) {
        free_block_memory(header);
    }
}

// collect_if_needed intentionally checks should_collect under the lock and then
// calls collect() after releasing it.  This is a known TOCTOU: a second thread
// could race in and collect between the check and the call.  That is safe because
// collect() is idempotent and re-checks collecting_ under its own lock.
void GC_Manager::collect_if_needed(std::size_t incoming_payload_bytes) {
    bool should_collect = false;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        cooperate_with_stop_the_world_locked(lock);
        require_registered_thread_locked(std::this_thread::get_id());
        const std::size_t projected =
            add_would_overflow(managed_bytes_, incoming_payload_bytes)
                ? (std::numeric_limits<std::size_t>::max)()
                : managed_bytes_ + incoming_payload_bytes;
        should_collect =
            !collecting_.load(std::memory_order_relaxed) &&
            projected >= collection_threshold_bytes_;
    }

    if (should_collect) {
        collect();
    }
}

void GC_Manager::collect() {
    std::vector<ChunkHeader*> unreachable;
    bool stop_the_world_active = false;
    const auto collector_thread = std::this_thread::get_id();
    const auto t_start = std::chrono::steady_clock::now();

    try {
        std::unique_lock<std::mutex> lock(mutex_);
        cooperate_with_stop_the_world_locked(lock);
        if (collecting_.load(std::memory_order_relaxed)) {
            return;
        }
        require_registered_thread_locked(collector_thread);
        collecting_.store(true, std::memory_order_relaxed);
        collector_thread_ = collector_thread;
        stop_the_world_requested_ = true;
        stop_the_world_active = true;

        wait_for_other_threads_to_pause_locked(lock, collector_thread);

        clear_marks_locked();
        std::vector<BlockRecord*> worklist;
        worklist.reserve(allocations_.size());

        for (const auto& [address, block] : allocations_) {
            if (block.constructing) mark_candidate_locked(address, worklist);
        }

        scan_registered_threads_locked(collector_thread, worklist);
        scan_registered_roots_locked(worklist);
        drain_mark_worklist_locked(worklist);
        unreachable = sweep_locked();

        // Update stats while still under the lock.
        std::size_t bytes_freed = 0U;
        for (const ChunkHeader* h : unreachable) {
            bytes_freed += h->size;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto dur = now - t_start;
        stats_.total_collections++;
        stats_.blocks_reclaimed_last = unreachable.size();
        stats_.bytes_reclaimed_last = bytes_freed;
        stats_.bytes_reclaimed_total += bytes_freed;
        stats_.duration_last =
            std::chrono::duration_cast<std::chrono::nanoseconds>(dur);
        stats_.duration_total += stats_.duration_last;
    } catch (...) {
        if (stop_the_world_active) {
            release_stop_the_world();
        }
        throw;
    }

    destroy_blocks(unreachable);
    release_stop_the_world();
}

void GC_Manager::shutdown() {
    std::vector<ChunkHeader*> blocks;
    bool stop_the_world_active = false;
    const auto collector_thread = std::this_thread::get_id();

    try {
        std::unique_lock<std::mutex> lock(mutex_);
        cooperate_with_stop_the_world_locked(lock);
        if (collecting_.load(std::memory_order_relaxed)) {
            return;
        }
        collecting_.store(true, std::memory_order_relaxed);
        collector_thread_ = collector_thread;
        stop_the_world_requested_ = true;
        stop_the_world_active = true;

        wait_for_other_threads_to_pause_locked(lock, collector_thread);
        for (const auto& [address, block] : allocations_) {
            (void)address;
            if (block.constructing) {
                throw std::logic_error("GC-Lib cannot shut down during object construction.");
            }
        }
        blocks = detach_all_blocks_locked();
        for (WeakRefEntry& entry : weak_refs_) {
            if (entry.storage != nullptr) {
                *entry.storage = 0U;
            }
        }
        weak_refs_.clear();
        stats_ = GCStats{};
    } catch (...) {
        if (stop_the_world_active) {
            release_stop_the_world();
        }
        throw;
    }

    destroy_blocks(blocks);
    release_stop_the_world();
}

// --- Weak reference management ---

void GC_Manager::register_weak_ref(std::uintptr_t target, std::uintptr_t* storage) {
    std::unique_lock<std::mutex> lock(mutex_);
    cooperate_with_stop_the_world_locked(lock);
    if (storage == nullptr) return;
    if (find_block_locked(target) == nullptr) {
        *storage = 0U;
        return;
    }
    weak_refs_.push_back({target, storage});
}

std::uintptr_t GC_Manager::read_weak_ref(const std::uintptr_t* storage) const noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    const_cast<GC_Manager*>(this)->cooperate_with_stop_the_world_locked(lock);
    for (const auto& entry : weak_refs_) {
        if (entry.storage == storage) return entry.target;
    }
    return 0U;
}

void GC_Manager::unregister_weak_ref(std::uintptr_t* storage) {
    std::unique_lock<std::mutex> lock(mutex_);
    cooperate_with_stop_the_world_locked(lock);
    for (auto it = weak_refs_.begin(); it != weak_refs_.end(); ++it) {
        if (it->storage == storage) {
            // Swap-and-pop: O(1) removal from unsorted vector.
            *it = weak_refs_.back();
            weak_refs_.pop_back();
            return;
        }
    }
}

void GC_Manager::copy_weak_ref(const std::uintptr_t* old_storage,
                               std::uintptr_t* new_storage) {
    std::unique_lock<std::mutex> lock(mutex_);
    cooperate_with_stop_the_world_locked(lock);
    for (const auto& entry : weak_refs_) {
        if (entry.storage == old_storage) {
            const auto target = entry.target;
            const auto encoded = *old_storage;
            weak_refs_.push_back({target, new_storage});
            *new_storage = encoded;
            return;
        }
    }
    *new_storage = 0U;
}

void GC_Manager::update_weak_ref(std::uintptr_t* old_storage,
                                 std::uintptr_t* new_storage) {
    std::unique_lock<std::mutex> lock(mutex_);
    cooperate_with_stop_the_world_locked(lock);
    for (auto& entry : weak_refs_) {
        if (entry.storage == old_storage) {
            *new_storage = *old_storage;
            *old_storage = 0U;
            entry.storage = new_storage;
            return;
        }
    }
    *new_storage = 0U;
}

void GC_Manager::invalidate_weak_refs_for_block_locked(
    std::uintptr_t pb, std::uintptr_t pe) noexcept {
    weak_refs_.erase(
        std::remove_if(weak_refs_.begin(), weak_refs_.end(),
                       [pb, pe](const WeakRefEntry& e) {
                           if (e.target >= pb && e.target < pe) {
                               *e.storage = 0U;
                               return true;
                           }
                           return false;
                       }),
        weak_refs_.end());
}

// --- Internal helpers ---

GC_Manager::ThreadState* GC_Manager::find_thread_state_locked(
    std::thread::id thread_id) noexcept {
    auto it = threads_.find(thread_id);
    return it == threads_.end() ? nullptr : &it->second;
}

const GC_Manager::ThreadState* GC_Manager::find_thread_state_locked(
    std::thread::id thread_id) const noexcept {
    auto it = threads_.find(thread_id);
    return it == threads_.end() ? nullptr : &it->second;
}

GC_Manager::ThreadState& GC_Manager::require_registered_thread_locked(
    std::thread::id thread_id) {
    ThreadState* state = find_thread_state_locked(thread_id);
    if (state == nullptr || state->stack_bottom == nullptr) {
        throw std::logic_error(
            "Current thread must call gc::register_current_thread() or gc::register_stack_bottom() before using GC-Lib on that thread.");
    }
    return *state;
}

void GC_Manager::cooperate_with_stop_the_world_locked(
    std::unique_lock<std::mutex>& lock) {
    const std::thread::id current = std::this_thread::get_id();
    if (!stop_the_world_requested_ || collector_thread_ == current) {
        return;
    }

    ThreadState* state = find_thread_state_locked(current);
    if (state == nullptr || state->stack_bottom == nullptr) {
        stop_the_world_cv_.wait(lock, [this, current] {
            return !stop_the_world_requested_ || collector_thread_ == current;
        });
        return;
    }

    std::uintptr_t stack_marker = 0U;
    std::jmp_buf registers;
    std::memset(&registers, 0, sizeof(registers));

    lock.unlock();
#if defined(_MSC_VER)
    // Capture register roots only; this buffer is never passed to longjmp.
#pragma warning(suppress : 4611)
#endif
    if (setjmp(registers) != 0) {
        lock.lock();
        return;
    }
    lock.lock();

    if (!stop_the_world_requested_ || collector_thread_ == current) {
        return;
    }

    state = find_thread_state_locked(current);
    if (state == nullptr || state->stack_bottom == nullptr) {
        stop_the_world_cv_.wait(lock, [this, current] {
            return !stop_the_world_requested_ || collector_thread_ == current;
        });
        return;
    }

    state->parked_stack_top = &stack_marker;
    state->fake_stack_handle = RootScanner::current_fake_stack_handle();
    state->paused_for_collection = true;
    state->register_spill.fill(0U);
    std::memcpy(state->register_spill.data(), &registers, sizeof(registers));

    stop_the_world_cv_.notify_all();
    stop_the_world_cv_.wait(lock, [this] { return !stop_the_world_requested_; });

    state->paused_for_collection = false;
    state->parked_stack_top = nullptr;
    state->fake_stack_handle = nullptr;
    state->register_spill.fill(0U);
}

void GC_Manager::wait_for_other_threads_to_pause_locked(
    std::unique_lock<std::mutex>& lock,
    std::thread::id collector_thread) {
    stop_the_world_cv_.wait(lock, [this, collector_thread] {
        for (const auto& [thread_id, state] : threads_) {
            if (thread_id == collector_thread || state.stack_bottom == nullptr) {
                continue;
            }
            if (!state.paused_for_collection) {
                return false;
            }
        }
        return true;
    });
}

void GC_Manager::release_stop_the_world() {
    std::lock_guard<std::mutex> lock(mutex_);
    collector_thread_ = std::thread::id();
    stop_the_world_requested_ = false;
    collecting_.store(false, std::memory_order_release);
    stop_the_world_cv_.notify_all();
}

void GC_Manager::scan_registered_threads_locked(
    std::thread::id collector_thread,
    std::vector<BlockRecord*>& worklist) {
    auto visit_candidate = [this, &worklist](std::uintptr_t candidate) {
        mark_candidate_locked(candidate, worklist);
    };

    for (const auto& [thread_id, state] : threads_) {
        if (state.stack_bottom == nullptr) {
            continue;
        }

        if (thread_id == collector_thread) {
            RootScanner::scan(state.stack_bottom, visit_candidate);
            continue;
        }

        RootScanner::scan_range(state.register_spill.data(),
                                state.register_spill.data() + state.register_spill.size(),
                                visit_candidate);
        RootScanner::scan_thread_stack(state.parked_stack_top,
                                       state.stack_bottom,
                                       state.fake_stack_handle,
                                       visit_candidate);
    }
}

std::vector<void*> GC_Manager::collect_outgoing_references_locked(
    const BlockRecord& block) const {
    std::vector<void*> references;
    constexpr std::size_t word_size = sizeof(std::uintptr_t);

    for (std::uintptr_t cur = block.payload_begin;
         cur + word_size <= block.payload_end;
         cur += word_size) {
        std::uintptr_t candidate = 0U;
        std::memcpy(&candidate, reinterpret_cast<const void*>(cur), word_size);
        const BlockRecord* target = find_block_locked(candidate);
        if (target == nullptr || target == &block) {
            continue;
        }

        void* payload = payload_pointer(target->header);
        if (std::find(references.begin(), references.end(), payload) == references.end()) {
            references.push_back(payload);
        }
    }

    return references;
}

HeapObjectInfo GC_Manager::describe_block_locked(const BlockRecord& block) const {
    const auto* header = block.header;
    return HeapObjectInfo{
        payload_pointer(block.header),
        header->size,
        block_allocation_size(header),
        header->payload_offset,
        header->allocation_alignment(),
        header->is_marked(),
        collect_outgoing_references_locked(block),
    };
}

GC_Manager::BlockRecord* GC_Manager::find_block_locked(
    std::uintptr_t address) noexcept {
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

const GC_Manager::BlockRecord* GC_Manager::find_block_locked(
    std::uintptr_t address) const noexcept {
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
    reserved_bytes_ += block_allocation_size(header);
    stats_.total_allocations = saturating_add(stats_.total_allocations, 1U);
    stats_.bytes_allocated_total = saturating_add(stats_.bytes_allocated_total, header->size);
    stats_.peak_live_bytes = (std::max)(stats_.peak_live_bytes, managed_bytes_);
    stats_.peak_live_blocks = (std::max)(stats_.peak_live_blocks, allocations_.size());
}

std::vector<ChunkHeader*> GC_Manager::order_blocks_for_finalization_locked(
    const std::vector<BlockRecord>& blocks) const {
    if (blocks.size() <= 1U) {
        std::vector<ChunkHeader*> ordered;
        ordered.reserve(blocks.size());
        for (const BlockRecord& block : blocks) {
            ordered.push_back(block.header);
        }
        return ordered;
    }

    std::map<std::uintptr_t, std::size_t> index_by_begin;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        index_by_begin.emplace(blocks[index].payload_begin, index);
    }

    std::vector<std::vector<std::size_t>> edges(blocks.size());
    std::vector<std::size_t> indegree(blocks.size(), 0U);

    for (std::size_t source_index = 0; source_index < blocks.size(); ++source_index) {
        const BlockRecord& block = blocks[source_index];
        constexpr std::size_t word_size = sizeof(std::uintptr_t);

        for (std::uintptr_t cur = block.payload_begin;
             cur + word_size <= block.payload_end;
             cur += word_size) {
            std::uintptr_t candidate = 0U;
            std::memcpy(&candidate, reinterpret_cast<const void*>(cur), word_size);
            auto it = index_by_begin.upper_bound(candidate);
            if (it == index_by_begin.begin()) {
                continue;
            }
            --it;

            const std::size_t target_index = it->second;
            const BlockRecord& target = blocks[target_index];
            if (candidate < target.payload_begin || candidate >= target.payload_end ||
                target_index == source_index) {
                continue;
            }

            auto& outgoing = edges[source_index];
            if (std::find(outgoing.begin(), outgoing.end(), target_index) != outgoing.end()) {
                continue;
            }

            outgoing.push_back(target_index);
            ++indegree[target_index];
        }
    }

    auto higher_address_first = [&blocks](std::size_t lhs, std::size_t rhs) {
        return blocks[lhs].payload_begin < blocks[rhs].payload_begin;
    };
    std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(higher_address_first)>
        ready(higher_address_first);

    for (std::size_t index = 0; index < blocks.size(); ++index) {
        if (indegree[index] == 0U) {
            ready.push(index);
        }
    }

    std::vector<ChunkHeader*> ordered;
    ordered.reserve(blocks.size());
    std::vector<bool> emitted(blocks.size(), false);

    while (!ready.empty()) {
        const std::size_t index = ready.top();
        ready.pop();
        if (emitted[index]) {
            continue;
        }

        emitted[index] = true;
        ordered.push_back(blocks[index].header);

        for (std::size_t target_index : edges[index]) {
            if (indegree[target_index] > 0U) {
                --indegree[target_index];
                if (indegree[target_index] == 0U) {
                    ready.push(target_index);
                }
            }
        }
    }

    for (std::size_t index = blocks.size(); index > 0U; --index) {
        if (!emitted[index - 1U]) {
            ordered.push_back(blocks[index - 1U].header);
        }
    }

    return ordered;
}

void GC_Manager::clear_marks_locked() noexcept {
    for (auto& [addr, block] : allocations_) {
        (void)addr;
        block.header->set_marked(false);
    }
}

void GC_Manager::mark_candidate_locked(std::uintptr_t candidate,
                                       std::vector<BlockRecord*>& worklist) {
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
        RootScanner::scan_range(range.begin, range.end,
                                [this, &worklist](std::uintptr_t candidate) {
                                    mark_candidate_locked(candidate, worklist);
                                });
    }
}

void GC_Manager::scan_payload_locked(const BlockRecord& block,
                                     std::vector<BlockRecord*>& worklist) {
    constexpr std::size_t word_size = sizeof(std::uintptr_t);
    for (std::uintptr_t cur = block.payload_begin;
         cur + word_size <= block.payload_end;
         cur += word_size) {
        std::uintptr_t value = 0U;
        std::memcpy(&value, reinterpret_cast<const void*>(cur), word_size);
        mark_candidate_locked(value, worklist);
    }
}

std::vector<ChunkHeader*> GC_Manager::sweep_locked() {
    std::vector<BlockRecord> unreachable;
    unreachable.reserve(allocations_.size());

    for (const auto& [address, block] : allocations_) {
        (void)address;
        if (!block.header->is_marked()) unreachable.push_back(block);
    }
    // Every potentially throwing allocation must finish before detaching blocks.
    auto ordered = order_blocks_for_finalization_locked(unreachable);

    for (auto it = allocations_.begin(); it != allocations_.end();) {
        ChunkHeader* header = it->second.header;
        if (header->is_marked()) {
            header->set_marked(false);
            ++it;
            continue;
        }
        // Null out any weak pointers that target this block before reclaiming it.
        invalidate_weak_refs_for_block_locked(it->second.payload_begin,
                                              it->second.payload_end);
        managed_bytes_ -= header->size;
        reserved_bytes_ -= block_allocation_size(header);
        it = allocations_.erase(it);
    }

    return ordered;
}

std::vector<ChunkHeader*> GC_Manager::detach_all_blocks_locked() {
    std::vector<BlockRecord> blocks;
    blocks.reserve(allocations_.size());

    for (const auto& [payload_addr, block] : allocations_) {
        (void)payload_addr;
        blocks.push_back(block);
    }

    auto ordered = order_blocks_for_finalization_locked(blocks);
    allocations_.clear();
    managed_bytes_ = 0;
    reserved_bytes_ = 0;
    return ordered;
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
