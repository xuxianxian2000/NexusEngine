#include "nexus/editor/editor_panels.h"
#include "nexus/editor/imgui_layer.h"
#include "nexus/scene/scene.h"
#include "nexus/scene/components.h"
#include "nexus/scene/registry.h"
#include "nexus/scene/hierarchy.h"
#include "nexus/renderer/forward_renderer_3d.h"
#include "nexus/renderer/batch_renderer_2d.h"
#include "nexus/renderer/camera.h"
#include "nexus/core/log.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace nexus::editor {

// ── ViewportPanel ──────────────────────────────────────────────────────────

void ViewportPanel::on_render() {
    if (!scene_) return;

    auto& registry = scene_->registry();

    // ── 3D rendering pass ──────────────────────────────────────────────
    if (renderer_3d_) {
        // Find camera
        Camera3D cam;
        bool found_camera = false;
        registry.each<CameraComponent, Transform3DComponent>(
            [&](u32 /*entity*/, CameraComponent& cc, Transform3DComponent& tc) {
                if (found_camera) return;
                cam.fov = cc.fov;
                cam.near_clip = cc.near_clip;
                cam.far_clip = cc.far_clip;
                // Convert quaternion to yaw/pitch
                Quat q = cc.orientation;
                float sinp = 2.0f * (q.w * q.x - q.z * q.y);
                cam.pitch = std::abs(sinp) >= 1.0f
                    ? std::copysign(90.0f, sinp)
                    : static_cast<float>(std::asin(sinp) * 180.0 / 3.14159265358979);
                cam.yaw = static_cast<float>(std::atan2(
                    2.0f * (q.w * q.y + q.x * q.z),
                    1.0f - 2.0f * (q.x * q.x + q.y * q.y)) * 180.0 / 3.14159265358979);
                cam.position = tc.world_matrix[3];
                float aspect = (height_ > 0) ? static_cast<float>(width_) / static_cast<float>(height_) : 16.0f / 9.0f;
                cam.set_perspective(aspect);
                found_camera = true;
            });

        if (found_camera) {
            renderer_3d_->begin_frame(cam);

            // Set directional lights
            registry.each<DirectionalLightComponent>(
                [&](u32 /*entity*/, DirectionalLightComponent& dl) {
                    DirectionalLight light;
                    light.direction = dl.direction;
                    light.color = dl.color;
                    light.intensity = dl.intensity;
                    renderer_3d_->set_directional_light(light);
                });

            // Add point lights
            registry.each<PointLightComponent, Transform3DComponent>(
                [&](u32 /*entity*/, PointLightComponent& pl, Transform3DComponent& tc) {
                    PointLight light;
                    light.position = Vec3(tc.world_matrix[3]);
                    light.color = pl.color;
                    light.intensity = pl.intensity;
                    light.radius = pl.radius;
                    renderer_3d_->add_point_light(light);
                });

            // Draw meshes
            registry.each<MeshRendererComponent, Transform3DComponent>(
                [&](u32 /*entity*/, MeshRendererComponent& mr, Transform3DComponent& tc) {
                    if (mr.mesh_id != 0) {
                        // Mesh rendering would use the cached meshes
                        (void)tc;
                    }
                });

            renderer_3d_->end_frame();
        }
    }

    // ── 2D rendering pass ──────────────────────────────────────────────
    if (renderer_2d_) {
        Camera2D cam2d;
        cam2d.set_projection(static_cast<float>(width_), static_cast<float>(height_));
        renderer_2d_->begin(cam2d);
        registry.each<SpriteRendererComponent, Transform2DComponent>(
            [&](u32 /*entity*/, SpriteRendererComponent& sr, Transform2DComponent& tc) {
                renderer_2d_->draw_quad(
                    {tc.world_position.x, tc.world_position.y},
                    {tc.world_scale.x * sr.size.x, tc.world_scale.y * sr.size.y},
                    tc.world_rotation,
                    sr.color
                );
            });
        renderer_2d_->end();
    }
}

// ── HierarchyPanel ─────────────────────────────────────────────────────────

