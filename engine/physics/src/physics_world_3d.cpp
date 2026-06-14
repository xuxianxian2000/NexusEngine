#include "nexus/physics/physics_world_3d.h"
#include "nexus/core/log.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace nexus::physics {

// ── Body3D ──────────────────────────────────────────────────────────────────

void Body3D::compute_mass() {
    if (type == Static) {
        mass = 0.0f;
        inv_mass = 0.0f;
        inertia_tensor = Mat3(0.0f);
        inv_inertia_tensor = Mat3(0.0f);
        return;
    }
    inv_mass = (mass > 0.0f) ? 1.0f / mass : 0.0f;

    // Compute inertia tensor based on shape
    Vec3 I(0.0f);
    if (shape == Box) {
        float w2 = 4.0f * half_extents.x * half_extents.x;
        float h2 = 4.0f * half_extents.y * half_extents.y;
        float d2 = 4.0f * half_extents.z * half_extents.z;
        I.x = mass * (h2 + d2) / 12.0f;
        I.y = mass * (w2 + d2) / 12.0f;
        I.z = mass * (w2 + h2) / 12.0f;
    } else if (shape == Sphere) {
        float Isphere = 0.4f * mass * radius * radius;
        I = Vec3(Isphere);
    } else { // Capsule — cylinder + two hemispheres
        float r2 = radius * radius;
        float h = height;
        float cyl_mass = mass * h / (h + (4.0f / 3.0f) * radius);
        float cap_mass = mass - cyl_mass;
        float Iy = cyl_mass * r2 * 0.5f + cap_mass * 0.4f * r2;
        float Ixz = cyl_mass * (3.0f * r2 + h * h) / 12.0f
                   + cap_mass * (0.4f * r2 + 0.25f * h * h + 0.375f * h * radius);
        I = Vec3(Ixz, Iy, Ixz);
    }
    inertia_tensor = Mat3(0.0f);
    inertia_tensor[0][0] = I.x;
    inertia_tensor[1][1] = I.y;
    inertia_tensor[2][2] = I.z;

    inv_inertia_tensor = Mat3(0.0f);
    if (I.x > 0.0f) inv_inertia_tensor[0][0] = 1.0f / I.x;
    if (I.y > 0.0f) inv_inertia_tensor[1][1] = 1.0f / I.y;
    if (I.z > 0.0f) inv_inertia_tensor[2][2] = 1.0f / I.z;
}

// ── PhysicsWorld3D ──────────────────────────────────────────────────────────

PhysicsWorld3D::PhysicsWorld3D(Vec3 gravity) : gravity_(gravity) {
    bodies_.reserve(256);
}

u32 PhysicsWorld3D::create_body(const Body3D& desc) {
    Body3D body = desc;
    body.id = next_id_++;
    body.compute_mass();
    bodies_.push_back(body);
    return body.id;
}

void PhysicsWorld3D::destroy_body(u32 id) {
    bodies_.erase(
        std::remove_if(bodies_.begin(), bodies_.end(),
            [id](const Body3D& b) { return b.id == id; }),
        bodies_.end());
}

Body3D* PhysicsWorld3D::get_body(u32 id) {
    for (auto& b : bodies_) {
        if (b.id == id) return &b;
    }
    return nullptr;
}

const Body3D* PhysicsWorld3D::get_body(u32 id) const {
    for (const auto& b : bodies_) {
        if (b.id == id) return &b;
    }
    return nullptr;
}

void PhysicsWorld3D::step(float dt, u32 iterations) {
    if (dt <= 0.0f) return;

    integrate(dt);
    broadphase();

    // Wake sleeping bodies involved in collisions
    for (auto& pair : contacts_) {
        Body3D* a = get_body(pair.body_a);
        Body3D* b = get_body(pair.body_b);
        if (a && b) {
            if (a->sleeping && b->type == Body3D::Dynamic && !b->sleeping) a->wake();
            if (b->sleeping && a->type == Body3D::Dynamic && !a->sleeping) b->wake();
        }
    }

    // Prepare joints
    for (auto& joint : joints_) {
        if (!joint->enabled) continue;
        joint->body_a = get_body(joint->body_a_id);
        joint->body_b = get_body(joint->body_b_id);
        if (joint->body_a && joint->body_b) {
            joint->body_a->wake();
            joint->body_b->wake();
            joint->prepare(dt);
        }
    }

    for (u32 iter = 0; iter < iterations; ++iter) {
        for (auto& pair : contacts_) {
            Body3D* a = get_body(pair.body_a);
            Body3D* b = get_body(pair.body_b);
            if (a && b) resolve_collision(*a, *b, pair.contact);
        }
        // Solve joints each iteration
        for (auto& joint : joints_) {
            if (joint->enabled && joint->body_a && joint->body_b) {
                joint->solve();
            }
        }
    }

    // Positional (Baumgarte) correction: a single pass after the velocity solve,
    // using the original penetration depth.
    {
        const float percent = 0.8f;
        const float slop = 0.01f;
        for (auto& pair : contacts_) {
            Body3D* a = get_body(pair.body_a);
            Body3D* b = get_body(pair.body_b);
            if (!a || !b || a->is_trigger || b->is_trigger) continue;
            float inv_mass_sum = a->inv_mass + b->inv_mass;
            if (inv_mass_sum <= 0.0f) continue;
            Vec3 correction = pair.contact.normal *
                (std::max(pair.contact.depth - slop, 0.0f) / inv_mass_sum) * percent;
            a->position -= correction * a->inv_mass;
            b->position += correction * b->inv_mass;
        }
    }

    update_sleeping(dt);

    if (contact_callback_) {
        for (const auto& pair : contacts_) {
            contact_callback_(pair);
        }
    }
}

