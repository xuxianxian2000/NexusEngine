#include "nexus/audio/audio_engine.h"
#include "nexus/core/log.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace nexus::audio {

AudioEngine::AudioEngine() {
    // Initialize default buses
    buses_.push_back({"Master", 1.0f, false, {}});
    buses_.push_back({"SFX",    1.0f, false, {}});
    buses_.push_back({"Music",  1.0f, false, {}});
    buses_.push_back({"Voice",  1.0f, false, {}});
}

// ── Clip management ─────────────────────────────────────────────────────────

AudioClipId AudioEngine::load_clip(const std::string& name, const std::string& filepath) {
    AudioBuffer buffer;
    if (!load_audio(filepath, buffer)) return INVALID_CLIP_ID;
    return load_clip_from_buffer(name, std::move(buffer));
}

AudioClipId AudioEngine::load_clip_from_buffer(const std::string& name, AudioBuffer buffer) {
    // clips_ is read by mix() on the audio thread; guard mutations so the
    // vector reallocation can't dangle a clip pointer mid-mix.
    std::lock_guard<std::mutex> lock(mutex_);
    AudioClipId id = next_clip_id_++;
    clips_.push_back({id, name, std::move(buffer)});
    clip_name_map_[name] = id;
    NX_INFO("Audio clip loaded: '{}' (id={}, {:.2f}s)", name, id,
            clips_.back().buffer.duration_seconds());
    return id;
}

const AudioClip* AudioEngine::get_clip(AudioClipId id) const {
    for (const auto& c : clips_) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

const AudioClip* AudioEngine::get_clip_by_name(const std::string& name) const {
    auto it = clip_name_map_.find(name);
    if (it == clip_name_map_.end()) return nullptr;
    return get_clip(it->second);
}

void AudioEngine::unload_clip(AudioClipId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Stop all voices using this clip
    for (auto& v : voices_) {
        if (v.clip_id == id) v.finished = true;
    }
    // Remove from name map
    for (auto it = clip_name_map_.begin(); it != clip_name_map_.end(); ++it) {
        if (it->second == id) { clip_name_map_.erase(it); break; }
    }
    clips_.erase(
        std::remove_if(clips_.begin(), clips_.end(),
            [id](const AudioClip& c) { return c.id == id; }),
        clips_.end());
}

// ── Playback ────────────────────────────────────────────────────────────────

VoiceId AudioEngine::play(AudioClipId clip, float volume, float pitch,
                           bool looping, u32 bus) {
    // Validate and enqueue under the lock: get_clip() reads clips_, which the
    // audio thread mutates, and a concurrent unload must not race the lookup.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!get_clip(clip)) return INVALID_VOICE_ID;

    Voice v;
    v.id = next_voice_id_++;
    v.clip_id = clip;
    v.volume = volume;
    v.pitch = pitch;
    v.looping = looping;
    v.bus_index = bus;

    voices_.push_back(v);
    return v.id;
}

VoiceId AudioEngine::play_spatial(AudioClipId clip, Vec3 position, float volume,
                                   float min_dist, float max_dist,
                                   bool looping, u32 bus) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!get_clip(clip)) return INVALID_VOICE_ID;

    Voice v;
    v.id = next_voice_id_++;
    v.clip_id = clip;
    v.volume = volume;
    v.looping = looping;
    v.spatial = true;
    v.position = position;
    v.min_distance = min_dist;
    v.max_distance = max_dist;
    v.bus_index = bus;

    voices_.push_back(v);
    return v.id;
}

void AudioEngine::pause(VoiceId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& v : voices_) {
        if (v.id == id) { v.paused = true; return; }
    }
}

void AudioEngine::resume(VoiceId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& v : voices_) {
        if (v.id == id) { v.paused = false; return; }
    }
}

void AudioEngine::stop(VoiceId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& v : voices_) {
        if (v.id == id) { v.finished = true; return; }
    }
}

void AudioEngine::stop_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& v : voices_) {
        v.finished = true;
    }
}

void AudioEngine::set_volume(VoiceId id, float volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& v : voices_) {
        if (v.id == id) { v.volume = volume; return; }
    }
}

void AudioEngine::set_pitch(VoiceId id, float pitch) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& v : voices_) {
        if (v.id == id) { v.pitch = pitch; return; }
    }
}

void AudioEngine::set_pan(VoiceId id, float pan) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& v : voices_) {
        if (v.id == id) { v.pan = pan; return; }
    }
}

void AudioEngine::set_position(VoiceId id, Vec3 position) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& v : voices_) {
        if (v.id == id) { v.position = position; return; }
    }
}

void AudioEngine::set_looping(VoiceId id, bool looping) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& v : voices_) {
        if (v.id == id) { v.looping = looping; return; }
    }
}

bool AudioEngine::is_playing(VoiceId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& v : voices_) {
        if (v.id == id) return !v.finished && !v.paused;
    }
    return false;
}

// ── Bus control ─────────────────────────────────────────────────────────────

void AudioEngine::set_bus_volume(u32 bus, float volume) {
    if (bus < buses_.size()) buses_[bus].volume = volume;
}

float AudioEngine::get_bus_volume(u32 bus) const {
    return (bus < buses_.size()) ? buses_[bus].volume : 0.0f;
}

void AudioEngine::set_bus_muted(u32 bus, bool muted) {
    if (bus < buses_.size()) buses_[bus].muted = muted;
}

bool AudioEngine::is_bus_muted(u32 bus) const {
    return (bus < buses_.size()) ? buses_[bus].muted : true;
}

void AudioEngine::set_master_volume(float volume) {
    buses_[BUS_MASTER].volume = volume;
}