void HierarchyPanel::on_render() {
    namespace ui = nexus::editor::imgui;

    ui::begin_window("Hierarchy");

    // Search filter
    std::string filter_buf = filter_;
    if (ui::input_text("##Filter", filter_buf)) {
        filter_ = filter_buf;
    }
    ui::separator();

    if (!scene_) {
        ui::text("No scene loaded");
        ui::end_window();
        return;
    }

    auto& registry = scene_->registry();

    // Collect root entities (no parent or no HierarchyComponent)
    std::vector<Entity> roots;
    registry.each<TagComponent>([&](Entity e, TagComponent& tag) {
        // Filter by name if filter is set
        if (!filter_.empty()) {
            if (tag.name.find(filter_) == std::string::npos) return;
        }
        // Check if root (no parent)
        if (!registry.has_component<HierarchyComponent>(e) ||
            registry.get_component<HierarchyComponent>(e).parent == INVALID_ENTITY) {
            roots.push_back(e);
        }
    });

    // Render entity tree recursively
    std::function<void(Entity)> render_entity = [&](Entity entity) {
        if (!registry.alive(entity)) return;

        const char* name = "Entity";
        if (registry.has_component<TagComponent>(entity)) {
            name = registry.get_component<TagComponent>(entity).name.c_str();
        }

        auto children = Hierarchy::get_children(registry, entity);
        bool is_leaf = children.empty();
        bool is_selected = has_selection_ && selected_ == entity;

        char label[256];
        std::snprintf(label, sizeof(label), "%s##%u", name, static_cast<u32>(entity));

        bool node_open = ui::tree_node(label, is_selected, is_leaf);

        // Selection
        if (ui::is_item_clicked()) {
            selected_ = entity;
            has_selection_ = true;
        }

        // Context menu (right-click)
        if (ui::begin_popup_context_item()) {
            if (ui::menu_item("Delete Entity")) {
                registry.destroy(entity);
            }
            if (ui::menu_item("Create Child")) {
                Entity child = registry.create();
                registry.add_component<TagComponent>(child, TagComponent{"New Entity"});
                Hierarchy::set_parent(registry, child, entity);
            }
            ui::end_popup();
        }

        // Drag-drop reparenting
        if (ui::begin_drag_source()) {
            ui::set_drag_payload("ENTITY", &entity, sizeof(Entity));
            ui::text("%s", name);
            ui::end_drag_source();
        }
        if (ui::begin_drag_target()) {
            const void* payload = ui::accept_drag_payload("ENTITY");
            if (payload) {
                Entity dragged = *static_cast<const Entity*>(payload);
                if (dragged != entity && !Hierarchy::is_ancestor(registry, entity, dragged)) {
                    Hierarchy::set_parent(registry, dragged, entity);
                }
            }
            ui::end_drag_target();
        }

        if (node_open && !is_leaf) {
            for (Entity child : children) {
                render_entity(child);
            }
            ui::tree_pop();
        }
    };

    for (Entity root : roots) {
        render_entity(root);
    }

    ui::end_window();
}

void HierarchyPanel::add_to_selection(u32 entity) {
    if (!is_multi_selected(entity)) {
        multi_selection_.push_back(entity);
    }
}

void HierarchyPanel::remove_from_selection(u32 entity) {
    multi_selection_.erase(
        std::remove(multi_selection_.begin(), multi_selection_.end(), entity),
        multi_selection_.end());
}

bool HierarchyPanel::is_multi_selected(u32 entity) const {
    return std::find(multi_selection_.begin(), multi_selection_.end(), entity)
           != multi_selection_.end();
}

// ── InspectorPanel ──────────────────────────────────────────────────────────

