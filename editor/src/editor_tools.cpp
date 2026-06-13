#include "nexus/editor/editor_tools.h"
#include "nexus/editor/imgui_layer.h"
#include "nexus/scene/scene.h"
#include "nexus/scene/components.h"
#include "nexus/scene/registry.h"
#include "nexus/core/log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <queue>

namespace nexus::editor {

// ============================================================================
// TilemapEditorPanel
// ============================================================================

void TilemapEditorPanel::on_render() {
    namespace ui = nexus::editor::imgui;

    ui::begin_window("Tilemap Editor");

    if (!has_target_ || !scene_) {
        ui::text("Select an entity with a TilemapComponent");
        ui::end_window();
        return;
    }

    auto& registry = scene_->registry();
    Entity target = static_cast<Entity>(target_);

    if (!registry.alive(target) || !registry.has_component<TilemapComponent>(target)) {
        ui::text("Entity has no TilemapComponent");
        ui::end_window();
        return;
    }

    auto& tilemap = registry.get_component<TilemapComponent>(target);

    // ── Toolbar ─────────────────────────────────────────────────────────
    if (ui::button("Paint"))  brush_mode_ = TilemapBrushMode::Paint;
    ui::same_line();
    if (ui::button("Erase"))  brush_mode_ = TilemapBrushMode::Erase;
    ui::same_line();
    if (ui::button("Fill"))   brush_mode_ = TilemapBrushMode::Fill;
    ui::same_line();
    if (ui::button("Rect"))   brush_mode_ = TilemapBrushMode::Rectangle;
    ui::same_line();
    if (ui::button("Pick"))   brush_mode_ = TilemapBrushMode::Pick;

    // Brush settings
    float brush_f = static_cast<float>(brush_size_);
    if (ui::input_float("Brush Size", &brush_f)) {
        brush_size_ = static_cast<u32>(std::max(1.0f, brush_f));
    }
    ui::checkbox("Show Grid", &show_grid_);
    ui::separator();

    // ── Tile palette ────────────────────────────────────────────────────
    if (ui::collapsing_header("Tile Palette")) {
        u32 palette_cols = tilemap.tiles_per_row;
        u32 palette_rows = tilemap.tiles_per_col;

        for (u32 row = 0; row < std::min(palette_rows, 8u); ++row) {
            for (u32 col = 0; col < std::min(palette_cols, 16u); ++col) {
                i32 tile_id = static_cast<i32>(row * palette_cols + col);
                bool is_selected = (selected_tile_ == tile_id);

                char label[32];
                std::snprintf(label, sizeof(label), "%d##tile_%d", tile_id, tile_id);
                if (ui::tree_node(label, is_selected, true)) {
                    // leaf, no pop needed
                }
                if (ui::is_item_clicked()) {
                    selected_tile_ = tile_id;
                }
                if (col + 1 < std::min(palette_cols, 16u)) {
                    ui::same_line();
                }
            }
        }
    }

    // ── Tilemap info ────────────────────────────────────────────────────
    ui::separator();
    ui::text("Map: %ux%u  Tile size: %.1f", tilemap.width, tilemap.height, tilemap.tile_size);
    ui::text("Selected tile: %d  Brush: %s",
             selected_tile_,
             brush_mode_ == TilemapBrushMode::Paint ? "Paint" :
             brush_mode_ == TilemapBrushMode::Erase ? "Erase" :
             brush_mode_ == TilemapBrushMode::Fill  ? "Fill" :
             brush_mode_ == TilemapBrushMode::Rectangle ? "Rectangle" : "Pick");

    ui::end_window();
}

void TilemapEditorPanel::paint_at(u32 x, u32 y) {
    if (!has_target_ || !scene_) return;
    auto& registry = scene_->registry();
    Entity target = static_cast<Entity>(target_);
    if (!registry.alive(target) || !registry.has_component<TilemapComponent>(target)) return;

    auto& tilemap = registry.get_component<TilemapComponent>(target);

    i32 tile_to_paint = (brush_mode_ == TilemapBrushMode::Erase) ? -1 : selected_tile_;

    i32 half = static_cast<i32>(brush_size_ / 2);
    for (i32 dy = 0; dy < static_cast<i32>(brush_size_); ++dy) {
        for (i32 dx = 0; dx < static_cast<i32>(brush_size_); ++dx) {
            // Use signed math so a brush centered near the origin doesn't wrap
            // around to ~4 billion and clip asymmetrically.
            i32 tx = static_cast<i32>(x) + dx - half;
            i32 ty = static_cast<i32>(y) + dy - half;
            if (tx < 0 || ty < 0) continue;
            tilemap.set_tile(static_cast<u32>(tx), static_cast<u32>(ty), tile_to_paint);
        }
    }
}

void TilemapEditorPanel::flood_fill(u32 x, u32 y, i32 new_tile) {
    if (!has_target_ || !scene_) return;
    auto& registry = scene_->registry();
    Entity target = static_cast<Entity>(target_);
    if (!registry.alive(target) || !registry.has_component<TilemapComponent>(target)) return;

    auto& tilemap = registry.get_component<TilemapComponent>(target);
    if (x >= tilemap.width || y >= tilemap.height) return;

    i32 old_tile = tilemap.get_tile(x, y);
    if (old_tile == new_tile) return;

    // BFS flood fill
    std::queue<std::pair<u32, u32>> queue;
    queue.push({x, y});
    tilemap.set_tile(x, y, new_tile);

    while (!queue.empty()) {
        auto [cx, cy] = queue.front();
        queue.pop();

        auto try_fill = [&](u32 nx, u32 ny) {
            if (nx < tilemap.width && ny < tilemap.height &&
                tilemap.get_tile(nx, ny) == old_tile) {
                tilemap.set_tile(nx, ny, new_tile);
                queue.push({nx, ny});
            }
        };

        if (cx > 0) try_fill(cx - 1, cy);
        if (cx + 1 < tilemap.width) try_fill(cx + 1, cy);
        if (cy > 0) try_fill(cx, cy - 1);
        if (cy + 1 < tilemap.height) try_fill(cx, cy + 1);
    }
}

// ============================================================================
// AnimationEditorPanel
// ============================================================================

void AnimationEditorPanel::on_render() {
    namespace ui = nexus::editor::imgui;

    ui::begin_window("Animation");

    // ── Transport controls ──────────────────────────────────────────────
    if (ui::button(playing_ ? "||" : ">")) toggle_play();
    ui::same_line();
    if (ui::button("|<")) { current_time_ = 0.0f; }
    ui::same_line();
    if (ui::button(">|")) { current_time_ = duration_; }
    ui::same_line();

    float time = current_time_;
    if (ui::input_float("Time", &time)) {
        current_time_ = std::clamp(time, 0.0f, duration_);
    }
    ui::same_line();

    float dur = duration_;
    if (ui::input_float("Duration", &dur)) {
        duration_ = std::max(0.01f, dur);
    }

    // Mode toggle
    if (ui::button(mode_ == TimelineMode::Dopesheet ? "Dopesheet" : "Curves")) {
        mode_ = (mode_ == TimelineMode::Dopesheet) ? TimelineMode::CurveEditor : TimelineMode::Dopesheet;
    }
    ui::separator();

    // ── Track list ──────────────────────────────────────────────────────
    ui::begin_child("Tracks", 150.0f, true);

    for (u32 i = 0; i < tracks_.size(); ++i) {
        bool is_selected = (selected_track_ == static_cast<i32>(i));
        char label[128];
        std::snprintf(label, sizeof(label), "%s (%zu keys)##track_%u",
                      tracks_[i].property_name.c_str(), tracks_[i].keyframes.size(), i);
        if (ui::tree_node(label, is_selected, true)) {
            // leaf
        }
        if (ui::is_item_clicked()) {
            selected_track_ = static_cast<i32>(i);
        }
    }

    ui::end_child();

    // ── Keyframe controls ───────────────────────────────────────────────
    if (selected_track_ >= 0 && selected_track_ < static_cast<i32>(tracks_.size())) {
        if (ui::button("Add Key")) {
            float val = sample_track(tracks_[static_cast<u32>(selected_track_)], current_time_);
            add_keyframe(static_cast<u32>(selected_track_), val);
        }
        ui::same_line();
        if (ui::button("Delete Key")) {
            delete_keyframe(static_cast<u32>(selected_track_));
        }
    }

    ui::end_window();
}

void AnimationEditorPanel::add_keyframe(u32 track_index, float value) {
    if (track_index >= tracks_.size()) return;

    AnimKeyframe key;
    key.time = current_time_;
    key.value = value;

    auto& keys = tracks_[track_index].keyframes;

    // Insert sorted by time
    auto it = std::lower_bound(keys.begin(), keys.end(), key,
        [](const AnimKeyframe& a, const AnimKeyframe& b) { return a.time < b.time; });

    // Replace if same time
    if (it != keys.end() && std::abs(it->time - key.time) < 0.001f) {
        it->value = value;
    } else {
        keys.insert(it, key);
    }
}

void AnimationEditorPanel::delete_keyframe(u32 track_index) {
    if (track_index >= tracks_.size()) return;
    auto& keys = tracks_[track_index].keyframes;
    if (keys.empty()) return;

    // Find nearest keyframe to current_time
    float min_dist = std::numeric_limits<float>::max();
    u32 nearest = 0;
    for (u32 i = 0; i < keys.size(); ++i) {
        float d = std::abs(keys[i].time - current_time_);
        if (d < min_dist) { min_dist = d; nearest = i; }
    }
    keys.erase(keys.begin() + nearest);
}

float AnimationEditorPanel::sample_track(const AnimTrack& track, float t) {
    if (track.keyframes.empty()) return 0.0f;
    if (track.keyframes.size() == 1) return track.keyframes[0].value;

    // Clamp to track bounds
    if (t <= track.keyframes.front().time) return track.keyframes.front().value;
    if (t >= track.keyframes.back().time) return track.keyframes.back().value;

    // Find surrounding keyframes
    for (u32 i = 0; i + 1 < track.keyframes.size(); ++i) {
        const auto& k0 = track.keyframes[i];
        const auto& k1 = track.keyframes[i + 1];
        if (t >= k0.time && t <= k1.time) {
            float dt = k1.time - k0.time;
            if (dt < 0.0001f) return k0.value;
            float u = (t - k0.time) / dt;

            // Cubic Hermite interpolation
            float u2 = u * u;
            float u3 = u2 * u;
            float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
            float h10 = u3 - 2.0f * u2 + u;
            float h01 = -2.0f * u3 + 3.0f * u2;
            float h11 = u3 - u2;

            return h00 * k0.value + h10 * dt * k0.out_tangent
                 + h01 * k1.value + h11 * dt * k1.in_tangent;
        }
    }
    return track.keyframes.back().value;
}

// ============================================================================
// ParticleEditorPanel
// ============================================================================

void ParticleEditorPanel::on_render() {
    namespace ui = nexus::editor::imgui;

    ui::begin_window("Particle Editor");

    // ── Preset management ───────────────────────────────────────────────
    if (ui::collapsing_header("Presets")) {
        for (u32 i = 0; i < presets_.size(); ++i) {
            bool sel = (selected_preset_ == static_cast<i32>(i));
            char label[128];
            std::snprintf(label, sizeof(label), "%s##preset_%u", presets_[i].name.c_str(), i);
            if (ui::tree_node(label, sel, true)) {
                // leaf
            }
            if (ui::is_item_clicked()) {
                load_preset(i);
            }
        }
        if (ui::button("Save as Preset")) {
            ParticlePreset p = current_;
            char name[32];
            std::snprintf(name, sizeof(name), "Preset %zu", presets_.size());
            p.name = name;
            presets_.push_back(p);
        }
    }

    ui::separator();

    // ── Emitter properties ──────────────────────────────────────────────
    if (ui::collapsing_header("Emission")) {
        ui::input_float("Rate", &current_.emission_rate);
        ui::input_vec3("Direction", &current_.direction);
        ui::input_float("Spread (deg)", &current_.spread);
        ui::input_float("Min Speed", &current_.min_speed);
        ui::input_float("Max Speed", &current_.max_speed);
    }

    if (ui::collapsing_header("Lifetime")) {
        ui::input_float("Min Lifetime", &current_.min_lifetime);
        ui::input_float("Max Lifetime", &current_.max_lifetime);
        ui::input_float("Gravity", &current_.gravity);
    }

    if (ui::collapsing_header("Appearance")) {
        ui::input_color4("Start Color", &current_.start_color);
        ui::input_color4("End Color", &current_.end_color);
        ui::input_float("Start Size", &current_.start_size);
        ui::input_float("End Size", &current_.end_size);
        ui::checkbox("Additive Blend", &current_.additive);
    }

    ui::separator();

    // ── Preview controls ────────────────────────────────────────────────
    ui::checkbox("Preview Active", &preview_active_);
    ui::same_line();
    ui::text("Alive: %u", alive_count_);

    ui::end_window();
}

void ParticleEditorPanel::load_preset(u32 index) {
    if (index < presets_.size()) {
        current_ = presets_[index];
        selected_preset_ = static_cast<i32>(index);
    }
}

// ============================================================================
// MaterialEditorPanel
// ============================================================================

void MaterialEditorPanel::on_render() {
    namespace ui = nexus::editor::imgui;

    ui::begin_window("Material Editor");

    // Material name and shader
    ui::text("Material: %s", material_name_.empty() ? "(none)" : material_name_.c_str());
    ui::text("Shader: %s", shader_name_.empty() ? "(default)" : shader_name_.c_str());
    ui::separator();

    // ── Property editors ────────────────────────────────────────────────
    for (auto& prop : properties_) {
        switch (prop.type) {
            case MaterialProperty::Float:
                if (ui::input_float(prop.name.c_str(), &prop.float_val)) {
                    modified_ = true;
                }
                break;

            case MaterialProperty::Vec2:
                if (ui::input_vec2(prop.name.c_str(), &prop.vec2_val)) {
                    modified_ = true;
                }
                break;

            case MaterialProperty::Vec3:
                if (ui::input_vec3(prop.name.c_str(), &prop.vec3_val)) {
                    modified_ = true;
                }
                break;

            case MaterialProperty::Vec4:
                if (ui::input_vec4(prop.name.c_str(), &prop.vec4_val)) {
                    modified_ = true;
                }
                break;

            case MaterialProperty::Color:
                if (ui::input_color4(prop.name.c_str(), &prop.vec4_val)) {
                    modified_ = true;
                }
                break;

            case MaterialProperty::Texture: {
                ui::text("%s: %s", prop.name.c_str(),
                        prop.texture_path.empty() ? "(none)" : prop.texture_path.c_str());
                break;
            }

            case MaterialProperty::Bool:
                if (ui::checkbox(prop.name.c_str(), &prop.bool_val)) {
                    modified_ = true;
                }
                break;
        }
    }

    // ── Save button ─────────────────────────────────────────────────────
    if (modified_) {
        ui::separator();
        if (ui::button("Save Material")) {
            if (on_save_) {
                on_save_(properties_);
            }
            modified_ = false;
        }
    }

    ui::end_window();
}

} // namespace nexus::editor
