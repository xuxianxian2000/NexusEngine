#include "nexus/perf/profiler.h"

#include <algorithm>
#include <numeric>

namespace nexus {

Profiler::Profiler() {
    history_.reserve(MAX_HISTORY);
}

void Profiler::begin_frame() {
    if (!active_) return;
    std::lock_guard lock(mutex_);
    current_ = FrameProfile{};
    scope_depth_ = 0;
    scope_starts_.clear();
    frame_start_ = Clock::now();
}

void Profiler::end_frame() {
    if (!active_) return;
    std::lock_guard lock(mutex_);

    auto now = Clock::now();
    current_.total_cpu_us = std::chrono::duration<f64, std::micro>(now - frame_start_).count();

    // Sum GPU timestamps.
    current_.total_gpu_us = 0.0;
    for (const auto& ts : current_.gpu_timestamps) {
        current_.total_gpu_us += ts.duration_us;
    }

    current_.frame_number = history_.empty() ? 0 : history_.back().frame_number + 1;

    if (history_.size() >= MAX_HISTORY) {
        history_.erase(history_.begin());
    }
    history_.push_back(std::move(current_));
    current_ = FrameProfile{};
}

void Profiler::begin_scope(const std::string& name) {
    if (!active_) return;
    std::lock_guard lock(mutex_);

    ProfileSample sample;
    sample.name  = name;
    sample.depth = scope_depth_;
    sample.start_us = std::chrono::duration<f64, std::micro>(Clock::now() - frame_start_).count();

    scope_starts_.push_back(Clock::now());
    current_.cpu_samples.push_back(std::move(sample));
    ++scope_depth_;
}

void Profiler::end_scope() {
    if (!active_) return;
    std::lock_guard lock(mutex_);

    if (scope_starts_.empty() || scope_depth_ == 0) return;
    --scope_depth_;

    auto start = scope_starts_.back();
    scope_starts_.pop_back();

    // Find the matching sample (the one at this depth that hasn't been closed).
    // Walk backwards to find the most recent sample at current depth.
    for (auto it = current_.cpu_samples.rbegin(); it != current_.cpu_samples.rend(); ++it) {
        if (it->depth == scope_depth_ && it->duration_us == 0.0) {
            it->duration_us = std::chrono::duration<f64, std::micro>(Clock::now() - start).count();
            break;
        }
    }
}

void Profiler::record_gpu_time(const std::string& name, f64 duration_us) {
    if (!active_) return;
    std::lock_guard lock(mutex_);
    current_.gpu_timestamps.push_back({name, duration_us});
}

std::optional<FrameProfile> Profiler::last_frame() const {
    std::lock_guard lock(mutex_);
    if (history_.empty()) return std::nullopt;
    return history_.back();
}

f64 Profiler::average_cpu_us() const {
    std::lock_guard lock(mutex_);
    if (history_.empty()) return 0.0;
    f64 sum = 0.0;
    for (const auto& f : history_) sum += f.total_cpu_us;
    return sum / static_cast<f64>(history_.size());
}

f64 Profiler::average_gpu_us() const {
    std::lock_guard lock(mutex_);
    if (history_.empty()) return 0.0;
    f64 sum = 0.0;
    for (const auto& f : history_) sum += f.total_gpu_us;
    return sum / static_cast<f64>(history_.size());
}

} // namespace nexus
