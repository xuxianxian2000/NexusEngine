#pragma once

#include <nexus/core/types.h>
#include <nexus/core/math.h>

namespace nexus {

// ─────────────────────────────────────────────────────────────────────────────
// Camera2D – orthographic camera for 2D rendering
// ─────────────────────────────────────────────────────────────────────────────
class Camera2D {
public:
    Vec2  position{0.0f, 0.0f};
    float rotation{0.0f};          // radians
    float zoom{1.0f};

    void set_projection(float width, float height) {
        projection_ = glm::ortho(0.0f, width, height, 0.0f, -1.0f, 1.0f);
        projection_dirty_ = false;
    }

    [[nodiscard]] Mat4 get_view_matrix() const {
        Mat4 view(1.0f);
        view = glm::translate(view, Vec3(-position, 0.0f));
        view = glm::rotate(view, -rotation, Vec3(0.0f, 0.0f, 1.0f));
        view = glm::scale(view, Vec3(zoom, zoom, 1.0f));
        return view;
    }

    [[nodiscard]] Mat4 get_projection_matrix() const {
        return projection_;
    }

    [[nodiscard]] Mat4 get_view_projection() const {
        return projection_ * get_view_matrix();
    }

    [[nodiscard]] Vec2 screen_to_world(Vec2 screen_pos, Vec2 screen_size) const {
        // Map pixel coords to normalized device coordinates in [-1, 1].
        // Screen origin is top-left, so the Y axis is flipped.
        Vec2 uv = screen_pos / screen_size;
        Vec4 clip{uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f};

        Mat4 inv_vp = glm::inverse(get_view_projection());
        Vec4 world = inv_vp * clip;
        if (world.w != 0.0f) {
            world /= world.w;
        }
        return Vec2(world.x, world.y);
    }

private:
    Mat4 projection_{1.0f};
    bool projection_dirty_{true};
};

// ─────────────────────────────────────────────────────────────────────────────
// Camera3D – perspective camera for 3D rendering
// ─────────────────────────────────────────────────────────────────────────────
class Camera3D {
public:
    Vec3  position{0.0f, 0.0f, 0.0f};
    float yaw{-90.0f};            // degrees – faces -Z by default
    float pitch{0.0f};            // degrees
    float roll{0.0f};             // degrees

    float fov{45.0f};             // vertical FOV in degrees
    float near_clip{0.1f};
    float far_clip{1000.0f};

    void set_perspective(float aspect_ratio) {
        aspect_ratio_ = aspect_ratio;
        projection_ = glm::perspective(glm::radians(fov), aspect_ratio_, near_clip, far_clip);
    }

    [[nodiscard]] Mat4 get_view_matrix() const {
        return glm::lookAt(position, position + forward(), up());
    }

    [[nodiscard]] Mat4 get_projection_matrix() const {
        return projection_;
    }

    [[nodiscard]] Mat4 get_view_projection() const {
        return projection_ * get_view_matrix();
    }

    [[nodiscard]] Vec3 forward() const {
        Vec3 dir;
        float yaw_rad   = glm::radians(yaw);
        float pitch_rad = glm::radians(pitch);
        dir.x = std::cos(yaw_rad) * std::cos(pitch_rad);
        dir.y = std::sin(pitch_rad);
        dir.z = std::sin(yaw_rad) * std::cos(pitch_rad);
        return glm::normalize(dir);
    }

    [[nodiscard]] Vec3 right() const {
        return glm::normalize(glm::cross(forward(), world_up()));
    }

    [[nodiscard]] Vec3 up() const {
        return glm::normalize(glm::cross(right(), forward()));
    }

    void look_at(Vec3 target, Vec3 up_vec = Vec3{0.0f, 1.0f, 0.0f}) {
        Vec3 dir = glm::normalize(target - position);
        // Clamp pitch to avoid gimbal lock at ±90 degrees
        float sin_p = math::clamp(dir.y, -0.9999f, 0.9999f);
        pitch = glm::degrees(std::asin(sin_p));
        yaw   = glm::degrees(std::atan2(dir.z, dir.x));
        (void)up_vec; // roll is kept at current value
    }

private:
    [[nodiscard]] static Vec3 world_up() { return Vec3{0.0f, 1.0f, 0.0f}; }

    float aspect_ratio_{16.0f / 9.0f};
    Mat4  projection_{1.0f};
};

} // namespace nexus
