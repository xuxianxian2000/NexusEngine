#include "nexus/renderer/gpu_particles.h"
#include "nexus/core/log.h"
#include <algorithm>
#include <cmath>

namespace nexus {

// ── Shaders ────────────────────────────────────────────────────────────────

static const char* GPU_PARTICLE_VERTEX = R"(
#version 330 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec4 a_Color;
layout(location = 2) in float a_Size;
layout(location = 3) in vec2 a_QuadVertex;

uniform mat4 u_ViewProjection;
uniform vec3 u_CameraRight;
uniform vec3 u_CameraUp;

out vec2 v_TexCoord;
out vec4 v_Color;

void main() {
    vec3 worldPos = a_Position
        + u_CameraRight * a_QuadVertex.x * a_Size
        + u_CameraUp * a_QuadVertex.y * a_Size;
    gl_Position = u_ViewProjection * vec4(worldPos, 1.0);
    v_TexCoord = a_QuadVertex * 0.5 + 0.5;
    v_Color = a_Color;
}
)";

static const char* GPU_PARTICLE_FRAGMENT = R"(
#version 330 core
in vec2 v_TexCoord;
in vec4 v_Color;
out vec4 FragColor;

uniform sampler2D u_Texture;
uniform bool u_HasTexture;

void main() {
    vec4 tex = u_HasTexture ? texture(u_Texture, v_TexCoord) : vec4(1.0);
    // Soft particle circle falloff
    float dist = length(v_TexCoord - vec2(0.5));
    float alpha = smoothstep(0.5, 0.4, dist);
    FragColor = v_Color * tex * vec4(1.0, 1.0, 1.0, alpha);
    if (FragColor.a < 0.01) discard;
}
)";

// ── Random number generation ───────────────────────────────────────────────

float GPUParticleSystem::rand_float() {
    // xorshift32
    rng_state_ ^= rng_state_ << 13;
    rng_state_ ^= rng_state_ >> 17;
    rng_state_ ^= rng_state_ << 5;
    return static_cast<float>(rng_state_) / static_cast<float>(0xFFFFFFFF);
}

float GPUParticleSystem::rand_range(float min, float max) {
    return min + rand_float() * (max - min);
}