float AudioEngine::master_volume() const {
    return buses_[BUS_MASTER].volume;
}

// ── Event system ────────────────────────────────────────────────────────────

void AudioEngine::register_event(const std::string& name, AudioEvent event) {
    event.name = name;
    events_[name] = std::move(event);
}

VoiceId AudioEngine::fire_event(const std::string& name) {
    auto it = events_.find(name);
    if (it == events_.end()) {
        NX_WARN("AudioEngine: unknown event '{}'", name);
        return INVALID_VOICE_ID;
    }
    const auto& e = it->second;
    return play(e.clip_id, e.volume, e.pitch, e.looping, e.bus_index);
}

VoiceId AudioEngine::fire_event_at(const std::string& name, Vec3 position) {
    auto it = events_.find(name);
    if (it == events_.end()) return INVALID_VOICE_ID;
    const auto& e = it->second;
    return play_spatial(e.clip_id, position, e.volume, 1.0f, 50.0f, e.looping, e.bus_index);
}

// ── Spatial audio helpers ───────────────────────────────────────────────────

float AudioEngine::compute_spatial_gain(const Voice& v) const {
    if (!v.spatial) return 1.0f;

    float dist = glm::length(v.position - listener_position_);
    if (dist <= v.min_distance) return 1.0f;
    if (dist >= v.max_distance) return 0.0f;

    // Inverse distance attenuation with rolloff
    float range = v.max_distance - v.min_distance;
    float t = (dist - v.min_distance) / range;
    return 1.0f / (1.0f + v.rolloff * t * (v.max_distance / v.min_distance - 1.0f));
}

float AudioEngine::compute_spatial_pan(const Voice& v) const {
    if (!v.spatial) return v.pan;

    Vec3 to_source = v.position - listener_position_;
    float dist = glm::length(to_source);
    if (dist < math::EPSILON) return 0.0f;

    to_source /= dist;

    // Right vector from listener forward (assuming Y-up)
    Vec3 right = glm::normalize(glm::cross(listener_forward_, Vec3(0.0f, 1.0f, 0.0f)));
    float dot_right = glm::dot(to_source, right);

    return math::clamp(dot_right, -1.0f, 1.0f);
}

// ── Mixing ──────────────────────────────────────────────────────────────────

void AudioEngine::mix(float* output, u32 frames) {
    std::memset(output, 0, frames * 2 * sizeof(float)); // stereo

    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& voice : voices_) {
        if (voice.finished || voice.paused) continue;

        const auto* clip = get_clip(voice.clip_id);
        if (!clip) { voice.finished = true; continue; }

        // Compute gains
        u32 bus_idx = voice.bus_index;
        float bus_vol = (bus_idx < buses_.size() && !buses_[bus_idx].muted)
                        ? buses_[bus_idx].volume : 0.0f;
        float master_vol = (!buses_[BUS_MASTER].muted) ? buses_[BUS_MASTER].volume : 0.0f;
        float spatial_gain = compute_spatial_gain(voice);
        float pan = compute_spatial_pan(voice);

        float final_volume = voice.volume * bus_vol * master_vol * spatial_gain;
        float left_gain  = final_volume * std::sqrt(0.5f * (1.0f - pan));
        float right_gain = final_volume * std::sqrt(0.5f * (1.0f + pan));

        const auto& buf = clip->buffer;
        double pitch_rate = static_cast<double>(voice.pitch) *
                           static_cast<double>(buf.format.sample_rate) /
                           static_cast<double>(output_sample_rate_);

        for (u32 f = 0; f < frames; ++f) {
            u32 src_frame = static_cast<u32>(voice.cursor);
            double frac = voice.cursor - static_cast<double>(src_frame);

            if (src_frame >= buf.frame_count) {
                if (voice.looping) {
                    voice.cursor = std::fmod(voice.cursor, static_cast<double>(buf.frame_count));
                    src_frame = static_cast<u32>(voice.cursor);
                    frac = voice.cursor - static_cast<double>(src_frame);
                } else {
                    voice.finished = true;
                    break;
                }
            }

            // Linear interpolation between samples for pitch shifting
            u32 next_frame = src_frame + 1;
            if (next_frame >= buf.frame_count) {
                next_frame = voice.looping ? 0 : src_frame;
            }

            float s0_l = buf.read_sample(src_frame, 0);
            float s1_l = buf.read_sample(next_frame, 0);
            float sample_l = s0_l + static_cast<float>(frac) * (s1_l - s0_l);

            float sample_r;
            if (buf.format.channels >= 2) {
                float s0_r = buf.read_sample(src_frame, 1);
                float s1_r = buf.read_sample(next_frame, 1);
                sample_r = s0_r + static_cast<float>(frac) * (s1_r - s0_r);
            } else {
                sample_r = sample_l;
            }

            output[f * 2 + 0] += sample_l * left_gain;
            output[f * 2 + 1] += sample_r * right_gain;

            voice.cursor += pitch_rate;
        }
    }

    // Soft clipping using tanh for musical saturation instead of hard clipping
    for (u32 i = 0; i < frames * 2; ++i) {
        if (output[i] > 1.0f || output[i] < -1.0f) {
            output[i] = std::tanh(output[i]);
        }
    }
}

// ── Update ──────────────────────────────────────────────────────────────────

void AudioEngine::update() {
    std::lock_guard<std::mutex> lock(mutex_);
    voices_.erase(
        std::remove_if(voices_.begin(), voices_.end(),
            [](const Voice& v) { return v.finished; }),
        voices_.end());
}

u32 AudioEngine::active_voice_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    u32 count = 0;
    for (const auto& v : voices_) {
        if (!v.finished) ++count;
    }
    return count;
}

} // namespace nexus::audio