void PhysicsWorld3D::destroy_joint(u32 joint_id) {
    joints_.erase(
        std::remove_if(joints_.begin(), joints_.end(),
            [joint_id](const std::unique_ptr<Constraint>& j) { return j->id == joint_id; }),
        joints_.end());
}

Constraint* PhysicsWorld3D::get_joint(u32 joint_id) {
    for (auto& j : joints_) {
        if (j->id == joint_id) return j.get();
    }
    return nullptr;
}

void PhysicsWorld3D::apply_force(u32 id, Vec3 force) {
    if (auto* b = get_body(id)) { b->wake(); b->force += force; }
}

void PhysicsWorld3D::apply_impulse(u32 id, Vec3 impulse) {
    if (auto* b = get_body(id)) {
        b->wake();
        b->velocity += impulse * b->inv_mass;
    }
}

void PhysicsWorld3D::apply_torque(u32 id, Vec3 torque) {
    if (auto* b = get_body(id)) { b->wake(); b->torque_accum += torque; }
}

// ── Integration ─────────────────────────────────────────────────────────────

void PhysicsWorld3D::integrate(float dt) {
    for (auto& b : bodies_) {
        if (b.type == Body3D::Static) continue;
        if (b.sleeping) continue;

        if (b.type == Body3D::Dynamic) {
            Vec3 accel = gravity_ * b.gravity_scale + b.force * b.inv_mass;
            b.velocity += accel * dt;

            // Torque integration: world-space inverse inertia tensor
            Mat3 rot_mat = glm::mat3_cast(b.rotation);
            Mat3 inv_I_world = rot_mat * b.inv_inertia_tensor * glm::transpose(rot_mat);
            b.angular_velocity += inv_I_world * b.torque_accum * dt;

            b.velocity *= 1.0f / (1.0f + b.linear_damping * dt);
            b.angular_velocity *= 1.0f / (1.0f + b.angular_damping * dt);
        }

        b.position += b.velocity * dt;

        // Integrate angular velocity into quaternion
        float ang_speed = glm::length(b.angular_velocity);
        if (ang_speed > math::EPSILON) {
            Vec3 axis = b.angular_velocity / ang_speed;
            float half_angle = ang_speed * dt * 0.5f;
            Quat dq = Quat(std::cos(half_angle),
                           axis.x * std::sin(half_angle),
                           axis.y * std::sin(half_angle),
                           axis.z * std::sin(half_angle));
            b.rotation = glm::normalize(dq * b.rotation);
        }

        b.force = Vec3(0.0f);
        b.torque_accum = Vec3(0.0f);
    }
}

void PhysicsWorld3D::update_sleeping(float dt) {
    for (auto& b : bodies_) {
        if (b.type == Body3D::Static) continue;
        float energy = glm::dot(b.velocity, b.velocity)
                     + glm::dot(b.angular_velocity, b.angular_velocity);
        if (energy < Body3D::SLEEP_THRESHOLD) {
            b.sleep_timer += dt;
            if (b.sleep_timer >= Body3D::SLEEP_TIME) {
                b.sleeping = true;
                b.velocity = Vec3(0.0f);
                b.angular_velocity = Vec3(0.0f);
            }
        } else {
            b.sleep_timer = 0.0f;
            b.sleeping = false;
        }
    }
}

// ── Broadphase ──────────────────────────────────────────────────────────────

static AABB body3d_aabb(const Body3D& b) {
    AABB aabb;
    if (b.shape == Body3D::Sphere) {
        aabb.min = b.position - Vec3(b.radius);
        aabb.max = b.position + Vec3(b.radius);
    } else if (b.shape == Body3D::Capsule) {
        float h = b.height * 0.5f;
        float r = b.radius;
        float ext = h + r;
        aabb.min = b.position - Vec3(r, ext, r);
        aabb.max = b.position + Vec3(r, ext, r);
    } else {
        // Conservative AABB for box (ignoring rotation for broadphase)
        float maxExt = std::max({b.half_extents.x, b.half_extents.y, b.half_extents.z});
        float diag = maxExt * 1.7321f; // sqrt(3)
        aabb.min = b.position - Vec3(diag);
        aabb.max = b.position + Vec3(diag);
    }
    return aabb;
}

