#include "nexus/perf/memory_tracker.h"

namespace nexus {

void MemoryTracker::record_alloc(void* ptr, std::size_t size, const std::string& tag) {
    if (!ptr) return;
    std::lock_guard lock(mutex_);

    auto key = reinterpret_cast<uintptr_t>(ptr);
    // If this address is already tracked (address reuse without a recorded
    // free, or a double-record), undo the previous record's accounting first so
    // current_bytes is not permanently inflated.
    if (auto existing = live_.find(key); existing != live_.end()) {
        auto prev = tag_stats_.find(existing->second.tag);
        if (prev != tag_stats_.end()) {
            prev->second.current_bytes -= existing->second.size;
        }
    }
    live_[key] = AllocationRecord{ptr, size, tag};

    auto& s = tag_stats_[tag];
    s.tag = tag;
    s.current_bytes += size;
    s.total_allocations++;
    if (s.current_bytes > s.peak_bytes) {
        s.peak_bytes = s.current_bytes;
    }
}

void MemoryTracker::record_free(void* ptr) {
    if (!ptr) return;
    std::lock_guard lock(mutex_);

    auto key = reinterpret_cast<uintptr_t>(ptr);
    auto it = live_.find(key);
    if (it == live_.end()) return;

    const auto& rec = it->second;
    auto sit = tag_stats_.find(rec.tag);
    if (sit != tag_stats_.end()) {
        sit->second.current_bytes -= rec.size;
        sit->second.total_deallocations++;
    }
    live_.erase(it);
}

AllocationStats MemoryTracker::stats_for(const std::string& tag) const {
    std::lock_guard lock(mutex_);
    auto it = tag_stats_.find(tag);
    if (it == tag_stats_.end()) return AllocationStats{tag};
    return it->second;
}

std::vector<AllocationStats> MemoryTracker::all_stats() const {
    std::lock_guard lock(mutex_);
    std::vector<AllocationStats> result;
    result.reserve(tag_stats_.size());
    for (const auto& [_, s] : tag_stats_) {
        result.push_back(s);
    }
    return result;
}

std::vector<AllocationRecord> MemoryTracker::live_allocations() const {
    std::lock_guard lock(mutex_);
    std::vector<AllocationRecord> result;
    result.reserve(live_.size());
    for (const auto& [_, r] : live_) {
        result.push_back(r);
    }
    return result;
}

std::size_t MemoryTracker::total_allocated() const {
    std::lock_guard lock(mutex_);
    std::size_t total = 0;
    for (const auto& [_, s] : tag_stats_) {
        total += s.current_bytes;
    }
    return total;
}

std::size_t MemoryTracker::total_peak() const {
    std::lock_guard lock(mutex_);
    std::size_t total = 0;
    for (const auto& [_, s] : tag_stats_) {
        total += s.peak_bytes;
    }
    return total;
}

u64 MemoryTracker::live_count() const {
    std::lock_guard lock(mutex_);
    return static_cast<u64>(live_.size());
}

void MemoryTracker::reset() {
    std::lock_guard lock(mutex_);
    live_.clear();
    tag_stats_.clear();
}

} // namespace nexus
