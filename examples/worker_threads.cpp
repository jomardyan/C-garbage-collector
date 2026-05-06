#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include "gc/GC.hpp"

namespace {

struct Job {
    explicit Job(int id_value) : id(id_value) {}

    gc::gc_ptr<Job> next;
    int id = 0;
};

void worker(std::atomic<int>& completed_jobs, int worker_id) {
    // Each mutator thread must register itself before using the GC.
    gc::ScopedThreadRegistration thread_registration;

    gc::gc_root<Job> head(gc::gc_new<Job>(worker_id * 100));
    head->next = gc::gc_new<Job>(worker_id * 100 + 1);

    for (int iteration = 0; iteration < 4; ++iteration) {
        // safepoint() lets the collector pause this thread cooperatively if needed.
        gc::safepoint();
        completed_jobs.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace

int main() {
    gc::register_current_thread();

    std::atomic<int> completed_jobs = 0;
    std::thread left(worker, std::ref(completed_jobs), 1);
    std::thread right(worker, std::ref(completed_jobs), 2);

    left.join();
    right.join();

    gc::collect();
    std::cout << "Completed worker iterations: " << completed_jobs.load() << '\n';

    gc::shutdown();
    return 0;
}