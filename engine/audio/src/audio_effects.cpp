#include "nexus/audio/audio_effects.h"
#include <cstring>
#include <cmath>

namespace nexus::audio {

// ─────────────────────────────────────────────────────────────────────────────
// LowPassFilter — first-order RC low-pass filter
//
//   alpha = dt / (RC + dt)  where  RC = 1 / (2*pi*cutoff)
//   y[n] = y[n-1] + alpha * (x[n] - y[n-1])
// ─────────────────────────────────────────────────────────────────────────────

void LowPassFilter::process(float* samples, u32 frame_count, u32 sample_rate) {
    if (!enabled) return;

    const float dt = 1.0f / static_cast<float>(sample_rate);
    const float rc = 1.0f / (2.0f * static_cast<float>(M_PI) * cutoff_hz_);
    const float alpha = dt / (rc + dt);

    for (u32 i = 0; i < frame_count; ++i) {
        float in_l = samples[i * 2 + 0];
        float in_r = samples[i * 2 + 1];

        prev_left_  += alpha * (in_l - prev_left_);
        prev_right_ += alpha * (in_r - prev_right_);

        // Mix dry/wet according to the mix parameter
        samples[i * 2 + 0] = in_l + mix * (prev_left_  - in_l);
        samples[i * 2 + 1] = in_r + mix * (prev_right_ - in_r);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// HighPassFilter — first-order high-pass filter
//
//   alpha = RC / (RC + dt)
//   y[n] = alpha * (y[n-1] + x[n] - x[n-1])
// ─────────────────────────────────────────────────────────────────────────────

void HighPassFilter::process(float* samples, u32 frame_count, u32 sample_rate) {
    if (!enabled) return;

    const float dt = 1.0f / static_cast<float>(sample_rate);
    const float rc = 1.0f / (2.0f * static_cast<float>(M_PI) * cutoff_hz_);
    const float alpha = rc / (rc + dt);

    for (u32 i = 0; i < frame_count; ++i) {
        float in_l = samples[i * 2 + 0];
        float in_r = samples[i * 2 + 1];

        float out_l = alpha * (prev_out_left_  + in_l - prev_in_left_);
        float out_r = alpha * (prev_out_right_ + in_r - prev_in_right_);

        prev_in_left_   = in_l;
        prev_in_right_  = in_r;
        prev_out_left_  = out_l;
        prev_out_right_ = out_r;

        // Mix dry/wet
        samples[i * 2 + 0] = in_l + mix * (out_l - in_l);
        samples[i * 2 + 1] = in_r + mix * (out_r - in_r);
    }
}

void HighPassFilter::reset() {
    prev_in_left_   = 0.0f;
    prev_in_right_  = 0.0f;
    prev_out_left_  = 0.0f;
    prev_out_right_ = 0.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReverbEffect — Schroeder reverb
//
// Uses 4 parallel comb filters (per channel) fed into 2 series allpass filters.
// Comb delay lengths are tuned for 44100 Hz and scaled proportionally for other
// sample rates.  Left and right channels use slightly offset delay lengths to
// produce a stereo spread.
// ─────────────────────────────────────────────────────────────────────────────

// Canonical Schroeder comb delay lengths at 44100 Hz
static constexpr u32 COMB_DELAYS_L[4] = { 1116, 1188, 1277, 1356 };
static constexpr u32 COMB_DELAYS_R[4] = { 1139, 1211, 1300, 1379 }; // offset +23

static constexpr u32 ALLPASS_DELAYS_L[2] = { 556, 441 };
static constexpr u32 ALLPASS_DELAYS_R[2] = { 579, 464 }; // offset +23

static u32 scale_delay(u32 base_delay, u32 sample_rate) {
    u32 scaled = static_cast<u32>(static_cast<float>(base_delay) *
                                  static_cast<float>(sample_rate) / 44100.0f);
    // Never return 0: the comb/allpass buffers index buffer[index] and an empty
    // buffer would be an out-of-bounds access at very low sample rates.
    return scaled > 0 ? scaled : 1;
}

// ── CombFilter ──────────────────────────────────────────────────────────────

void ReverbEffect::CombFilter::init(u32 size) {
    buffer.assign(size, 0.0f);
    index = 0;
    filter_store = 0.0f;
}

float ReverbEffect::CombFilter::process(float input) {
    float output = buffer[index];

    // One-pole low-pass on the feedback path (damping)
    filter_store = output * damp2 + filter_store * damp1;

    buffer[index] = input + filter_store * feedback;
    if (++index >= static_cast<u32>(buffer.size())) index = 0;

    return output;
}

// ── AllpassFilter ───────────────────────────────────────────────────────────

void ReverbEffect::AllpassFilter::init(u32 size) {
    buffer.assign(size, 0.0f);
    index = 0;
}

float ReverbEffect::AllpassFilter::process(float input) {
    float buffered = buffer[index];
    float output = -input + buffered;

    buffer[index] = input + buffered * feedback;
    if (++index >= static_cast<u32>(buffer.size())) index = 0;

    return output;
}

// ── ReverbEffect public interface ───────────────────────────────────────────

ReverbEffect::ReverbEffect()
    : config_() {}

ReverbEffect::ReverbEffect(const Config& config)
    : config_(config) {}

void ReverbEffect::init_filters(u32 sample_rate) {
    for (u32 i = 0; i < NUM_COMBS; ++i) {
        combs_left_[i].init(scale_delay(COMB_DELAYS_L[i], sample_rate));
        combs_right_[i].init(scale_delay(COMB_DELAYS_R[i], sample_rate));
    }
    for (u32 i = 0; i < NUM_ALLPASS; ++i) {
        allpass_left_[i].init(scale_delay(ALLPASS_DELAYS_L[i], sample_rate));
        allpass_right_[i].init(scale_delay(ALLPASS_DELAYS_R[i], sample_rate));
        allpass_left_[i].feedback  = 0.5f;
        allpass_right_[i].feedback = 0.5f;
    }
    last_sample_rate_ = sample_rate;
    initialized_ = true;
    update_params();
}

void ReverbEffect::update_params() {
    // Map room_size 0..1  to feedback roughly 0.7..0.98
    float feedback = 0.7f + config_.room_size * 0.28f;

    // Damping: damp1 controls the one-pole LP coefficient in the feedback path
    float damp1 = config_.damping * 0.4f;
    float damp2 = 1.0f - damp1;

    for (u32 i = 0; i < NUM_COMBS; ++i) {
        combs_left_[i].feedback  = feedback;
        combs_right_[i].feedback = feedback;
        combs_left_[i].damp1     = damp1;
        combs_right_[i].damp1    = damp1;
        combs_left_[i].damp2     = damp2;
        combs_right_[i].damp2    = damp2;
    }
}

void ReverbEffect::process(float* samples, u32 frame_count, u32 sample_rate) {
    if (!enabled) return;

    // Lazily initialise or re-initialise if sample rate changed
    if (!initialized_ || last_sample_rate_ != sample_rate) {
        init_filters(sample_rate);
    }

    for (u32 i = 0; i < frame_count; ++i) {
        float in_l = samples[i * 2 + 0];
        float in_r = samples[i * 2 + 1];

        // Feed the same mono input into all comb filters for a denser reverb
        float input = (in_l + in_r) * 0.5f;

        // Sum the parallel comb filter outputs
        float wet_l = 0.0f;
        float wet_r = 0.0f;
        for (u32 c = 0; c < NUM_COMBS; ++c) {
            wet_l += combs_left_[c].process(input);
            wet_r += combs_right_[c].process(input);
        }

        // Feed through series allpass filters for diffusion
        for (u32 a = 0; a < NUM_ALLPASS; ++a) {
            wet_l = allpass_left_[a].process(wet_l);
            wet_r = allpass_right_[a].process(wet_r);
        }

        // Mix dry and wet signals
        samples[i * 2 + 0] = in_l * config_.dry + wet_l * config_.wet;
        samples[i * 2 + 1] = in_r * config_.dry + wet_r * config_.wet;
    }
}

void ReverbEffect::reset() {
    initialized_ = false;
    last_sample_rate_ = 0;
    for (u32 i = 0; i < NUM_COMBS; ++i) {
        combs_left_[i]  = CombFilter{};
        combs_right_[i] = CombFilter{};
    }
    for (u32 i = 0; i < NUM_ALLPASS; ++i) {
        allpass_left_[i]  = AllpassFilter{};
        allpass_right_[i] = AllpassFilter{};
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// AudioEffectChain
// ─────────────────────────────────────────────────────────────────────────────

void AudioEffectChain::add_effect(std::unique_ptr<AudioEffect> effect) {
    effects_.push_back(std::move(effect));
}

void AudioEffectChain::remove_effect(u32 index) {
    if (index < effects_.size()) {
        effects_.erase(effects_.begin() + static_cast<ptrdiff_t>(index));
    }
}

void AudioEffectChain::clear() {
    effects_.clear();
}

void AudioEffectChain::process(float* samples, u32 frame_count, u32 sample_rate) {
    for (auto& effect : effects_) {
        if (effect && effect->enabled) {
            effect->process(samples, frame_count, sample_rate);
        }
    }
}

void AudioEffectChain::reset() {
    for (auto& effect : effects_) {
        if (effect) {
            effect->reset();
        }
    }
}

} // namespace nexus::audio