void PhysicsWorld3D::broadphase() {
    contacts_.clear();

    // Build spatial hash
    spatial_hash_.clear();
    std::unordered_map<u32, AABB> aabb_cache;
    for (const auto& b : bodies_) {
        AABB aabb = body3d_aabb(b);
        aabb_cache[b.id] = aabb;
        spatial_hash_.insert(b.id, aabb);
    }

    // Query candidate pairs via spatial hash
    std::unordered_set<u64> seen_pairs;
    std::vector<std::pair<u32, u32>> candidates;
    for (const auto& b : bodies_) {
        spatial_hash_.query(aabb_cache[b.id], seen_pairs, b.id, candidates);
    }

    // Narrow phase on candidate pairs
    for (const auto& [id_lo, id_hi] : candidates) {
        Body3D* a = get_body(id_lo);
        Body3D* b = get_body(id_hi);
        if (!a || !b) continue;

        if (a->type == Body3D::Static && b->type == Body3D::Static) continue;
        if (!(a->layer & b->mask) || !(b->layer & a->mask)) continue;

        // AABB overlap confirmation
        if (!aabb_cache[a->id].overlaps(aabb_cache[b->id])) continue;

        Contact3D contact;
        if (narrowphase(*a, *b, contact)) {
            contacts_.push_back({a->id, b->id, contact});
        }
    }
}

// ── Narrow phase ────────────────────────────────────────────────────────────

bool PhysicsWorld3D::narrowphase(const Body3D& a, const Body3D& b, Contact3D& contact) const {
    if (a.shape == Body3D::Sphere && b.shape == Body3D::Sphere) {
        return sphere_vs_sphere(a, b, contact);
    }
    if (a.shape == Body3D::Box && b.shape == Body3D::Box) {
        return box_vs_box(a, b, contact);
    }
    if (a.shape == Body3D::Sphere && b.shape == Body3D::Box) {
        return sphere_vs_box(a, b, contact);
    }
    if (a.shape == Body3D::Box && b.shape == Body3D::Sphere) {
        bool result = sphere_vs_box(b, a, contact);
        if (result) contact.normal = -contact.normal;
        return result;
    }
    // Capsule collision — uses line-segment closest-point tests
    if (a.shape == Body3D::Capsule && b.shape == Body3D::Capsule) {
        return capsule_vs_capsule(a, b, contact);
    }
    if (a.shape == Body3D::Capsule && b.shape == Body3D::Sphere) {
        return capsule_vs_sphere(a, b, contact);
    }
    if (a.shape == Body3D::Sphere && b.shape == Body3D::Capsule) {
        bool result = capsule_vs_sphere(b, a, contact);
        if (result) contact.normal = -contact.normal;
        return result;
    }
    if (a.shape == Body3D::Capsule && b.shape == Body3D::Box) {
        return capsule_vs_box(a, b, contact);
    }
    if (a.shape == Body3D::Box && b.shape == Body3D::Capsule) {
        bool result = capsule_vs_box(b, a, contact);
        if (result) contact.normal = -contact.normal;
        return result;
    }
    return false;
}

bool PhysicsWorld3D::sphere_vs_sphere(const Body3D& a, const Body3D& b, Contact3D& c) const {
    Vec3 diff = b.position - a.position;
    float dist_sq = glm::dot(diff, diff);
    float sum_r = a.radius + b.radius;

    if (dist_sq > sum_r * sum_r) return false;

    float dist = std::sqrt(dist_sq);
    if (dist < math::EPSILON) {
        c.normal = {0.0f, 1.0f, 0.0f};
        c.depth = sum_r;
        c.point = a.position;
    } else {
        c.normal = diff / dist;
        c.depth = sum_r - dist;
        c.point = a.position + c.normal * a.radius;
    }
    return true;
}