Vec3 GPUParticleSystem::rand_cone(Vec3 direction, float angle) {
    float half_angle = glm::radians(angle * 0.5f);
    float cos_angle = std::cos(half_angle);

    // Random direction within cone
    float z = rand_range(cos_angle, 1.0f);
    float phi = rand_range(0.0f, 2.0f * 3.14159265f);
    float sin_theta = std::sqrt(1.0f - z * z);

    Vec3 local(sin_theta * std::cos(phi), sin_theta * std::sin(phi), z);

    // Build basis from direction
    Vec3 up = (std::abs(direction.y) > 0.9f) ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
    Vec3 right = glm::normalize(glm::cross(direction, up));
    up = glm::cross(right, direction);

    return glm::normalize(right * local.x + up * local.y + direction * local.z);
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

void GPUParticleSystem::init(rhi::RHI* rhi, const GPUParticleEmitterConfig& config) {
    rhi_ = rhi;
    config_ = config;
    particles_.reserve(config_.max_particles);
    alive_count_ = 0;

    shader_ = rhi_->create_shader(GPU_PARTICLE_VERTEX, GPU_PARTICLE_FRAGMENT);

    rhi::PipelineDesc pipe_desc;
    pipe_desc.shader = shader_;
    pipe_desc.blend = config_.additive_blend ? rhi::BlendMode::Additive : rhi::BlendMode::Alpha;
    pipe_desc.depth_test = true;
    pipe_desc.depth_write = false;  // particles don't write depth
    pipe_desc.cull = rhi::CullMode::None;
    pipe_desc.primitive = rhi::PrimitiveType::Triangles;
    pipe_desc.vertex_layout.stride = sizeof(float) * 10;  // pos(3)+color(4)+size(1)+quad(2)
    pipe_desc.vertex_layout.attributes = {
        {0, 3, 0, false},                        // position
        {1, 4, sizeof(float) * 3, false},         // color
        {2, 1, sizeof(float) * 7, false},          // size
        {3, 2, sizeof(float) * 8, false},          // quad vertex
    };
    pipeline_ = rhi_->create_pipeline(pipe_desc);

    // Create VBO with max capacity for particle quads (6 verts per particle)
    rhi::BufferDesc vbo_desc;
    vbo_desc.size = config_.max_particles * 6 * sizeof(float) * 10;
    vbo_desc.usage = rhi::BufferUsage::Dynamic;
    vbo_desc.data = nullptr;
    vbo_ = rhi_->create_buffer(vbo_desc);

    NX_INFO("GPUParticleSystem initialized: max {} particles", config_.max_particles);
}

void GPUParticleSystem::shutdown() {
    if (!rhi_) return;
    if (vbo_ != rhi::INVALID_HANDLE) rhi_->destroy_buffer(vbo_);
    if (pipeline_ != rhi::INVALID_HANDLE) rhi_->destroy_pipeline(pipeline_);
    if (shader_ != rhi::INVALID_HANDLE) rhi_->destroy_shader(shader_);
    vbo_ = rhi::INVALID_HANDLE;
    pipeline_ = rhi::INVALID_HANDLE;
    shader_ = rhi::INVALID_HANDLE;
    particles_.clear();
    alive_count_ = 0;
    rhi_ = nullptr;
}

void GPUParticleSystem::update(float dt) {
    emit_particles(dt);
    simulate_particles(dt);
    compact_dead_particles();
}

void GPUParticleSystem::emit_particles(float dt) {
    emission_accumulator_ += config_.emission_rate * dt;
    u32 to_emit = static_cast<u32>(emission_accumulator_);
    emission_accumulator_ -= static_cast<float>(to_emit);

    for (u32 i = 0; i < to_emit && alive_count_ < config_.max_particles; ++i) {
        GPUParticle p;
        p.position = config_.position;
        float speed = rand_range(config_.min_speed, config_.max_speed);
        Vec3 dir = rand_cone(config_.direction, config_.spread_angle);
        p.velocity = dir * speed;
        p.max_lifetime = rand_range(config_.min_lifetime, config_.max_lifetime);
        p.lifetime = p.max_lifetime;
        p.color = config_.start_color;
        p.size = config_.start_size;
        p.pad[0] = p.pad[1] = p.pad[2] = 0.0f;

        if (alive_count_ < particles_.size()) {
            particles_[alive_count_] = p;
        } else {
            particles_.push_back(p);
        }
        alive_count_++;
    }
}

void GPUParticleSystem::simulate_particles(float dt) {
    for (u32 i = 0; i < alive_count_; ++i) {
        auto& p = particles_[i];
        p.lifetime -= dt;

        // Apply gravity
        p.velocity.y += config_.gravity * dt;

        // Integrate position
        p.position += p.velocity * dt;

        // Interpolate color and size based on life fraction
        float t = p.max_lifetime > 0.0f
                      ? 1.0f - (p.lifetime / p.max_lifetime)  // 0 at birth, 1 at death
                      : 1.0f;
        t = glm::clamp(t, 0.0f, 1.0f);
        p.color = glm::mix(config_.start_color, config_.end_color, t);
        p.size = glm::mix(config_.start_size, config_.end_size, t);
    }
}

void GPUParticleSystem::compact_dead_particles() {
    // Remove dead particles by swapping with the last alive
    u32 i = 0;
    while (i < alive_count_) {
        if (particles_[i].lifetime <= 0.0f) {
            alive_count_--;
            if (i < alive_count_) {
                particles_[i] = particles_[alive_count_];
            }
        } else {
            ++i;
        }
    }
}

void GPUParticleSystem::render(const Mat4& view, const Mat4& projection,
                                Vec3 camera_right, Vec3 camera_up) {
    if (!rhi_ || alive_count_ == 0) return;

    // Build vertex data: 6 verts per particle (2 triangles)
    static const Vec2 quad_verts[6] = {
        {-1, -1}, {1, -1}, {1, 1},
        {-1, -1}, {1,  1}, {-1, 1}
    };

    // 10 floats per vertex: pos(3) + color(4) + size(1) + quadvert(2)
    std::vector<float> vertex_data;
    vertex_data.reserve(alive_count_ * 6 * 10);

    for (u32 i = 0; i < alive_count_; ++i) {
        const auto& p = particles_[i];
        for (int q = 0; q < 6; ++q) {
            vertex_data.push_back(p.position.x);
            vertex_data.push_back(p.position.y);
            vertex_data.push_back(p.position.z);
            vertex_data.push_back(p.color.r);
            vertex_data.push_back(p.color.g);
            vertex_data.push_back(p.color.b);
            vertex_data.push_back(p.color.a);
            vertex_data.push_back(p.size);
            vertex_data.push_back(quad_verts[q].x);
            vertex_data.push_back(quad_verts[q].y);
        }
    }

    // Upload and render
    rhi_->update_buffer(vbo_, vertex_data.data(),
                        static_cast<u32>(vertex_data.size() * sizeof(float)));

    Mat4 vp = projection * view;
    rhi_->bind_shader(shader_);
    rhi_->bind_pipeline(pipeline_);
    rhi_->set_uniform_mat4(shader_, "u_ViewProjection", vp);
    rhi_->set_uniform_vec3(shader_, "u_CameraRight", camera_right);
    rhi_->set_uniform_vec3(shader_, "u_CameraUp", camera_up);

    bool has_tex = (texture_ != rhi::INVALID_HANDLE);
    rhi_->set_uniform_int(shader_, "u_HasTexture", has_tex ? 1 : 0);
    if (has_tex) {
        rhi_->bind_texture(texture_, 0);
        rhi_->set_uniform_int(shader_, "u_Texture", 0);
    }

    rhi_->bind_vertex_buffer(vbo_);
    rhi_->draw(alive_count_ * 6);
}

} // namespace nexus
