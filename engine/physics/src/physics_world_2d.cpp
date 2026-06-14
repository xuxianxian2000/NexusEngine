#include "nexus/physics/physics_world_2d.h"
#include "nexus/core/log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace nexus::physics {

// ── Body2D mass computation ─────────────────────────────────────────────────

void Body2D::compute_mass() {
    if (type == Static) {
        mass = 0.0f;
        inv_mass = 0.0f;
        inertia = 0.0f;
        inv_inertia = 0.0f;
        return;
    }

    float area = 0.0f;
    if (shape == Box) {
        area = 4.0f * half_size.x * half_size.y;
    } else {
        area = math::PI * radius * radius;
    }

    mass = density * area;
    inv_mass = (mass > 0.0f) ? 1.0f / mass : 0.0f;

    // Moment of inertia
    if (shape == Box) {
        float w = 2.0f * half_size.x;
        float h = 2.0f * half_size.y;
        inertia = mass * (w * w + h * h) / 12.0f;
    } else {
        inertia = 0.5f * mass * radius * radius;
    }

    if (fixed_rotation) {
        inertia = 0.0f;
        inv_inertia = 0.0f;
    } else {
        inv_inertia = (inertia > 0.0f) ? 1.0f / inertia : 0.0f;
    }
}

// ── PhysicsWorld2D ──────────────────────────────────────────────────────────

PhysicsWorld2D::PhysicsWorld2D(Vec2 gravity) : gravity_(gravity) {
    bodies_.reserve(256);
}

u32 PhysicsWorld2D::create_body(const Body2D& desc) {
    Body2D body = desc;
    body.id = next_id_++;
    body.compute_mass();
    bodies_.push_back(body);
    return body.id;
}

void PhysicsWorld2D::destroy_body(u32 id) {
    bodies_.erase(
        std::remove_if(bodies_.begin(), bodies_.end(),
            [id](const Body2D& b) { return b.id == id; }),
        bodies_.end());
}

Body2D* PhysicsWorld2D::get_body(u32 id) {
    for (auto& b : bodies_) {
        if (b.id == id) return &b;
    }
    return nullptr;
}

const Body2D* PhysicsWorld2D::get_body(u32 id) const {
    for (const auto& b : bodies_) {
        if (b.id == id) return &b;
    }
    return nullptr;
}

void PhysicsWorld2D::step(float dt, u32 velocity_iterations, u32 /*position_iterations*/) {
    if (dt <= 0.0f) return;

    // Integrate forces/velocity
    integrate(dt);

    // Detect collisions
    broadphase();

    // Wake sleeping bodies involved in collisions
    for (auto& pair : contacts_) {
        Body2D* a = get_body(pair.body_a);
        Body2D* b = get_body(pair.body_b);
        if (a && b) {
            if (a->sleeping && b->type == Body2D::Dynamic && !b->sleeping) a->wake();
            if (b->sleeping && a->type == Body2D::Dynamic && !a->sleeping) b->wake();
        }
    }

    // Resolve velocities (iterative impulse solver)
    for (u32 iter = 0; iter < velocity_iterations; ++iter) {
        for (auto& pair : contacts_) {
            Body2D* a = get_body(pair.body_a);
            Body2D* b = get_body(pair.body_b);
            if (a && b) resolve_collision(*a, *b, pair.contact);
        }
    }

    // Positional (Baumgarte) correction: a single pass after the velocity solve,
    // using the original penetration depth.
    {
        const float percent = 0.8f;
        const float slop = 0.01f;
        for (auto& pair : contacts_) {
            Body2D* a = get_body(pair.body_a);
            Body2D* b = get_body(pair.body_b);
            if (!a || !b || a->is_trigger || b->is_trigger) continue;
            float inv_mass_sum = a->inv_mass + b->inv_mass;
            if (inv_mass_sum <= 0.0f) continue;
            Vec2 correction = pair.contact.normal *
                (std::max(pair.contact.depth - slop, 0.0f) / inv_mass_sum) * percent;
            a->position -= correction * a->inv_mass;
            b->position += correction * b->inv_mass;
        }
    }

    update_sleeping(dt);

    // Fire contact callbacks
    if (contact_callback_) {
        for (const auto& pair : contacts_) {
            contact_callback_(pair);
        }
    }
}

