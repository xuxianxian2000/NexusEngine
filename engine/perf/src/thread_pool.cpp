#include "nexus/perf/thread_pool.h"

#include <algorithm>

namespace nexus {

// ---------------------------------------------------------------------------
// ParallelCommandBuffer
// ---------------------------------------------------------------------------

void ParallelCommandBuffer::submit(CommandBucket bucket) {
    std::lock_guard lock(mutex_);
    buckets_.push_back(std::move(bucket));
}

void ParallelCommandBuffer::sort_and_execute() {
    std::lock_guard lock(mutex_);
    std::sort(buckets_.begin(), buckets_.end(),
        [](const CommandBucket& a, const CommandBucket& b) {
            return a.sort_key < b.sort_key;
        });
    for (auto& cmd : buckets_) {
        if (cmd.execute) cmd.execute();
    }
    buckets_.clear();
}

u32 ParallelCommandBuffer::pending_count() const {
    std::lock_guard lock(mutex_);
    return static_cast<u32>(buckets_.size());
}

void ParallelCommandBuffer::clear() {
    std::lock_guard lock(mutex_);
    buckets_.clear();
}

// ---------------------------------------------------------------------------
// RenderThreadPool
// ---------------------------------------------------------------------------

RenderThreadPool::RenderThreadPool(u32 thread_count) {
    if (thread_count == 0) {
        thread_count = std::max(1u, std::thread::hardware_concurrency() - 1);
    }
    workers_.reserve(thread_count);
    for (u32 i = 0; i < thread_count; ++i) {
        workers_.emplace_back(&RenderThreadPool::worker_loop, this);
    }
}

RenderThreadPool::~RenderThreadPool() {
    stop_.store(true);
    cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
}

std::future<void> RenderThreadPool::submit(std::function<void()> task) {
    std::packaged_task<void()> pt(std::move(task));
    auto future = pt.get_future();
    {
        std::lock_guard lock(mutex_);
        tasks_.push(std::move(pt));
    }
    cv_.notify_one();
    return future;
}

void RenderThreadPool::wait_idle() {
    std::unique_lock lock(mutex_);
    idle_cv_.wait(lock, [this] {
        return tasks_.empty() && active_tasks_.load() == 0;
    });
}

u32 RenderThreadPool::queued_tasks() const {
    std::lock_guard lock(mutex_);
    return static_cast<u32>(tasks_.size());
}

void RenderThreadPool::worker_loop() {
    while (true) {
        std::packaged_task<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_.load() || !tasks_.empty(); });
            if (stop_.load() && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
            active_tasks_.fetch_add(1);
        }
        task();
        // Decrement under the mutex so wait_idle() cannot miss the wakeup
        // between the decrement and the notify (lost-wakeup hang).
        {
            std::lock_guard lock(mutex_);
            active_tasks_.fetch_sub(1);
        }
        idle_cv_.notify_all();
    }
}

} // namespace nexus