void InspectorPanel::on_render() {
    namespace ui = nexus::editor::imgui;

    ui::begin_window("Inspector");

    if (!has_target_ || !scene_) {
        ui::text("No entity selected");
        ui::end_window();
        return;
    }

    auto& registry = scene_->registry();
    Entity target = static_cast<Entity>(target_);

    if (!registry.alive(target)) {
        ui::text("Entity no longer valid");
        has_target_ = false;
        ui::end_window();
        return;
    }

    // Lock toggle
    ui::checkbox("Lock", &locked_);
    ui::separator();

    // ── TagComponent ─────────────────────────────────────────────────────
    if (registry.has_component<TagComponent>(target)) {
        if (ui::collapsing_header("Tag")) {
            auto& tag = registry.get_component<TagComponent>(target);
            std::string name_buf = tag.name;
            if (ui::input_text("Name", name_buf)) {
                PropertyEdit edit{"Tag", "name", tag.name, name_buf};
                tag.name = name_buf;
                push_edit(edit);
            }
        }
    }

    // ── Transform3DComponent ─────────────────────────────────────────────
    if (registry.has_component<Transform3DComponent>(target)) {
        if (ui::collapsing_header("Transform 3D")) {
            auto& t = registry.get_component<Transform3DComponent>(target);
            Vec3 pos = t.position;
            if (ui::input_vec3("Position", &pos)) {
                t.position = pos;
                push_edit({"Transform3D", "position", "", ""});
            }
            // Euler angles from quaternion for editing
            Vec3 euler = glm::degrees(glm::eulerAngles(t.rotation));
            if (ui::input_vec3("Rotation", &euler)) {
                t.rotation = Quat(glm::radians(euler));
                push_edit({"Transform3D", "rotation", "", ""});
            }
            Vec3 scale = t.scale;
            if (ui::input_vec3("Scale", &scale)) {
                t.scale = scale;
                push_edit({"Transform3D", "scale", "", ""});
            }
        }
    }

    // ── Transform2DComponent ─────────────────────────────────────────────
    if (registry.has_component<Transform2DComponent>(target)) {
        if (ui::collapsing_header("Transform 2D")) {
            auto& t = registry.get_component<Transform2DComponent>(target);
            Vec2 pos = t.position;
            if (ui::input_vec2("Position", &pos)) {
                t.position = pos;
                push_edit({"Transform2D", "position", "", ""});
            }
            float rot = glm::degrees(t.rotation);
            if (ui::input_float("Rotation", &rot)) {
                t.rotation = glm::radians(rot);
                push_edit({"Transform2D", "rotation", "", ""});
            }
            Vec2 scale = t.scale;
            if (ui::input_vec2("Scale", &scale)) {
                t.scale = scale;
                push_edit({"Transform2D", "scale", "", ""});
            }
        }
    }

    // ── SpriteRendererComponent ──────────────────────────────────────────
    if (registry.has_component<SpriteRendererComponent>(target)) {
        if (ui::collapsing_header("Sprite Renderer")) {
            auto& sr = registry.get_component<SpriteRendererComponent>(target);
            ui::input_color4("Color", &sr.color);
            ui::input_vec2("Size", &sr.size);
        }
    }

    // ── CameraComponent ──────────────────────────────────────────────────
    if (registry.has_component<CameraComponent>(target)) {
        if (ui::collapsing_header("Camera")) {
            auto& cam = registry.get_component<CameraComponent>(target);
            ui::checkbox("Primary", &cam.is_primary);
            ui::checkbox("Orthographic", &cam.is_orthographic);
            ui::input_float("FOV", &cam.fov);
            ui::input_float("Near Clip", &cam.near_clip);
            ui::input_float("Far Clip", &cam.far_clip);
        }
    }

    // ── DirectionalLightComponent ────────────────────────────────────────
    if (registry.has_component<DirectionalLightComponent>(target)) {
        if (ui::collapsing_header("Directional Light")) {
            auto& dl = registry.get_component<DirectionalLightComponent>(target);
            ui::input_vec3("Direction", &dl.direction);
            ui::input_vec3("Color", &dl.color);
            ui::input_float("Intensity", &dl.intensity);
        }
    }

    // ── PointLightComponent ──────────────────────────────────────────────
    if (registry.has_component<PointLightComponent>(target)) {
        if (ui::collapsing_header("Point Light")) {
            auto& pl = registry.get_component<PointLightComponent>(target);
            ui::input_vec3("Color", &pl.color);
            ui::input_float("Intensity", &pl.intensity);
            ui::input_float("Radius", &pl.radius);
        }
    }

    // ── AudioSourceComponent ─────────────────────────────────────────────
    if (registry.has_component<AudioSourceComponent>(target)) {
        if (ui::collapsing_header("Audio Source")) {
            auto& as = registry.get_component<AudioSourceComponent>(target);
            ui::input_float("Volume", &as.volume);
            ui::input_float("Pitch", &as.pitch);
            ui::checkbox("Looping", &as.looping);
            ui::checkbox("Spatial", &as.spatial);
            ui::input_float("Min Distance", &as.min_distance);
            ui::input_float("Max Distance", &as.max_distance);
        }
    }

    // ── RigidBody3DComponent ─────────────────────────────────────────────
    if (registry.has_component<RigidBody3DComponent>(target)) {
        if (ui::collapsing_header("Rigid Body 3D")) {
            auto& rb = registry.get_component<RigidBody3DComponent>(target);
            ui::input_float("Mass", &rb.mass);
            ui::input_float("Friction", &rb.friction);
            ui::input_float("Restitution", &rb.restitution);
            ui::input_float("Linear Damping", &rb.linear_damping);
            ui::input_float("Angular Damping", &rb.angular_damping);
        }
    }

    // ── ActiveComponent ──────────────────────────────────────────────────
    if (registry.has_component<ActiveComponent>(target)) {
        auto& ac = registry.get_component<ActiveComponent>(target);
        ui::checkbox("Active", &ac.active);
    }

    ui::end_window();
}