bool PhysicsWorld3D::box_vs_box(const Body3D& a, const Body3D& b, Contact3D& c) const {
    // OBB vs OBB using Separating Axis Theorem (15 axes)
    // Build rotation matrices from quaternions
    Mat3 rot_a = glm::mat3_cast(a.rotation);
    Mat3 rot_b = glm::mat3_cast(b.rotation);

    Vec3 axes_a[3] = { rot_a[0], rot_a[1], rot_a[2] };
    Vec3 axes_b[3] = { rot_b[0], rot_b[1], rot_b[2] };

    Vec3 diff = b.position - a.position;

    // Compute rotation matrix expressing B in A's coordinate frame
    float R[3][3], absR[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            R[i][j] = glm::dot(axes_a[i], axes_b[j]);
            absR[i][j] = std::abs(R[i][j]) + math::EPSILON; // Add epsilon to handle parallel edges
        }
    }

    float t[3] = { glm::dot(diff, axes_a[0]), glm::dot(diff, axes_a[1]), glm::dot(diff, axes_a[2]) };

    float min_overlap = std::numeric_limits<float>::max();
    Vec3 min_axis{0.0f};
    int min_axis_id = -1;

    // Test 15 separating axes
    auto test_axis = [&](float ra, float rb, float sep, Vec3 axis, int id) -> bool {
        float overlap = ra + rb - std::abs(sep);
        if (overlap <= 0.0f) return false;
        float len = glm::length(axis);
        if (len < math::EPSILON) return true; // Degenerate axis, skip
        overlap /= len;
        if (overlap < min_overlap) {
            min_overlap = overlap;
            min_axis = axis / len;
            min_axis_id = id;
        }
        return true;
    };

    // A's face normals (3 axes)
    if (!test_axis(a.half_extents[0],
                   b.half_extents[0]*absR[0][0] + b.half_extents[1]*absR[0][1] + b.half_extents[2]*absR[0][2],
                   t[0], axes_a[0], 0)) return false;
    if (!test_axis(a.half_extents[1],
                   b.half_extents[0]*absR[1][0] + b.half_extents[1]*absR[1][1] + b.half_extents[2]*absR[1][2],
                   t[1], axes_a[1], 1)) return false;
    if (!test_axis(a.half_extents[2],
                   b.half_extents[0]*absR[2][0] + b.half_extents[1]*absR[2][1] + b.half_extents[2]*absR[2][2],
                   t[2], axes_a[2], 2)) return false;

    // B's face normals (3 axes)
    if (!test_axis(a.half_extents[0]*absR[0][0] + a.half_extents[1]*absR[1][0] + a.half_extents[2]*absR[2][0],
                   b.half_extents[0],
                   t[0]*R[0][0] + t[1]*R[1][0] + t[2]*R[2][0], axes_b[0], 3)) return false;
    if (!test_axis(a.half_extents[0]*absR[0][1] + a.half_extents[1]*absR[1][1] + a.half_extents[2]*absR[2][1],
                   b.half_extents[1],
                   t[0]*R[0][1] + t[1]*R[1][1] + t[2]*R[2][1], axes_b[1], 4)) return false;
    if (!test_axis(a.half_extents[0]*absR[0][2] + a.half_extents[1]*absR[1][2] + a.half_extents[2]*absR[2][2],
                   b.half_extents[2],
                   t[0]*R[0][2] + t[1]*R[1][2] + t[2]*R[2][2], axes_b[2], 5)) return false;

    // 9 edge-edge cross products (Ax x Bx, Ax x By, Ax x Bz, Ay x Bx, ...)
    if (!test_axis(a.half_extents[1]*absR[2][0] + a.half_extents[2]*absR[1][0],
                   b.half_extents[1]*absR[0][2] + b.half_extents[2]*absR[0][1],
                   t[2]*R[1][0] - t[1]*R[2][0], glm::cross(axes_a[0], axes_b[0]), 6)) return false;
    if (!test_axis(a.half_extents[1]*absR[2][1] + a.half_extents[2]*absR[1][1],
                   b.half_extents[0]*absR[0][2] + b.half_extents[2]*absR[0][0],
                   t[2]*R[1][1] - t[1]*R[2][1], glm::cross(axes_a[0], axes_b[1]), 7)) return false;
    if (!test_axis(a.half_extents[1]*absR[2][2] + a.half_extents[2]*absR[1][2],
                   b.half_extents[0]*absR[0][1] + b.half_extents[1]*absR[0][0],
                   t[2]*R[1][2] - t[1]*R[2][2], glm::cross(axes_a[0], axes_b[2]), 8)) return false;

    if (!test_axis(a.half_extents[0]*absR[2][0] + a.half_extents[2]*absR[0][0],
                   b.half_extents[1]*absR[1][2] + b.half_extents[2]*absR[1][1],
                   t[0]*R[2][0] - t[2]*R[0][0], glm::cross(axes_a[1], axes_b[0]), 9)) return false;
    if (!test_axis(a.half_extents[0]*absR[2][1] + a.half_extents[2]*absR[0][1],
                   b.half_extents[0]*absR[1][2] + b.half_extents[2]*absR[1][0],
                   t[0]*R[2][1] - t[2]*R[0][1], glm::cross(axes_a[1], axes_b[1]), 10)) return false;
    if (!test_axis(a.half_extents[0]*absR[2][2] + a.half_extents[2]*absR[0][2],
                   b.half_extents[0]*absR[1][1] + b.half_extents[1]*absR[1][0],
                   t[0]*R[2][2] - t[2]*R[0][2], glm::cross(axes_a[1], axes_b[2]), 11)) return false;

    if (!test_axis(a.half_extents[0]*absR[1][0] + a.half_extents[1]*absR[0][0],
                   b.half_extents[1]*absR[2][2] + b.half_extents[2]*absR[2][1],
                   t[1]*R[0][0] - t[0]*R[1][0], glm::cross(axes_a[2], axes_b[0]), 12)) return false;
    if (!test_axis(a.half_extents[0]*absR[1][1] + a.half_extents[1]*absR[0][1],
                   b.half_extents[0]*absR[2][2] + b.half_extents[2]*absR[2][0],
                   t[1]*R[0][1] - t[0]*R[1][1], glm::cross(axes_a[2], axes_b[1]), 13)) return false;
    if (!test_axis(a.half_extents[0]*absR[1][2] + a.half_extents[1]*absR[0][2],
                   b.half_extents[0]*absR[2][1] + b.half_extents[1]*absR[2][0],
                   t[1]*R[0][2] - t[0]*R[1][2], glm::cross(axes_a[2], axes_b[2]), 14)) return false;

    // Ensure normal points from A to B
    if (glm::dot(min_axis, diff) < 0.0f) {
        min_axis = -min_axis;
    }

    c.normal = min_axis;
    c.depth = min_overlap;
    c.point = a.position + min_axis * glm::dot(a.half_extents, Vec3(
        std::abs(glm::dot(axes_a[0], min_axis)),
        std::abs(glm::dot(axes_a[1], min_axis)),
        std::abs(glm::dot(axes_a[2], min_axis))));
    return true;
}

