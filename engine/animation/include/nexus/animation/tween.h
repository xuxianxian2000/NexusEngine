#pragma once

#include "nexus/core/types.h"
#include "nexus/core/math.h"
#include <cmath>
#include <deque>
#include <functional>

namespace nexus::anim {

// ─────────────────────────────────────────────────────────────────────────────
// Easing functions — all take t in [0,1] and return [0,1]
// ─────────────────────────────────────────────────────────────────────────────

namespace ease {

inline float linear(float t)        { return t; }

// Quadratic
inline float in_quad(float t)       { return t * t; }
inline float out_quad(float t)      { return t * (2.0f - t); }
inline float in_out_quad(float t)   { return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t; }

// Cubic
inline float in_cubic(float t)      { return t * t * t; }
inline float out_cubic(float t)     { float u = t - 1.0f; return u * u * u + 1.0f; }
inline float in_out_cubic(float t)  { return t < 0.5f ? 4.0f * t * t * t : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f; }

// Quartic
inline float in_quart(float t)      { return t * t * t * t; }
inline float out_quart(float t)     { float u = t - 1.0f; return 1.0f - u * u * u * u; }
inline float in_out_quart(float t)  { float u = t - 1.0f; return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - 8.0f * u * u * u * u; }

// Sine
inline float in_sine(float t)       { return 1.0f - std::cos(t * math::HALF_PI); }
inline float out_sine(float t)      { return std::sin(t * math::HALF_PI); }
inline float in_out_sine(float t)   { return 0.5f * (1.0f - std::cos(math::PI * t)); }

// Exponential
inline float in_expo(float t)       { return t == 0.0f ? 0.0f : std::pow(2.0f, 10.0f * (t - 1.0f)); }
inline float out_expo(float t)      { return t == 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t); }
inline float in_out_expo(float t) {
    if (t == 0.0f) return 0.0f;
    if (t == 1.0f) return 1.0f;
    return t < 0.5f ? 0.5f * std::pow(2.0f, 20.0f * t - 10.0f)
                    : 1.0f - 0.5f * std::pow(2.0f, -20.0f * t + 10.0f);
}

// Circular
inline float in_circ(float t)       { return 1.0f - std::sqrt(1.0f - t * t); }
inline float out_circ(float t)      { float u = t - 1.0f; return std::sqrt(1.0f - u * u); }
inline float in_out_circ(float t) {
    if (t < 0.5f) return 0.5f * (1.0f - std::sqrt(1.0f - 4.0f * t * t));
    float u = 2.0f * t - 2.0f;
    return 0.5f * (std::sqrt(1.0f - u * u) + 1.0f);
}

// Back (overshoot)
inline float in_back(float t)       { const float s = 1.70158f; return t * t * ((s + 1.0f) * t - s); }
inline float out_back(float t)      { const float s = 1.70158f; float u = t - 1.0f; return u * u * ((s + 1.0f) * u + s) + 1.0f; }

// Elastic
inline float in_elastic(float t) {
    if (t == 0.0f || t == 1.0f) return t;
    return -std::pow(2.0f, 10.0f * t - 10.0f) * std::sin((t * 10.0f - 10.75f) * math::TWO_PI / 3.0f);
}
inline float out_elastic(float t) {
    if (t == 0.0f || t == 1.0f) return t;
    return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * math::TWO_PI / 3.0f) + 1.0f;
}

// Bounce
inline float out_bounce(float t) {
    if (t < 1.0f / 2.75f)      return 7.5625f * t * t;
    if (t < 2.0f / 2.75f)      { t -= 1.5f / 2.75f; return 7.5625f * t * t + 0.75f; }
    if (t < 2.5f / 2.75f)      { t -= 2.25f / 2.75f; return 7.5625f * t * t + 0.9375f; }
    t -= 2.625f / 2.75f;        return 7.5625f * t * t + 0.984375f;
}
inline float in_bounce(float t) { return 1.0f - out_bounce(1.0f - t); }

} // namespace ease

// ─────────────────────────────────────────────────────────────────────────────
// EaseFunc type
// ─────────────────────────────────────────────────────────────────────────────

using EaseFunc = float(*)(float);

// ─────────────────────────────────────────────────────────────────────────────
// Tween - animates a float value from start to end over duration
// ─────────────────────────────────────────────────────────────────────────────

class Tween {
public:
    Tween() = default;

    Tween(float* target, float start, float end, float duration,
          EaseFunc easing = ease::linear)
        : target_(target), start_(start), end_(end),
          duration_(duration), easing_(easing) {}

    /// Advance time. Returns true if still active.
    bool update(float dt) {
        if (finished_) return false;

        elapsed_ += dt;
        float t = math::clamp(elapsed_ / duration_, 0.0f, 1.0f);
        float eased = easing_(t);

        if (target_) *target_ = math::lerp(start_, end_, eased);

        if (elapsed_ >= duration_) {
            if (target_) *target_ = end_;
            finished_ = true;
            if (on_complete_) on_complete_();
            return false;
        }
        return true;
    }

    bool finished() const { return finished_; }
    float progress() const { return math::clamp(elapsed_ / duration_, 0.0f, 1.0f); }

    Tween& on_complete(std::function<void()> cb) { on_complete_ = std::move(cb); return *this; }
    Tween& set_delay(float delay) { elapsed_ = -delay; return *this; }

private:
    float* target_{nullptr};
    float  start_{0.0f};
    float  end_{1.0f};
    float  duration_{1.0f};
    float  elapsed_{0.0f};
    bool   finished_{false};
    EaseFunc easing_{ease::linear};
    std::function<void()> on_complete_;
};

// ─────────────────────────────────────────────────────────────────────────────
// TweenManager - manages multiple tweens
// ─────────────────────────────────────────────────────────────────────────────

class TweenManager {
public:
    Tween& add(float* target, float start, float end, float duration,
               EaseFunc easing = ease::linear) {
        tweens_.emplace_back(target, start, end, duration, easing);
        return tweens_.back();
    }

    void update(float dt) {
        for (auto it = tweens_.begin(); it != tweens_.end(); ) {
            if (!it->update(dt)) {
                it = tweens_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void clear() { tweens_.clear(); }
    size_t active_count() const { return tweens_.size(); }

private:
    // deque (not vector) so the reference returned by add() stays valid when
    // later add() calls grow the container.
    std::deque<Tween> tweens_;
};

} // namespace nexus::anim