void PhysicsWorld2D::apply_force(u32 id, Vec2 force) {
    if (auto* b = get_body(id)) { b->wake(); b->force += force; }
}

void PhysicsWorld2D::apply_impulse(u32 id, Vec2 impulse) {
    if (auto* b = get_body(id)) {
        b->wake();
        b->velocity += impulse * b->inv_mass;
    }
}

void PhysicsWorld2D::apply_torque(u32 id, float torque) {
    if (auto* b = get_body(id)) { b->wake(); b->torque += torque; }
}

// ── Integration ─────────────────────────────────────────────────────────────

void PhysicsWorld2D::integrate(float dt) {
    for (auto& b : bodies_) {
        if (b.type == Body2D::Static) continue;
        if (b.sleeping) continue;

        if (b.type == Body2D::Dynamic) {
            // Apply gravity
            Vec2 accel = gravity_ * b.gravity_scale + b.force * b.inv_mass;
            b.velocity += accel * dt;
            b.angular_velocity += b.torque * b.inv_inertia * dt;

            // Damping
            b.velocity *= 1.0f / (1.0f + b.linear_damping * dt);
            b.angular_velocity *= 1.0f / (1.0f + b.angular_damping * dt);
        }

        // Integrate position
        b.position += b.velocity * dt;
        b.rotation += b.angular_velocity * dt;

        // Clear accumulators
        b.force = {0.0f, 0.0f};
        b.torque = 0.0f;
    }
}

void PhysicsWorld2D::update_sleeping(float dt) {
    for (auto& b : bodies_) {
        if (b.type == Body2D::Static) continue;
        float energy = glm::dot(b.velocity, b.velocity)
                     + b.angular_velocity * b.angular_velocity;
        if (energy < Body2D::SLEEP_THRESHOLD) {
            b.sleep_timer += dt;
            if (b.sleep_timer >= Body2D::SLEEP_TIME) {
                b.sleeping = true;
                b.velocity = Vec2(0.0f);
                b.angular_velocity = 0.0f;
            }
        } else {
            b.sleep_timer = 0.0f;
            b.sleeping = false;
        }
    }
}

// ── Broadphase ──────────────────────────────────────────────────────────────

static void body_aabb(const Body2D& b, Vec2& out_min, Vec2& out_max) {
    if (b.shape == Body2D::Circle) {
        out_min = b.position - Vec2(b.radius);
        out_max = b.position + Vec2(b.radius);
    } else {
        // Conservative AABB for rotated box
        float c = std::abs(std::cos(b.rotation));
        float s = std::abs(std::sin(b.rotation));
        float hx = b.half_size.x * c + b.half_size.y * s;
        float hy = b.half_size.x * s + b.half_size.y * c;
        out_min = b.position - Vec2(hx, hy);
        out_max = b.position + Vec2(hx, hy);
    }
}