bool PhysicsWorld3D::sphere_vs_box(const Body3D& sphere, const Body3D& box, Contact3D& c) const {
    // Transform sphere center to box local space for proper OBB support
    Mat3 rot = glm::mat3_cast(box.rotation);
    Mat3 rot_inv = glm::transpose(rot);
    Vec3 local_diff = rot_inv * (sphere.position - box.position);

    Vec3 closest;
    for (int i = 0; i < 3; ++i) {
        closest[i] = math::clamp(local_diff[i], -box.half_extents[i], box.half_extents[i]);
    }

    Vec3 delta = local_diff - closest;
    float dist_sq = glm::dot(delta, delta);

    if (dist_sq > sphere.radius * sphere.radius) return false;

    float dist = std::sqrt(dist_sq);
    Vec3 local_normal;
    if (dist < math::EPSILON) {
        // Sphere center inside box
        Vec3 pen;
        for (int i = 0; i < 3; ++i) {
            pen[i] = box.half_extents[i] - std::abs(local_diff[i]);
        }
        int min_axis = 0;
        if (pen[1] < pen[min_axis]) min_axis = 1;
        if (pen[2] < pen[min_axis]) min_axis = 2;

        local_normal = Vec3(0.0f);
        local_normal[min_axis] = (local_diff[min_axis] < 0.0f) ? -1.0f : 1.0f;
        c.depth = pen[min_axis] + sphere.radius;
    } else {
        local_normal = delta / dist;
        c.depth = sphere.radius - dist;
    }
    // Transform normal back to world space
    c.normal = rot * local_normal;
    c.point = sphere.position - c.normal * sphere.radius;
    return true;
}

// ── Capsule helpers ─────────────────────────────────────────────────────────

/// Closest point on line segment AB to point P
static Vec3 closest_point_on_segment(Vec3 A, Vec3 B, Vec3 P) {
    Vec3 ab = B - A;
    float t = glm::dot(P - A, ab) / (glm::dot(ab, ab) + math::EPSILON);
    return A + ab * math::clamp(t, 0.0f, 1.0f);
}

/// Get capsule segment endpoints (Y-axis aligned in local space, rotated by body quaternion)
static void capsule_segment(const Body3D& cap, Vec3& A, Vec3& B) {
    Vec3 up = glm::mat3_cast(cap.rotation) * Vec3(0.0f, cap.height * 0.5f, 0.0f);
    A = cap.position - up;
    B = cap.position + up;
}

/// Closest points between two line segments (returns squared distance)
static float closest_points_segments(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2,
                                     Vec3& c1, Vec3& c2) {
    Vec3 d1 = q1 - p1;
    Vec3 d2 = q2 - p2;
    Vec3 r  = p1 - p2;
    float a = glm::dot(d1, d1);
    float e = glm::dot(d2, d2);
    float f = glm::dot(d2, r);

    float s, t;

    if (a <= math::EPSILON && e <= math::EPSILON) {
        s = t = 0.0f;
    } else if (a <= math::EPSILON) {
        s = 0.0f;
        t = math::clamp(f / e, 0.0f, 1.0f);
    } else {
        float c = glm::dot(d1, r);
        if (e <= math::EPSILON) {
            t = 0.0f;
            s = math::clamp(-c / a, 0.0f, 1.0f);
        } else {
            float b = glm::dot(d1, d2);
            float denom = a * e - b * b;
            s = (denom != 0.0f) ? math::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) { t = 0.0f; s = math::clamp(-c / a, 0.0f, 1.0f); }
            else if (t > 1.0f) { t = 1.0f; s = math::clamp((b - c) / a, 0.0f, 1.0f); }
        }
    }

    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
    Vec3 diff = c1 - c2;
    return glm::dot(diff, diff);
}

bool PhysicsWorld3D::capsule_vs_capsule(const Body3D& a, const Body3D& b, Contact3D& c) const {
    Vec3 a0, a1, b0, b1;
    capsule_segment(a, a0, a1);
    capsule_segment(b, b0, b1);

    Vec3 ca, cb;
    float dist_sq = closest_points_segments(a0, a1, b0, b1, ca, cb);
    float sum_r = a.radius + b.radius;

    if (dist_sq > sum_r * sum_r) return false;

    float dist = std::sqrt(dist_sq);
    if (dist < math::EPSILON) {
        c.normal = Vec3(0.0f, 1.0f, 0.0f);
        c.depth = sum_r;
    } else {
        c.normal = (cb - ca) / dist;
        c.depth = sum_r - dist;
    }
    c.point = ca + c.normal * a.radius;
    return true;
}

