#include "nexus/net/prediction.h"
#include <algorithm>
#include <cstring>

namespace nexus::net {

// ── ClientPrediction ────────────────────────────────────────────────────────

void ClientPrediction::record_input(const std::vector<u8>& input_data,
                                      const std::vector<u8>& predicted_state) {
    InputSnapshot snapshot;
    snapshot.sequence = next_sequence_++;
    snapshot.input_data = input_data;

    pending_inputs_.push_back(std::move(snapshot));
    predicted_state_ = predicted_state;
}

bool ClientPrediction::reconcile(u32 acked_sequence,
                                   const std::vector<u8>& server_state) {
    // Remove all inputs up to and including the acked sequence
    while (!pending_inputs_.empty() &&
           pending_inputs_.front().sequence <= acked_sequence) {
        pending_inputs_.pop_front();
    }

    // Check if our prediction matches the server state
    bool needs_correction = false;
    if (states_equal_) {
        needs_correction = !states_equal_(predicted_state_, server_state);
    } else {
        needs_correction = (predicted_state_ != server_state);
    }

    if (needs_correction) {
        corrections_++;

        // Start from server state and re-apply all pending inputs
        std::vector<u8> state = server_state;

        if (apply_input_) {
            for (auto& input : pending_inputs_) {
                state = apply_input_(state, input.input_data);
            }
        }

        predicted_state_ = state;
        return true;
    }

    return false;
}

void ClientPrediction::reset() {
    pending_inputs_.clear();
    predicted_state_.clear();
    next_sequence_ = 1;
    corrections_ = 0;
}

// ── InterpolationBuffer ─────────────────────────────────────────────────────

void InterpolationBuffer::push_state(f32 server_time,
                                       const std::vector<u8>& state) {
    buffer_.push_back({server_time, state});

    // Keep buffer bounded
    while (buffer_.size() > MAX_BUFFER_SIZE) {
        buffer_.pop_front();
    }
}

std::vector<u8> InterpolationBuffer::sample(f32 render_time) const {
    if (buffer_.empty()) return {};

    // We render at (render_time - delay_) to ensure we have data
    f32 target_time = render_time - delay_;

    // Find the two states to interpolate between
    if (buffer_.size() == 1) {
        return buffer_.front().state;
    }

    // If target is before all samples, return oldest
    if (target_time <= buffer_.front().timestamp) {
        return buffer_.front().state;
    }

    // If target is after all samples, return newest
    if (target_time >= buffer_.back().timestamp) {
        return buffer_.back().state;
    }

    // Find the two bounding states
    for (u32 i = 0; i + 1 < buffer_.size(); i++) {
        const auto& from = buffer_[i];
        const auto& to = buffer_[i + 1];

        if (target_time >= from.timestamp && target_time <= to.timestamp) {
            f32 duration = to.timestamp - from.timestamp;
            f32 t = (duration > 0.0001f) ?
                    (target_time - from.timestamp) / duration : 0.0f;

            if (interpolate_) {
                return interpolate_(from.state, to.state, t);
            }

            // Default interpolation: byte-wise lerp for equal-sized states
            if (from.state.size() == to.state.size() && !from.state.empty()) {
                std::vector<u8> result(from.state.size());
                // Treat data as array of floats if size is aligned, otherwise byte lerp
                if (from.state.size() % sizeof(float) == 0) {
                    size_t count = from.state.size() / sizeof(float);
                    // memcpy in/out of float temporaries: the u8 buffers carry no
                    // alignment guarantee, so reinterpret_cast'ing to float* is UB.
                    for (size_t j = 0; j < count; ++j) {
                        float a, b;
                        std::memcpy(&a, from.state.data() + j * sizeof(float), sizeof(float));
                        std::memcpy(&b, to.state.data() + j * sizeof(float), sizeof(float));
                        float r = a + (b - a) * t;
                        std::memcpy(result.data() + j * sizeof(float), &r, sizeof(float));
                    }
                } else {
                    for (size_t j = 0; j < result.size(); ++j) {
                        result[j] = static_cast<u8>(
                            static_cast<float>(from.state[j]) +
                            (static_cast<float>(to.state[j]) - static_cast<float>(from.state[j])) * t);
                    }
                }
                return result;
            }

            // Fallback: return the closer state
            return (t < 0.5f) ? from.state : to.state;
        }
    }

    return buffer_.back().state;
}

void InterpolationBuffer::clear() {
    buffer_.clear();
}

} // namespace nexus::net
