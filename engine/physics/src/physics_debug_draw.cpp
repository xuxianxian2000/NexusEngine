#include "nexus/physics/physics_debug_draw.h"
#include <cmath>

namespace nexus::physics {

Vec4 PhysicsDebugDraw::body_color(const Body3D& body) const {
    if (body.is_trigger) return trigger_color;
    if (body.sleeping) return sleeping_color;
    switch (body.type) {
        case Body3D::Static:    return static_color;
        case Body3D::Kinematic: return kinematic_color;
        default:                return dynamic_color;
    }
}

void PhysicsDebugDraw::draw_body(const Body3D& body) const {
    Vec4 color = body_color(body);

    switch (body.shape) {
        case Body3D::Sphere:
            renderer_.draw_sphere(body.position, body.radius, color, 16);
            break;
        case Body3D::Box:
            renderer_.draw_box(body.position, body.half_extents, color);
            break;
        case Body3D::Capsule:
            draw_capsule_wireframe(body.position, body.rotation,
                                    body.radius, body.height, color);
            break;
    }
}

void PhysicsDebugDraw::draw_capsule_wireframe(Vec3 center, Quat rotation,
                                               float radius, float height, Vec4 color) const {
    Vec3 up = glm::mat3_cast(rotation) * Vec3(0.0f, 1.0f, 0.0f);
    float half_h = height * 0.5f;
    Vec3 top = center + up * half_h;
    Vec3 bot = center - up * half_h;

    // Draw hemisphere caps as sphere approximations
    renderer_.draw_sphere(top, radius, color, 8);
    renderer_.draw_sphere(bot, radius, color, 8);

    // Draw connecting lines
    Vec3 right = glm::mat3_cast(rotation) * Vec3(radius, 0.0f, 0.0f);
    Vec3 fwd   = glm::mat3_cast(rotation) * Vec3(0.0f, 0.0f, radius);
    renderer_.draw_line(top + right, bot + right, color);
    renderer_.draw_line(top - right, bot - right, color);
    renderer_.draw_line(top + fwd, bot + fwd, color);
    renderer_.draw_line(top - fwd, bot - fwd, color);
}

void PhysicsDebugDraw::draw_world(const PhysicsWorld3D& world) const {
    for (const auto& body : world.bodies()) {
        draw_body(body);
    }
}

void PhysicsDebugDraw::draw_contacts(const PhysicsWorld3D& world) const {
    for (const auto& pair : world.contacts()) {
        Vec3 p = pair.contact.point;
        Vec3 n = pair.contact.normal;

        // Draw contact point as small cross
        float sz = 0.05f;
        renderer_.draw_line(p - Vec3(sz, 0, 0), p + Vec3(sz, 0, 0), contact_color);
        renderer_.draw_line(p - Vec3(0, sz, 0), p + Vec3(0, sz, 0), contact_color);
        renderer_.draw_line(p - Vec3(0, 0, sz), p + Vec3(0, 0, sz), contact_color);

        // Draw normal as arrow
        renderer_.draw_ray(p, n, 0.3f, contact_color);
    }
}

void PhysicsDebugDraw::draw_joints(const PhysicsWorld3D& world) const {
    for (const auto& joint : world.joints()) {
        if (!joint->enabled) continue;

        // Resolve bodies by id rather than the cached raw pointers, which are
        // only refreshed inside step() and dangle after any create/destroy_body
        // reallocates the body storage.
        const Body3D* a = world.get_body(joint->body_a_id);
        const Body3D* b = world.get_body(joint->body_b_id);
        if (!a || !b) continue;

        renderer_.draw_line(a->position, b->position, joint_color);
    }
}

} // namespace nexus::physics
