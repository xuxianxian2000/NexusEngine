#include "nexus/editor/gizmo.h"
#include <cmath>

namespace nexus::editor {

static const Vec4 COLOR_X{1.0f, 0.2f, 0.2f, 1.0f};  // red
static const Vec4 COLOR_Y{0.2f, 1.0f, 0.2f, 1.0f};  // green
static const Vec4 COLOR_Z{0.2f, 0.4f, 1.0f, 1.0f};  // blue
static const Vec4 COLOR_ACTIVE{1.0f, 1.0f, 0.0f, 1.0f}; // yellow highlight

void Gizmo::draw(DebugRenderer& debug, const Camera3D& camera,
                  Vec3 position, Quat orientation) const {
    // Scale handle length by distance to camera for consistent screen size
    float dist = glm::length(camera.position - position);
    float scale = dist * 0.1f * handle_size;

    Vec3 ax_x = Vec3(1, 0, 0);
    Vec3 ax_y = Vec3(0, 1, 0);
    Vec3 ax_z = Vec3(0, 0, 1);

    if (space_ == GizmoSpace::Local) {
        ax_x = glm::rotate(orientation, ax_x);
        ax_y = glm::rotate(orientation, ax_y);
        ax_z = glm::rotate(orientation, ax_z);
    }

    Vec4 cx = (active_axis_ == GizmoAxis::X) ? COLOR_ACTIVE : COLOR_X;
    Vec4 cy = (active_axis_ == GizmoAxis::Y) ? COLOR_ACTIVE : COLOR_Y;
    Vec4 cz = (active_axis_ == GizmoAxis::Z) ? COLOR_ACTIVE : COLOR_Z;

    if (mode_ == GizmoMode::Translate || mode_ == GizmoMode::Scale) {
        // Draw axis lines
        debug.draw_line(position, position + ax_x * scale, cx);
        debug.draw_line(position, position + ax_y * scale, cy);
        debug.draw_line(position, position + ax_z * scale, cz);

        if (mode_ == GizmoMode::Translate) {
            // Arrow tips
            float tip = scale * 0.15f;
            debug.draw_line(position + ax_x * scale,
                           position + ax_x * (scale - tip) + ax_y * tip * 0.5f, cx);
            debug.draw_line(position + ax_x * scale,
                           position + ax_x * (scale - tip) - ax_y * tip * 0.5f, cx);

            debug.draw_line(position + ax_y * scale,
                           position + ax_y * (scale - tip) + ax_x * tip * 0.5f, cy);
            debug.draw_line(position + ax_y * scale,
                           position + ax_y * (scale - tip) - ax_x * tip * 0.5f, cy);

            debug.draw_line(position + ax_z * scale,
                           position + ax_z * (scale - tip) + ax_y * tip * 0.5f, cz);
            debug.draw_line(position + ax_z * scale,
                           position + ax_z * (scale - tip) - ax_y * tip * 0.5f, cz);
        } else {
            // Scale: small cubes at ends
            float cube_s = scale * 0.08f;
            AABB bx{position + ax_x * scale - Vec3(cube_s), position + ax_x * scale + Vec3(cube_s)};
            AABB by{position + ax_y * scale - Vec3(cube_s), position + ax_y * scale + Vec3(cube_s)};
            AABB bz{position + ax_z * scale - Vec3(cube_s), position + ax_z * scale + Vec3(cube_s)};
            debug.draw_aabb(bx, cx);
            debug.draw_aabb(by, cy);
            debug.draw_aabb(bz, cz);
        }
    } else if (mode_ == GizmoMode::Rotate) {
        // Draw rotation rings (simplified: 3 circles)
        int segments = 32;
        float r = scale * 0.8f;
        for (int i = 0; i < segments; ++i) {
            float a0 = static_cast<float>(i) / static_cast<float>(segments) * 6.2831853f;
            float a1 = static_cast<float>(i + 1) / static_cast<float>(segments) * 6.2831853f;

            // X ring (in YZ plane)
            debug.draw_line(
                position + ax_y * (std::cos(a0) * r) + ax_z * (std::sin(a0) * r),
                position + ax_y * (std::cos(a1) * r) + ax_z * (std::sin(a1) * r), cx);
            // Y ring (in XZ plane)
            debug.draw_line(
                position + ax_x * (std::cos(a0) * r) + ax_z * (std::sin(a0) * r),
                position + ax_x * (std::cos(a1) * r) + ax_z * (std::sin(a1) * r), cy);
            // Z ring (in XY plane)
            debug.draw_line(
                position + ax_x * (std::cos(a0) * r) + ax_y * (std::sin(a0) * r),
                position + ax_x * (std::cos(a1) * r) + ax_y * (std::sin(a1) * r), cz);
        }
    }
}

GizmoAxis Gizmo::hit_test(const Camera3D& camera, Vec3 gizmo_position,
                            Vec2 screen_pos, Vec2 screen_size) const {
    // Unproject screen position to world ray
    Mat4 inv_vp = glm::inverse(camera.get_view_projection());

    Vec2 ndc = (screen_pos / screen_size) * 2.0f - Vec2(1.0f);
    ndc.y = -ndc.y;

    Vec4 ray_near = inv_vp * Vec4(ndc, -1.0f, 1.0f);
    Vec4 ray_far  = inv_vp * Vec4(ndc,  1.0f, 1.0f);
    ray_near /= ray_near.w;
    ray_far /= ray_far.w;

    Vec3 ray_origin = Vec3(ray_near);
    Vec3 ray_dir = glm::normalize(Vec3(ray_far) - Vec3(ray_near));

    float dist = glm::length(camera.position - gizmo_position);
    float scale = dist * 0.1f * handle_size;
    float threshold = scale * 0.15f;

    // Test each axis line
    auto test_axis = [&](Vec3 axis_dir) -> float {
        Vec3 end = gizmo_position + axis_dir * scale;
        Vec3 w = gizmo_position - ray_origin;
        Vec3 u = ray_dir;
        Vec3 v = end - gizmo_position;

        float a = glm::dot(u, u);
        float b = glm::dot(u, v);
        float c = glm::dot(v, v);
        float d = glm::dot(u, w);
        float e = glm::dot(v, w);
        float denom = a * c - b * b;
        if (std::abs(denom) < 1e-6f) return 1e10f;

        float s = (b * e - c * d) / denom;
        float t = (a * e - b * d) / denom;
        t = math::clamp(t, 0.0f, 1.0f);

        Vec3 closest_on_ray = ray_origin + u * s;
        Vec3 closest_on_axis = gizmo_position + v * t;
        return glm::length(closest_on_ray - closest_on_axis);
    };

    float dx = test_axis(Vec3(1, 0, 0));
    float dy = test_axis(Vec3(0, 1, 0));
    float dz = test_axis(Vec3(0, 0, 1));

    // Pick the nearest axis directly instead of re-comparing with float equality
    // against the min (fragile, and a tie/NaN would fall through to Z).
    GizmoAxis best = GizmoAxis::X;
    float min_dist = dx;
    if (dy < min_dist) { min_dist = dy; best = GizmoAxis::Y; }
    if (dz < min_dist) { min_dist = dz; best = GizmoAxis::Z; }

    if (min_dist > threshold) return GizmoAxis::None;
    return best;
}

void Gizmo::begin_drag(GizmoAxis axis, Vec3 start_position, Vec2 mouse_pos) {
    active_axis_ = axis;
    dragging_ = true;
    drag_start_ = start_position;
    drag_mouse_start_ = mouse_pos;
}

Vec3 Gizmo::update_drag(const Camera3D& camera, Vec3 gizmo_position,
                          Vec2 mouse_pos, Vec2 screen_size) {
    if (!dragging_ || active_axis_ == GizmoAxis::None) return Vec3(0.0f);

    Vec2 delta = mouse_pos - drag_mouse_start_;
    drag_mouse_start_ = mouse_pos;

    float dist = glm::length(camera.position - gizmo_position);
    float sensitivity = dist * 0.002f;

    Vec3 result(0.0f);
    // Project axis to screen and use dominant screen direction
    Vec3 axis_dir(0.0f);
    if (active_axis_ == GizmoAxis::X) axis_dir = Vec3(1, 0, 0);
    else if (active_axis_ == GizmoAxis::Y) axis_dir = Vec3(0, 1, 0);
    else if (active_axis_ == GizmoAxis::Z) axis_dir = Vec3(0, 0, 1);

    // Project axis direction to screen space to determine which mouse axis to use
    Mat4 vp = camera.get_view_projection();
    Vec4 p0 = vp * Vec4(gizmo_position, 1.0f);
    Vec4 p1 = vp * Vec4(gizmo_position + axis_dir, 1.0f);
    if (std::abs(p0.w) > 1e-6f && std::abs(p1.w) > 1e-6f) {
        Vec2 s0 = Vec2(p0) / p0.w;
        Vec2 s1 = Vec2(p1) / p1.w;
        Vec2 screen_axis = s1 - s0;
        float proj = glm::dot(Vec2(delta.x / screen_size.x, -delta.y / screen_size.y),
                              screen_axis);
        result = axis_dir * proj * dist * 2.0f;
    } else {
        result = axis_dir * delta.x * sensitivity;
    }

    return result;
}

void Gizmo::end_drag() {
    dragging_ = false;
    active_axis_ = GizmoAxis::None;
}

} // namespace nexus::editor