void PhysicsWorld2D::broadphase() {
    contacts_.clear();

    // Build spatial hash
    spatial_hash_.clear();
    std::unordered_map<u32, std::pair<Vec2, Vec2>> aabb_cache;
    for (const auto& b : bodies_) {
        Vec2 bmin, bmax;
        body_aabb(b, bmin, bmax);
        aabb_cache[b.id] = {bmin, bmax};
        spatial_hash_.insert(b.id, bmin, bmax);
    }

    // Query candidate pairs via spatial hash
    std::unordered_set<u64> seen_pairs;
    std::vector<std::pair<u32, u32>> candidates;
    for (const auto& b : bodies_) {
        auto& [bmin, bmax] = aabb_cache[b.id];
        spatial_hash_.query(bmin, bmax, seen_pairs, b.id, candidates);
    }

    // Narrow phase on candidate pairs
    for (const auto& [id_lo, id_hi] : candidates) {
        Body2D* a = get_body(id_lo);
        Body2D* b = get_body(id_hi);
        if (!a || !b) continue;

        if (a->type == Body2D::Static && b->type == Body2D::Static) continue;
        if (!(a->layer & b->mask) || !(b->layer & a->mask)) continue;

        // AABB overlap confirmation
        auto& [a_min, a_max] = aabb_cache[a->id];
        auto& [b_min, b_max] = aabb_cache[b->id];
        if (a_max.x < b_min.x || a_min.x > b_max.x ||
            a_max.y < b_min.y || a_min.y > b_max.y) continue;

        Contact2D contact;
        if (narrowphase(*a, *b, contact)) {
            contacts_.push_back({a->id, b->id, contact});
        }
    }
}

// ── Narrow phase ────────────────────────────────────────────────────────────

bool PhysicsWorld2D::narrowphase(const Body2D& a, const Body2D& b, Contact2D& contact) const {
    if (a.shape == Body2D::Circle && b.shape == Body2D::Circle) {
        return circle_vs_circle(a, b, contact);
    }
    if (a.shape == Body2D::Box && b.shape == Body2D::Box) {
        return box_vs_box(a, b, contact);
    }
    if (a.shape == Body2D::Circle && b.shape == Body2D::Box) {
        return circle_vs_box(a, b, contact);
    }
    if (a.shape == Body2D::Box && b.shape == Body2D::Circle) {
        bool result = circle_vs_box(b, a, contact);
        if (result) contact.normal = -contact.normal;
        return result;
    }
    return false;
}

bool PhysicsWorld2D::circle_vs_circle(const Body2D& a, const Body2D& b, Contact2D& c) const {
    Vec2 diff = b.position - a.position;
    float dist_sq = glm::dot(diff, diff);
    float sum_r = a.radius + b.radius;

    if (dist_sq > sum_r * sum_r) return false;

    float dist = std::sqrt(dist_sq);
    if (dist < math::EPSILON) {
        c.normal = {0.0f, 1.0f};
        c.depth = sum_r;
        c.point = a.position;
    } else {
        c.normal = diff / dist;
        c.depth = sum_r - dist;
        c.point = a.position + c.normal * a.radius;
    }
    return true;
}

bool PhysicsWorld2D::box_vs_box(const Body2D& a, const Body2D& b, Contact2D& c) const {
    // OBB vs OBB using Separating Axis Theorem with rotation support
    float cos_a = std::cos(a.rotation), sin_a = std::sin(a.rotation);
    float cos_b = std::cos(b.rotation), sin_b = std::sin(b.rotation);

    // Build local axes for each OBB
    Vec2 axes[4] = {
        {cos_a, sin_a}, {-sin_a, cos_a},   // A's local X and Y axes
        {cos_b, sin_b}, {-sin_b, cos_b}    // B's local X and Y axes
    };

    Vec2 diff = b.position - a.position;
    float min_overlap = std::numeric_limits<float>::max();
    Vec2 min_axis{0.0f, 0.0f};

    // Get corner vertices of each OBB
    auto get_corners = [](Vec2 pos, Vec2 half, float cs, float sn) {
        Vec2 ax{cs, sn};
        Vec2 ay{-sn, cs};
        Vec2 ex = ax * half.x;
        Vec2 ey = ay * half.y;
        return std::array<Vec2, 4>{{
            pos - ex - ey, pos + ex - ey,
            pos + ex + ey, pos - ex + ey
        }};
    };

    auto corners_a = get_corners(a.position, a.half_size, cos_a, sin_a);
    auto corners_b = get_corners(b.position, b.half_size, cos_b, sin_b);

    // Test all 4 separating axes
    for (int i = 0; i < 4; ++i) {
        Vec2 axis = axes[i];

        // Project both OBBs onto this axis
        float min_a_proj =  std::numeric_limits<float>::max();
        float max_a_proj = -std::numeric_limits<float>::max();
        float min_b_proj =  std::numeric_limits<float>::max();
        float max_b_proj = -std::numeric_limits<float>::max();

        for (size_t j = 0; j < 4; ++j) {
            float pa = glm::dot(corners_a[j], axis);
            float pb = glm::dot(corners_b[j], axis);
            min_a_proj = std::min(min_a_proj, pa);
            max_a_proj = std::max(max_a_proj, pa);
            min_b_proj = std::min(min_b_proj, pb);
            max_b_proj = std::max(max_b_proj, pb);
        }

        // Check for separation
        float overlap = std::min(max_a_proj, max_b_proj) - std::max(min_a_proj, min_b_proj);
        if (overlap <= 0.0f) return false; // Separating axis found

        if (overlap < min_overlap) {
            min_overlap = overlap;
            min_axis = axis;
        }
    }

    // Ensure normal points from A to B
    if (glm::dot(min_axis, diff) < 0.0f) {
        min_axis = -min_axis;
    }

    c.normal = min_axis;
    c.depth = min_overlap;
    c.point = a.position + c.normal * glm::dot(a.half_size, Vec2(std::abs(glm::dot(axes[0], min_axis)),
                                                                    std::abs(glm::dot(axes[1], min_axis))));
    return true;
}

