#include "nexus/animation/animation_state_machine.h"
#include "nexus/core/log.h"
#include <cmath>

namespace nexus::anim {

void AnimationStateMachine::add_state(const std::string& name, AnimationClip* clip,
                                       float speed, bool looping) {
    states_.push_back({name, clip, speed, looping});
}

void AnimationStateMachine::add_transition(const std::string& from, const std::string& to,
                                            float blend_duration,
                                            std::function<bool()> condition) {
    transitions_.push_back({from, to, blend_duration, std::move(condition)});
}

void AnimationStateMachine::set_state(const std::string& name) {
    if (find_state(name)) {
        current_name_ = name;
        current_time_ = 0.0f;
        transitioning_ = false;
    }
}

void AnimationStateMachine::update(float dt, const Skeleton& skeleton,
                                    std::vector<BonePose>& out_pose) {
    if (current_name_.empty()) return;

    auto* current = find_state(current_name_);
    if (!current || !current->clip) return;

    // Check transitions
    if (!transitioning_) {
        for (const auto& t : transitions_) {
            if (t.from == current_name_ && t.condition && t.condition()) {
                if (find_state(t.to)) {
                    transitioning_ = true;
                    target_name_ = t.to;
                    target_time_ = 0.0f;
                    blend_elapsed_ = 0.0f;
                    blend_duration_ = t.blend_duration;
                    break;
                }
            }
        }
    }

    // Advance current time
    float prev_time = current_time_;
    current_time_ += dt * current->speed;
    if (current->clip->duration() > 0.0f) {
        if (current->looping) {
            current_time_ = std::fmod(current_time_, current->clip->duration());
        } else {
            current_time_ = std::min(current_time_, current->clip->duration());
        }
    }

    // Fire animation events
    if (event_callback_ && current->clip) {
        std::vector<const AnimationEvent*> fired;
        current->clip->collect_events(prev_time, current_time_, current->looping, fired);
        for (const auto* ev : fired) {
            event_callback_(ev->name);
        }
    }

    // Sample current
    out_pose.resize(skeleton.bone_count());
    auto bind_pose = skeleton.get_bind_pose();
    out_pose = bind_pose;
    current->clip->sample(current_time_, out_pose);

    // Handle transition blending
    if (transitioning_) {
        auto* target = find_state(target_name_);
        if (target && target->clip) {
            target_time_ += dt * target->speed;
            if (target->clip->duration() > 0.0f) {
                if (target->looping) {
                    target_time_ = std::fmod(target_time_, target->clip->duration());
                } else {
                    // Clamp non-looping clips so a completed transition doesn't
                    // leave current_time_ past the clip's duration.
                    target_time_ = std::min(target_time_, target->clip->duration());
                }
            }

            std::vector<BonePose> target_pose = bind_pose;
            target->clip->sample(target_time_, target_pose);

            blend_elapsed_ += dt;
            float t = math::clamp(blend_elapsed_ / blend_duration_, 0.0f, 1.0f);

            AnimationClip::blend(out_pose, target_pose, t, out_pose);

            if (blend_elapsed_ >= blend_duration_) {
                current_name_ = target_name_;
                current_time_ = target_time_;
                transitioning_ = false;
                out_pose = target_pose;
            }
        }
    }
}

void AnimationStateMachine::set_float(const std::string& name, float value) {
    float_params_[name] = value;
}

void AnimationStateMachine::set_bool(const std::string& name, bool value) {
    bool_params_[name] = value;
}

float AnimationStateMachine::get_float(const std::string& name) const {
    auto it = float_params_.find(name);
    return (it != float_params_.end()) ? it->second : 0.0f;
}

bool AnimationStateMachine::get_bool(const std::string& name) const {
    auto it = bool_params_.find(name);
    return (it != bool_params_.end()) ? it->second : false;
}

AnimationState* AnimationStateMachine::find_state(const std::string& name) {
    for (auto& s : states_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

} // namespace nexus::anim