bool PhysicsWorld3D::capsule_vs_sphere(const Body3D& cap, const Body3D& sph, Contact3D& c) const {
    Vec3 a, b;
    capsule_segment(cap, a, b);

    Vec3 closest = closest_point_on_segment(a, b, sph.position);
    Vec3 diff = sph.position - closest;
    float dist_sq = glm::dot(diff, diff);
    float sum_r = cap.radius + sph.radius;

    if (dist_sq > sum_r * sum_r) return false;

    float dist = std::sqrt(dist_sq);
    if (dist < math::EPSILON) {
        c.normal = Vec3(0.0f, 1.0f, 0.0f);
        c.depth = sum_r;
    } else {
        c.normal = diff / dist;
        c.depth = sum_r - dist;
    }
    c.point = closest + c.normal * cap.radius;
    return true;
}

bool PhysicsWorld3D::capsule_vs_box(const Body3D& cap, const Body3D& box, Contact3D& c) const {
    Vec3 a, b;
    capsule_segment(cap, a, b);

    // Transform capsule segment into box local space
    Mat3 rot = glm::mat3_cast(box.rotation);
    Mat3 rot_inv = glm::transpose(rot);
    Vec3 la = rot_inv * (a - box.position);
    Vec3 lb = rot_inv * (b - box.position);

    // Find closest point on segment to box, then clamp to box
    // Sample several points on the segment and pick the one with minimum distance
    float best_dist_sq = std::numeric_limits<float>::max();
    Vec3 best_cap_pt, best_box_pt;

    auto test_point = [&](Vec3 local_pt) {
        Vec3 clamped;
        for (int i = 0; i < 3; ++i)
            clamped[i] = math::clamp(local_pt[i], -box.half_extents[i], box.half_extents[i]);
        Vec3 delta = local_pt - clamped;
        float d2 = glm::dot(delta, delta);
        if (d2 < best_dist_sq) {
            best_dist_sq = d2;
            best_cap_pt = local_pt;
            best_box_pt = clamped;
        }
    };

    // Test segment endpoints
    test_point(la);
    test_point(lb);

    // Also find closest point on segment to box center and to each face center
    test_point(closest_point_on_segment(la, lb, Vec3(0.0f)));
    for (int axis = 0; axis < 3; ++axis) {
        Vec3 face_pt(0.0f);
        face_pt[axis] = box.half_extents[axis];
        test_point(closest_point_on_segment(la, lb, face_pt));
        face_pt[axis] = -box.half_extents[axis];
        test_point(closest_point_on_segment(la, lb, face_pt));
    }

    // Now refine: closest point on segment to the best box point
    Vec3 refined_seg_pt = closest_point_on_segment(la, lb, best_box_pt);
    test_point(refined_seg_pt);
    // And closest box point to that refined segment point
    Vec3 refined_box_pt;
    for (int i = 0; i < 3; ++i)
        refined_box_pt[i] = math::clamp(refined_seg_pt[i], -box.half_extents[i], box.half_extents[i]);
    Vec3 rdelta = refined_seg_pt - refined_box_pt;
    float rd2 = glm::dot(rdelta, rdelta);
    if (rd2 < best_dist_sq) {
        best_dist_sq = rd2;
        best_cap_pt = refined_seg_pt;
        best_box_pt = refined_box_pt;
    }

    float dist = std::sqrt(best_dist_sq);

    if (dist > cap.radius) return false;

    Vec3 local_normal;
    if (dist < math::EPSILON) {
        // Capsule segment inside box — push out along smallest penetration axis
        Vec3 pen;
        for (int i = 0; i < 3; ++i)
            pen[i] = box.half_extents[i] - std::abs(best_cap_pt[i]);
        int min_axis = 0;
        if (pen[1] < pen[min_axis]) min_axis = 1;
        if (pen[2] < pen[min_axis]) min_axis = 2;
        local_normal = Vec3(0.0f);
        local_normal[min_axis] = (best_cap_pt[min_axis] < 0.0f) ? -1.0f : 1.0f;
        c.depth = pen[min_axis] + cap.radius;
    } else {
        local_normal = (best_cap_pt - best_box_pt) / dist;
        c.depth = cap.radius - dist;
    }

    // Transform back to world space
    c.normal = rot * local_normal;
    Vec3 world_cap_pt = box.position + rot * best_cap_pt;
    c.point = world_cap_pt - c.normal * cap.radius;
    return true;
}

// ── Resolution ──────────────────────────────────────────────────────────────