bool PhysicsWorld2D::circle_vs_box(const Body2D& circle, const Body2D& box, Contact2D& c) const {
    // Transform circle center into box local space for proper OBB support
    float cos_r = std::cos(-box.rotation);
    float sin_r = std::sin(-box.rotation);
    Vec2 world_diff = circle.position - box.position;
    Vec2 local_diff = { cos_r * world_diff.x - sin_r * world_diff.y,
                        sin_r * world_diff.x + cos_r * world_diff.y };

    // Clamp to box extents
    Vec2 closest;
    closest.x = math::clamp(local_diff.x, -box.half_size.x, box.half_size.x);
    closest.y = math::clamp(local_diff.y, -box.half_size.y, box.half_size.y);

    Vec2 delta = local_diff - closest;
    float dist_sq = glm::dot(delta, delta);

    if (dist_sq > circle.radius * circle.radius) return false;

    float dist = std::sqrt(dist_sq);
    Vec2 local_normal;
    if (dist < math::EPSILON) {
        // Circle center inside box
        float pen_x = box.half_size.x - std::abs(local_diff.x);
        float pen_y = box.half_size.y - std::abs(local_diff.y);
        if (pen_x < pen_y) {
            local_normal = {(local_diff.x < 0.0f) ? -1.0f : 1.0f, 0.0f};
            c.depth = pen_x + circle.radius;
        } else {
            local_normal = {0.0f, (local_diff.y < 0.0f) ? -1.0f : 1.0f};
            c.depth = pen_y + circle.radius;
        }
    } else {
        local_normal = delta / dist;
        c.depth = circle.radius - dist;
    }
    // Transform normal back to world space
    float cos_fwd = std::cos(box.rotation);
    float sin_fwd = std::sin(box.rotation);
    c.normal = { cos_fwd * local_normal.x - sin_fwd * local_normal.y,
                 sin_fwd * local_normal.x + cos_fwd * local_normal.y };
    c.point = circle.position - c.normal * circle.radius;
    return true;
}

// ── Collision resolution ────────────────────────────────────────────────────

