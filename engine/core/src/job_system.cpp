#include "nexus/core/job_system.h"
#include "nexus/core/log.h"
#include <algorithm>

namespace nexus {

namespace {
// True while executing on a JobSystem worker thread; used to detect re-entrant
// parallel_for/wait_idle calls that would otherwise deadlock the pool.
thread_local bool t_is_worker = false;
} // namespace

JobSystem::JobSystem(u32 thread_count) {
    if (thread_count == 0) {
        thread_count = std::max(1u, std::thread::hardware_concurrency() - 1);
    }

    workers_.reserve(thread_count);
    for (u32 i = 0; i < thread_count; ++i) {
        workers_.emplace_back(&JobSystem::worker_loop, this);
    }

    NX_INFO("JobSystem started with {} worker threads", thread_count);
}

JobSystem::~JobSystem() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    condition_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

void JobSystem::worker_loop() {
    t_is_worker = true;
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] {
                return stop_ || !tasks_.empty();
            });

            if (stop_ && tasks_.empty()) return;

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        task();

        // Decrement under the mutex so wait_idle() cannot evaluate its predicate
        // and start waiting in the window between the decrement and the notify
        // (which would lose the wakeup and hang forever).
        bool became_idle;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            became_idle = (--pending_ == 0);
        }
        if (became_idle) {
            idle_condition_.notify_all();
        }
    }
}

void JobSystem::parallel_for(u32 count, const std::function<void(u32)>& task) {
    if (count == 0) return;

    // Calling from a worker thread would block that worker on sub-tasks that may
    // never be scheduled (all workers waiting → deadlock). Run inline instead.
    if (t_is_worker) {
        for (u32 i = 0; i < count; ++i) task(i);
        return;
    }

    std::vector<std::future<void>> futures;
    futures.reserve(count);

    for (u32 i = 0; i < count; ++i) {
        futures.push_back(submit([&task, i]() { task(i); }));
    }

    for (auto& f : futures) {
        f.get();
    }
}

void JobSystem::wait_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_condition_.wait(lock, [this] {
        return pending_ == 0 && tasks_.empty();
    });
}

JobSystem& JobSystem::instance() {
    static JobSystem instance;
    return instance;
}

} // namespace nexus