void PhysicsWorld3D::resolve_collision(Body3D& a, Body3D& b, const Contact3D& contact) {
    if (a.is_trigger || b.is_trigger) return;

    float inv_mass_sum = a.inv_mass + b.inv_mass;
    if (inv_mass_sum <= 0.0f) return;

    // Positional (Baumgarte) correction is applied once after the velocity solve
    // in step(); doing it here ran it once per velocity iteration and overshot.

    // Contact-relative vectors
    Vec3 ra = contact.point - a.position;
    Vec3 rb = contact.point - b.position;

    // World-space inverse inertia tensors
    Mat3 rot_a = glm::mat3_cast(a.rotation);
    Mat3 rot_b = glm::mat3_cast(b.rotation);
    Mat3 inv_I_a = rot_a * a.inv_inertia_tensor * glm::transpose(rot_a);
    Mat3 inv_I_b = rot_b * b.inv_inertia_tensor * glm::transpose(rot_b);

    // Relative velocity at contact point (includes angular)
    Vec3 rel_vel = (b.velocity + glm::cross(b.angular_velocity, rb))
                 - (a.velocity + glm::cross(a.angular_velocity, ra));
    float vel_along_normal = glm::dot(rel_vel, contact.normal);

    if (vel_along_normal > 0.0f) return;

    // Geometric mean restitution (physically correct)
    float e = std::sqrt(a.restitution * b.restitution);

    // Effective mass including rotational terms
    Vec3 ra_cross_n = glm::cross(ra, contact.normal);
    Vec3 rb_cross_n = glm::cross(rb, contact.normal);
    float angular_factor = glm::dot(contact.normal,
        glm::cross(inv_I_a * ra_cross_n, ra) +
        glm::cross(inv_I_b * rb_cross_n, rb));

    float j = -(1.0f + e) * vel_along_normal / (inv_mass_sum + angular_factor);

    Vec3 impulse = j * contact.normal;
    a.velocity -= impulse * a.inv_mass;
    b.velocity += impulse * b.inv_mass;
    a.angular_velocity -= inv_I_a * glm::cross(ra, impulse);
    b.angular_velocity += inv_I_b * glm::cross(rb, impulse);

    // Friction with angular contribution
    Vec3 tangent = rel_vel - contact.normal * vel_along_normal;
    float tangent_len = glm::length(tangent);
    if (tangent_len > math::EPSILON) {
        tangent /= tangent_len;
        Vec3 ra_cross_t = glm::cross(ra, tangent);
        Vec3 rb_cross_t = glm::cross(rb, tangent);
        float angular_factor_t = glm::dot(tangent,
            glm::cross(inv_I_a * ra_cross_t, ra) +
            glm::cross(inv_I_b * rb_cross_t, rb));

        float jt = -glm::dot(rel_vel, tangent) / (inv_mass_sum + angular_factor_t);
        float mu = std::sqrt(a.friction * b.friction);

        // Coulomb clamp against the magnitude of the normal impulse; using the
        // signed j would flip the bound (and inject energy) if j were negative.
        float jmax = std::abs(j) * mu;
        Vec3 friction_impulse = std::clamp(jt, -jmax, jmax) * tangent;

        a.velocity -= friction_impulse * a.inv_mass;
        b.velocity += friction_impulse * b.inv_mass;
        a.angular_velocity -= inv_I_a * glm::cross(ra, friction_impulse);
        b.angular_velocity += inv_I_b * glm::cross(rb, friction_impulse);
    }
}

// ── Queries ─────────────────────────────────────────────────────────────────