void PhysicsWorld2D::resolve_collision(Body2D& a, Body2D& b, const Contact2D& contact) {
    // Don't resolve triggers
    if (a.is_trigger || b.is_trigger) return;

    float inv_mass_sum = a.inv_mass + b.inv_mass;
    if (inv_mass_sum <= 0.0f) return;

    // Positional (Baumgarte) correction is applied once after the velocity solve
    // in step(); doing it here ran it once per velocity iteration and overshot.

    // Contact-relative vectors
    Vec2 ra = contact.point - a.position;
    Vec2 rb = contact.point - b.position;

    // 2D cross product helper: cross(v, n) = v.x*n.y - v.y*n.x
    auto cross2d = [](Vec2 v, Vec2 n) { return v.x * n.y - v.y * n.x; };
    // Perpendicular of scalar cross with vector: s × v = (-s*v.y, s*v.x)
    auto cross_sv = [](float s, Vec2 v) { return Vec2(-s * v.y, s * v.x); };

    // Relative velocity at contact point (includes angular)
    Vec2 rel_vel = (b.velocity + cross_sv(b.angular_velocity, rb))
                 - (a.velocity + cross_sv(a.angular_velocity, ra));
    float vel_along_normal = glm::dot(rel_vel, contact.normal);

    // Don't resolve if separating
    if (vel_along_normal > 0.0f) return;

    // Geometric mean restitution (physically correct)
    float e = std::sqrt(a.restitution * b.restitution);

    // Effective mass including rotational terms
    float ra_cross_n = cross2d(ra, contact.normal);
    float rb_cross_n = cross2d(rb, contact.normal);
    float angular_factor = ra_cross_n * ra_cross_n * a.inv_inertia
                         + rb_cross_n * rb_cross_n * b.inv_inertia;

    float j = -(1.0f + e) * vel_along_normal / (inv_mass_sum + angular_factor);

    Vec2 impulse = j * contact.normal;
    a.velocity -= impulse * a.inv_mass;
    b.velocity += impulse * b.inv_mass;
    a.angular_velocity -= a.inv_inertia * cross2d(ra, impulse);
    b.angular_velocity += b.inv_inertia * cross2d(rb, impulse);

    // Friction with angular contribution
    Vec2 tangent = rel_vel - contact.normal * vel_along_normal;
    float tangent_len = glm::length(tangent);
    if (tangent_len > math::EPSILON) {
        tangent /= tangent_len;
        float ra_cross_t = cross2d(ra, tangent);
        float rb_cross_t = cross2d(rb, tangent);
        float angular_factor_t = ra_cross_t * ra_cross_t * a.inv_inertia
                               + rb_cross_t * rb_cross_t * b.inv_inertia;

        float jt = -glm::dot(rel_vel, tangent) / (inv_mass_sum + angular_factor_t);
        float mu = std::sqrt(a.friction * b.friction);

        // Coulomb clamp against the magnitude of the normal impulse; using the
        // signed j would flip the bound (and inject energy) if j were negative.
        float jmax = std::abs(j) * mu;
        Vec2 friction_impulse = std::clamp(jt, -jmax, jmax) * tangent;

        a.velocity -= friction_impulse * a.inv_mass;
        b.velocity += friction_impulse * b.inv_mass;
        a.angular_velocity -= a.inv_inertia * cross2d(ra, friction_impulse);
        b.angular_velocity += b.inv_inertia * cross2d(rb, friction_impulse);
    }
}

// ── Queries ─────────────────────────────────────────────────────────────────

