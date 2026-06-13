#pragma once

#include <nexus/core/types.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nexus {

// ---------------------------------------------------------------------------
// ProfileSample — a single recorded scope measurement
// ---------------------------------------------------------------------------
struct ProfileSample {
    std::string name;
    u32         depth       = 0;
    f64         start_us    = 0.0;   // microseconds from frame start
    f64         duration_us = 0.0;
};

// ---------------------------------------------------------------------------
// GPUTimestamp — a single GPU timing measurement
// ---------------------------------------------------------------------------
struct GPUTimestamp {
    std::string name;
    f64         duration_us = 0.0;
};

// ---------------------------------------------------------------------------
// FrameProfile — all profiling data for one frame
// ---------------------------------------------------------------------------
struct FrameProfile {
    u64 frame_number = 0;
    f64 total_cpu_us = 0.0;
    f64 total_gpu_us = 0.0;
    std::vector<ProfileSample> cpu_samples;
    std::vector<GPUTimestamp>   gpu_timestamps;
};

// ---------------------------------------------------------------------------
// Profiler — CPU frame timeline + GPU timing
// ---------------------------------------------------------------------------
class Profiler {
public:
    static constexpr u32 MAX_HISTORY = 120;

    Profiler();

    /// Call at the start of every frame.
    void begin_frame();

    /// Call at the end of every frame.
    void end_frame();

    /// Push a named CPU scope (nested scopes produce a tree).
    void begin_scope(const std::string& name);
    void end_scope();

    /// Record a GPU timestamp result (driver-side timing passed in).
    void record_gpu_time(const std::string& name, f64 duration_us);

    /// Access the completed frame history (ring buffer, most recent last).
    [[nodiscard]] const std::vector<FrameProfile>& history() const { return history_; }

    /// Most recent completed frame, copied under the lock (empty if none).
    /// Returns by value so the result can't dangle if end_frame() recycles the
    /// history buffer on another thread.
    [[nodiscard]] std::optional<FrameProfile> last_frame() const;

    /// Average CPU frame time over the history buffer (microseconds).
    [[nodiscard]] f64 average_cpu_us() const;

    /// Average GPU frame time over the history buffer (microseconds).
    [[nodiscard]] f64 average_gpu_us() const;

    /// Whether profiling is currently active.
    [[nodiscard]] bool is_active() const { return active_; }
    void set_active(bool v) { active_ = v; }

private:
    using Clock     = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    bool          active_ = true;
    FrameProfile  current_;
    u32           scope_depth_ = 0;
    std::vector<TimePoint> scope_starts_;

    TimePoint     frame_start_{};
    std::vector<FrameProfile> history_;

    mutable std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// ScopedProfile — RAII scope helper
// ---------------------------------------------------------------------------
class ScopedProfile {
public:
    ScopedProfile(Profiler& profiler, const std::string& name)
        : profiler_(profiler) {
        profiler_.begin_scope(name);
    }
    ~ScopedProfile() {
        profiler_.end_scope();
    }

    NEXUS_NON_COPYABLE(ScopedProfile)
    NEXUS_NON_MOVABLE(ScopedProfile)

private:
    Profiler& profiler_;
};

} // namespace nexus