bool PhysicsWorld3D::raycast(Vec3 origin, Vec3 direction, float max_distance,
                              RayHit3D& hit, u16 layer_mask) const {
    Vec3 dir = glm::normalize(direction);
    float closest = max_distance;
    bool found = false;

    for (const auto& b : bodies_) {
        if (!(b.layer & layer_mask)) continue;

        if (b.shape == Body3D::Sphere) {
            Vec3 oc = origin - b.position;
            float a_c = glm::dot(dir, dir);
            float b_c = 2.0f * glm::dot(oc, dir);
            float c_c = glm::dot(oc, oc) - b.radius * b.radius;
            float disc = b_c * b_c - 4.0f * a_c * c_c;
            if (disc < 0.0f) continue;

            float t = (-b_c - std::sqrt(disc)) / (2.0f * a_c);
            if (t >= 0.0f && t < closest) {
                closest = t;
                hit.body_id = b.id;
                hit.point = origin + dir * t;
                hit.normal = glm::normalize(hit.point - b.position);
                hit.distance = t;
                found = true;
            }
        } else if (b.shape == Body3D::Capsule) {
            // Ray-capsule: test ray vs line-segment swept sphere
            Vec3 capA, capB;
            capsule_segment(b, capA, capB);
            Vec3 seg = capB - capA;
            Vec3 oc = origin - capA;

            float seg_dot_seg = glm::dot(seg, seg);
            float seg_dot_dir = glm::dot(seg, dir);
            float seg_dot_oc  = glm::dot(seg, oc);

            // Quadratic coefficients for infinite cylinder
            float a_c = glm::dot(dir, dir) - seg_dot_dir * seg_dot_dir / (seg_dot_seg + math::EPSILON);
            float b_c = 2.0f * (glm::dot(oc, dir) - seg_dot_dir * seg_dot_oc / (seg_dot_seg + math::EPSILON));
            float c_c = glm::dot(oc, oc) - seg_dot_oc * seg_dot_oc / (seg_dot_seg + math::EPSILON) - b.radius * b.radius;
            float disc = b_c * b_c - 4.0f * a_c * c_c;

            float best_t = closest;
            bool cap_found = false;

            if (disc >= 0.0f && std::abs(a_c) > math::EPSILON) {
                float t_cyl = (-b_c - std::sqrt(disc)) / (2.0f * a_c);
                if (t_cyl >= 0.0f && t_cyl < best_t) {
                    Vec3 pt = origin + dir * t_cyl;
                    float proj = glm::dot(pt - capA, seg) / seg_dot_seg;
                    if (proj >= 0.0f && proj <= 1.0f) {
                        best_t = t_cyl;
                        cap_found = true;
                    }
                }
            }

            // Test hemisphere caps (just use sphere tests at endpoints)
            for (int cap = 0; cap < 2; ++cap) {
                Vec3 center = (cap == 0) ? capA : capB;
                Vec3 co = origin - center;
                float sa = glm::dot(dir, dir);
                float sb = 2.0f * glm::dot(co, dir);
                float sc = glm::dot(co, co) - b.radius * b.radius;
                float sd = sb * sb - 4.0f * sa * sc;
                if (sd < 0.0f) continue;
                float st = (-sb - std::sqrt(sd)) / (2.0f * sa);
                if (st >= 0.0f && st < best_t) {
                    best_t = st;
                    cap_found = true;
                }
            }

            if (cap_found && best_t < closest) {
                closest = best_t;
                hit.body_id = b.id;
                hit.point = origin + dir * best_t;
                hit.distance = best_t;
                // Normal: closest point on capsule segment to hit point
                Vec3 cp = closest_point_on_segment(capA, capB, hit.point);
                hit.normal = glm::normalize(hit.point - cp);
                found = true;
            }
        } else {
            // OBB ray intersection — transform ray to box local space
            Mat3 rot = glm::mat3_cast(b.rotation);
            Mat3 rot_inv = glm::transpose(rot);
            Vec3 local_origin = rot_inv * (origin - b.position);
            Vec3 local_dir = rot_inv * dir;

            float tmin_val = 0.0f, tmax_val = max_distance;
            int hit_axis = -1;
            float hit_sign = 1.0f;

            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(local_dir[axis]) < math::EPSILON) {
                    if (local_origin[axis] < -b.half_extents[axis] ||
                        local_origin[axis] > b.half_extents[axis])
                        goto next_body_3d;
                    continue;
                }
                float inv_d = 1.0f / local_dir[axis];
                float t1 = (-b.half_extents[axis] - local_origin[axis]) * inv_d;
                float t2 = ( b.half_extents[axis] - local_origin[axis]) * inv_d;
                float sign = -1.0f;
                if (inv_d < 0.0f) { std::swap(t1, t2); sign = 1.0f; }
                if (t1 > tmin_val) { tmin_val = t1; hit_axis = axis; hit_sign = sign; }
                tmax_val = std::min(tmax_val, t2);
                if (tmax_val < tmin_val) goto next_body_3d;
            }

            if (tmin_val < closest) {
                closest = tmin_val;
                hit.body_id = b.id;
                hit.point = origin + dir * tmin_val;
                hit.distance = tmin_val;
                // Normal in local space, then rotate back to world
                Vec3 local_normal(0.0f);
                if (hit_axis >= 0)
                    local_normal[hit_axis] = hit_sign;
                hit.normal = rot * local_normal;
                found = true;
            }
        }
        next_body_3d:;
    }
    return found;
}

std::vector<u32> PhysicsWorld3D::overlap_sphere(Vec3 center, float radius,
                                                 u16 layer_mask) const {
    std::vector<u32> result;
    for (const auto& b : bodies_) {
        if (!(b.layer & layer_mask)) continue;

        if (b.shape == Body3D::Sphere) {
            float dist = glm::length(b.position - center);
            if (dist < radius + b.radius) result.push_back(b.id);
        } else {
            Vec3 closest;
            for (int i = 0; i < 3; ++i) {
                closest[i] = math::clamp(center[i],
                    b.position[i] - b.half_extents[i],
                    b.position[i] + b.half_extents[i]);
            }
            float dist_sq = glm::dot(center - closest, center - closest);
            if (dist_sq < radius * radius) result.push_back(b.id);
        }
    }
    return result;
}

std::vector<u32> PhysicsWorld3D::overlap_aabb(Vec3 min_pt, Vec3 max_pt,
                                               u16 layer_mask) const {
    std::vector<u32> result;
    for (const auto& b : bodies_) {
        if (!(b.layer & layer_mask)) continue;
        AABB aabb = body3d_aabb(b);
        if (aabb.max.x >= min_pt.x && aabb.min.x <= max_pt.x &&
            aabb.max.y >= min_pt.y && aabb.min.y <= max_pt.y &&
            aabb.max.z >= min_pt.z && aabb.min.z <= max_pt.z) {
            result.push_back(b.id);
        }
    }
    return result;
}

} // namespace nexus::physics