std::vector<PropertyEdit> InspectorPanel::drain_edits() {
    std::vector<PropertyEdit> result;
    std::swap(result, pending_edits_);
    return result;
}

// ── ConsolePanel ────────────────────────────────────────────────────────────

void ConsolePanel::on_render() {
    namespace ui = nexus::editor::imgui;

    ui::begin_window("Console");

    // Toolbar: level filter toggles + clear button
    if (ui::button("Clear")) { clear(); }
    ui::same_line();
    ui::checkbox("Info", &show_info_);
    ui::same_line();
    ui::checkbox("Warn", &show_warning_);
    ui::same_line();
    ui::checkbox("Error", &show_error_);
    ui::same_line();
    ui::checkbox("Debug", &show_debug_);
    ui::separator();

    // Scrollable message log
    ui::begin_child("ConsoleLog", 0.0f, true);

    for (const auto& msg : messages_) {
        // Filter by level
        if (!is_level_shown(msg.level)) continue;

        // Color by level
        Vec4 color;
        switch (msg.level) {
            case LogLevel::Info:    color = Vec4(1.0f, 1.0f, 1.0f, 1.0f); break;
            case LogLevel::Warning: color = Vec4(1.0f, 0.9f, 0.2f, 1.0f); break;
            case LogLevel::Error:   color = Vec4(1.0f, 0.3f, 0.3f, 1.0f); break;
            case LogLevel::Debug:   color = Vec4(0.6f, 0.6f, 0.6f, 1.0f); break;
        }

        // Level prefix
        const char* prefix = "";
        switch (msg.level) {
            case LogLevel::Info:    prefix = "[INFO] "; break;
            case LogLevel::Warning: prefix = "[WARN] "; break;
            case LogLevel::Error:   prefix = "[ERR]  "; break;
            case LogLevel::Debug:   prefix = "[DBG]  "; break;
        }

        std::string full_msg = std::string(prefix) + msg.text;
        ui::text_colored(color, full_msg.c_str());
    }

    // Auto-scroll to bottom
    if (auto_scroll_) {
        ui::set_scroll_here_y();
    }

    ui::end_child();
    ui::end_window();
}

void ConsolePanel::add_message(const std::string& text, LogLevel level) {
    ConsoleMessage msg;
    msg.text = text;
    msg.level = level;
    messages_.push_back(std::move(msg));

    // Prune if exceeding max — erase the whole overflow range in one shift
    // rather than one element at a time (which is O(n) per removed message).
    if (messages_.size() > max_messages_) {
        messages_.erase(messages_.begin(),
                        messages_.begin() +
                            static_cast<std::ptrdiff_t>(messages_.size() - max_messages_));
    }
}

void ConsolePanel::clear() {
    messages_.clear();
}

void ConsolePanel::set_level_filter(LogLevel level, bool show) {
    switch (level) {
        case LogLevel::Info:    show_info_ = show; break;
        case LogLevel::Warning: show_warning_ = show; break;
        case LogLevel::Error:   show_error_ = show; break;
        case LogLevel::Debug:   show_debug_ = show; break;
    }
}

bool ConsolePanel::is_level_shown(LogLevel level) const {
    switch (level) {
        case LogLevel::Info:    return show_info_;
        case LogLevel::Warning: return show_warning_;
        case LogLevel::Error:   return show_error_;
        case LogLevel::Debug:   return show_debug_;
    }
    return true;
}