bool PhysicsWorld2D::raycast(Vec2 origin, Vec2 direction, float max_distance,
                              RayHit2D& hit, u16 layer_mask) const {
    Vec2 dir = glm::normalize(direction);
    float closest = max_distance;
    bool found = false;

    for (const auto& b : bodies_) {
        if (!(b.layer & layer_mask)) continue;

        if (b.shape == Body2D::Circle) {
            Vec2 oc = origin - b.position;
            float a_coeff = glm::dot(dir, dir);
            float b_coeff = 2.0f * glm::dot(oc, dir);
            float c_coeff = glm::dot(oc, oc) - b.radius * b.radius;
            float disc = b_coeff * b_coeff - 4.0f * a_coeff * c_coeff;
            if (disc < 0.0f) continue;

            float t = (-b_coeff - std::sqrt(disc)) / (2.0f * a_coeff);
            if (t >= 0.0f && t < closest) {
                closest = t;
                hit.body_id = b.id;
                hit.point = origin + dir * t;
                hit.normal = glm::normalize(hit.point - b.position);
                hit.distance = t;
                found = true;
            }
        } else {
            // OBB ray intersection — transform ray to box local space
            float cos_r = std::cos(-b.rotation);
            float sin_r = std::sin(-b.rotation);
            Vec2 local_origin = {
                cos_r * (origin.x - b.position.x) - sin_r * (origin.y - b.position.y),
                sin_r * (origin.x - b.position.x) + cos_r * (origin.y - b.position.y)
            };
            Vec2 local_dir = { cos_r * dir.x - sin_r * dir.y,
                               sin_r * dir.x + cos_r * dir.y };

            float tmin_val = 0.0f, tmax_val = max_distance;
            int hit_axis = -1;
            float hit_sign = 1.0f;

            for (int axis = 0; axis < 2; ++axis) {
                if (std::abs(local_dir[axis]) < math::EPSILON) {
                    if (local_origin[axis] < -b.half_size[axis] ||
                        local_origin[axis] > b.half_size[axis])
                        goto next_body;
                    continue;
                }
                float inv_d = 1.0f / local_dir[axis];
                float t1 = (-b.half_size[axis] - local_origin[axis]) * inv_d;
                float t2 = ( b.half_size[axis] - local_origin[axis]) * inv_d;
                float sign = -1.0f;
                if (inv_d < 0.0f) { std::swap(t1, t2); sign = 1.0f; }
                if (t1 > tmin_val) { tmin_val = t1; hit_axis = axis; hit_sign = sign; }
                tmax_val = std::min(tmax_val, t2);
                if (tmax_val < tmin_val) goto next_body;
            }

            if (tmin_val < closest) {
                closest = tmin_val;
                hit.body_id = b.id;
                hit.point = origin + dir * tmin_val;
                hit.distance = tmin_val;
                // Normal in local space, then rotate back to world
                Vec2 local_normal(0.0f);
                if (hit_axis >= 0) local_normal[hit_axis] = hit_sign;
                float cos_fwd = std::cos(b.rotation);
                float sin_fwd = std::sin(b.rotation);
                hit.normal = { cos_fwd * local_normal.x - sin_fwd * local_normal.y,
                               sin_fwd * local_normal.x + cos_fwd * local_normal.y };
                found = true;
            }
        }
        next_body:;
    }
    return found;
}

std::vector<u32> PhysicsWorld2D::overlap_circle(Vec2 center, float radius,
                                                 u16 layer_mask) const {
    std::vector<u32> result;
    for (const auto& b : bodies_) {
        if (!(b.layer & layer_mask)) continue;

        if (b.shape == Body2D::Circle) {
            float dist = glm::length(b.position - center);
            if (dist < radius + b.radius) result.push_back(b.id);
        } else {
            // Circle vs AABB
            Vec2 closest;
            closest.x = math::clamp(center.x, b.position.x - b.half_size.x,
                                    b.position.x + b.half_size.x);
            closest.y = math::clamp(center.y, b.position.y - b.half_size.y,
                                    b.position.y + b.half_size.y);
            float dist_sq = glm::dot(center - closest, center - closest);
            if (dist_sq < radius * radius) result.push_back(b.id);
        }
    }
    return result;
}

std::vector<u32> PhysicsWorld2D::overlap_aabb(Vec2 min_pt, Vec2 max_pt,
                                               u16 layer_mask) const {
    std::vector<u32> result;
    for (const auto& b : bodies_) {
        if (!(b.layer & layer_mask)) continue;

        Vec2 bmin, bmax;
        body_aabb(b, bmin, bmax);

        if (bmax.x >= min_pt.x && bmin.x <= max_pt.x &&
            bmax.y >= min_pt.y && bmin.y <= max_pt.y) {
            result.push_back(b.id);
        }
    }
    return result;
}

} // namespace nexus::physics