// ── AssetBrowserPanel ───────────────────────────────────────────────────────

void AssetBrowserPanel::on_render() {
    namespace ui = nexus::editor::imgui;

    ui::begin_window("Asset Browser");

    // ── Navigation bar ──────────────────────────────────────────────────
    if (ui::button("<")) { go_back(); }
    ui::same_line();
    if (ui::button(">")) { go_forward(); }
    ui::same_line();
    if (ui::button("^")) { navigate_up(); }
    ui::same_line();

    // Breadcrumb path display
    ui::text("%s", current_path_.empty() ? "/" : current_path_.c_str());

    // Search bar
    std::string search_buf = search_;
    if (ui::input_text("##Search", search_buf)) {
        search_ = search_buf;
    }
    ui::separator();

    // ── Content area ────────────────────────────────────────────────────
    ui::begin_child("AssetContent", 0.0f, true);

    float panel_width = ui::get_content_width();
    float cell_size = static_cast<float>(thumbnail_size_) + 16.0f;
    u32 columns = static_cast<u32>(panel_width / cell_size);
    if (columns < 1) columns = 1;

    u32 col = 0;
    for (const auto& entry : entries_) {
        // Filter by search
        if (!search_.empty()) {
            if (entry.name.find(search_) == std::string::npos) continue;
        }

        // Icon/label based on type
        const char* icon = entry.is_directory ? "[D]" : "[F]";

        // Determine file type icon based on extension
        if (!entry.is_directory) {
            if (entry.extension == ".bmp" || entry.extension == ".png" ||
                entry.extension == ".jpg" || entry.extension == ".tga" ||
                entry.extension == ".hdr") {
                icon = "[IMG]";
            } else if (entry.extension == ".obj" || entry.extension == ".gltf" ||
                       entry.extension == ".glb") {
                icon = "[MESH]";
            } else if (entry.extension == ".wav" || entry.extension == ".ogg" ||
                       entry.extension == ".mp3") {
                icon = "[SFX]";
            } else if (entry.extension == ".lua") {
                icon = "[LUA]";
            } else if (entry.extension == ".glsl" || entry.extension == ".vert" ||
                       entry.extension == ".frag") {
                icon = "[SHDR]";
            } else if (entry.extension == ".nxs" || entry.extension == ".json") {
                icon = "[SCENE]";
            }
        }

        char label[512];
        std::snprintf(label, sizeof(label), "%s\n%s##%s", icon, entry.name.c_str(), entry.path.c_str());

        bool is_selected = (selected_ == entry.path);
        if (ui::tree_node(label, is_selected, true)) {
            // Leaf nodes don't need tree_pop
        }

        if (ui::is_item_clicked()) {
            selected_ = entry.path;
            if (entry.is_directory) {
                navigate_to(entry.path);
            }
        }

        // Grid layout: same_line for columns
        if (view_mode_ == ViewMode::Grid) {
            col++;
            if (col < columns) {
                ui::same_line();
            } else {
                col = 0;
            }
        }
    }

    ui::end_child();
    ui::end_window();
}

void AssetBrowserPanel::navigate_to(const std::string& path) {
    // Trim history forward if we navigated back then go somewhere new
    if (history_index_ >= 0 &&
        history_index_ + 1 < static_cast<i32>(history_.size())) {
        history_.erase(history_.begin() + history_index_ + 1, history_.end());
    }

    history_.push_back(path);
    history_index_ = static_cast<i32>(history_.size()) - 1;
    current_path_ = path;
}

void AssetBrowserPanel::navigate_up() {
    auto pos = current_path_.find_last_of('/');
    if (pos != std::string::npos && pos > 0) {
        navigate_to(current_path_.substr(0, pos));
    } else if (!current_path_.empty()) {
        navigate_to("");
    }
}

void AssetBrowserPanel::go_back() {
    if (can_go_back()) {
        history_index_--;
        current_path_ = history_[static_cast<size_t>(history_index_)];
    }
}

void AssetBrowserPanel::go_forward() {
    if (can_go_forward()) {
        history_index_++;
        current_path_ = history_[static_cast<size_t>(history_index_)];
    }
}

} // namespace nexus::editor
