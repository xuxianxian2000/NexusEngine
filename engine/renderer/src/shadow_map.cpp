#include "nexus/renderer/shadow_map.h"
#include "nexus/core/log.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace nexus {

// ── Shadow depth shaders ────────────────────────────────────────────────────

namespace shadow_shaders {

const char* DEPTH_VERTEX = R"(
#version 330 core
layout (location = 0) in vec3 a_Position;

uniform mat4 u_LightViewProjection;
uniform mat4 u_Model;

void main() {
    gl_Position = u_LightViewProjection * u_Model * vec4(a_Position, 1.0);
}
)";

const char* DEPTH_FRAGMENT = R"(
#version 330 core
void main() {
    // Depth is written automatically by the depth buffer
}
)";

// ── PCF Soft Shadow sampling (used by lighting shaders) ─────────────────────
// This is exported as a utility string that forward/deferred renderers can
// #include or paste into their fragment shaders.

const char* SHADOW_SAMPLING_GLSL = R"(
// ── Poisson disk (16 taps) for soft shadow sampling ──────────────────────
const vec2 poissonDisk[16] = vec2[](
    vec2(-0.94201624, -0.39906216),
    vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870),
    vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432),
    vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845),
    vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554),
    vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023),
    vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507),
    vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367),
    vec2( 0.14383161, -0.14100790)
);

// ── PCF shadow for directional/CSM shadow maps ──────────────────────────
// coord: shadow-map projected XY, depth: comparison depth, texelSize: 1/shadow_resolution
float sampleShadowPCF(sampler2D shadowMap, vec3 projCoords, float bias, float texelSize) {
    float currentDepth = projCoords.z - bias;
    if (currentDepth > 1.0) return 1.0;

    float shadow = 0.0;
    // 16-tap Poisson disk PCF
    for (int i = 0; i < 16; ++i) {
        float pcfDepth = texture(shadowMap, projCoords.xy + poissonDisk[i] * texelSize * 2.0).r;
        shadow += (currentDepth > pcfDepth) ? 0.0 : 1.0;
    }
    return shadow / 16.0;
}

// ── Simple 3x3 PCF fallback (cheaper) ───────────────────────────────────
float sampleShadowPCF3x3(sampler2D shadowMap, vec3 projCoords, float bias, float texelSize) {
    float currentDepth = projCoords.z - bias;
    if (currentDepth > 1.0) return 1.0;

    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r;
            shadow += (currentDepth > pcfDepth) ? 0.0 : 1.0;
        }
    }
    return shadow / 9.0;
}

// ── Point light soft shadow (PCF on cubemap depth) ──────────────────────
float samplePointShadowPCF(samplerCube shadowCube, vec3 fragToLight, float currentDist,
                            float farPlane, float bias) {
    float shadow = 0.0;
    float diskRadius = 0.02;
    // 20-tap offset directions for cubemap PCF
    vec3 sampleOffsets[20] = vec3[](
        vec3( 1,  1,  1), vec3( 1, -1,  1), vec3(-1, -1,  1), vec3(-1,  1,  1),
        vec3( 1,  1, -1), vec3( 1, -1, -1), vec3(-1, -1, -1), vec3(-1,  1, -1),
        vec3( 1,  1,  0), vec3( 1, -1,  0), vec3(-1, -1,  0), vec3(-1,  1,  0),
        vec3( 1,  0,  1), vec3(-1,  0,  1), vec3( 1,  0, -1), vec3(-1,  0, -1),
        vec3( 0,  1,  1), vec3( 0, -1,  1), vec3( 0, -1, -1), vec3( 0,  1, -1)
    );

    float currentDepth = currentDist / farPlane - bias;
    for (int i = 0; i < 20; ++i) {
        float closestDepth = texture(shadowCube, fragToLight + sampleOffsets[i] * diskRadius).r;
        shadow += (currentDepth > closestDepth) ? 0.0 : 1.0;
    }
    return shadow / 20.0;
}

// ── PCSS (Percentage-Closer Soft Shadows) ──────────────────────────────
// Provides contact-hardening shadows: sharp near contact, soft far away.
// lightSize: world-space light size (larger = softer), searchRadius: blocker search texels
float findAverageBlockerDepth(sampler2D shadowMap, vec2 uv, float receiverDepth,
                               float searchRadius, float texelSize) {
    float blockerSum = 0.0;
    float numBlockers = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 offset = poissonDisk[i] * searchRadius * texelSize;
        float sampleDepth = texture(shadowMap, uv + offset).r;
        if (sampleDepth < receiverDepth) {
            blockerSum += sampleDepth;
            numBlockers += 1.0;
        }
    }
    if (numBlockers < 0.5) return -1.0; // No blockers found
    return blockerSum / numBlockers;
}

float sampleShadowPCSS(sampler2D shadowMap, vec3 projCoords, float bias,
                        float texelSize, float lightSize) {
    float receiverDepth = projCoords.z - bias;
    if (receiverDepth > 1.0) return 1.0;

    // Step 1: Blocker search
    float searchRadius = lightSize * 20.0;
    float avgBlockerDepth = findAverageBlockerDepth(shadowMap, projCoords.xy,
                                                     receiverDepth, searchRadius, texelSize);
    if (avgBlockerDepth < 0.0) return 1.0; // No blockers = fully lit

    // Step 2: Penumbra estimation
    float penumbraWidth = (receiverDepth - avgBlockerDepth) * lightSize / avgBlockerDepth;
    float filterRadius = penumbraWidth * 10.0;

    // Step 3: Variable-size PCF
    float shadow = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 offset = poissonDisk[i] * filterRadius * texelSize;
        float sampleDepth = texture(shadowMap, projCoords.xy + offset).r;
        shadow += (receiverDepth > sampleDepth) ? 0.0 : 1.0;
    }
    return shadow / 16.0;
}
)";

const char* POINT_DEPTH_VERTEX = R"(
#version 330 core
layout (location = 0) in vec3 a_Position;

uniform mat4 u_Model;
uniform mat4 u_LightViewProjection;

out vec3 v_WorldPos;

void main() {
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    v_WorldPos = worldPos.xyz;
    gl_Position = u_LightViewProjection * worldPos;
}
)";

const char* POINT_DEPTH_FRAGMENT = R"(
#version 330 core
in vec3 v_WorldPos;

uniform vec3  u_LightPos;
uniform float u_FarPlane;

void main() {
    float dist = length(v_WorldPos - u_LightPos);
    gl_FragDepth = dist / u_FarPlane;
}
)";

} // namespace shadow_shaders

// ── CascadedShadowMap ───────────────────────────────────────────────────────

void CascadedShadowMap::init(rhi::RHI* rhi, const Config& config) {
    rhi_ = rhi;
    config_ = config;
    // Clamp to the fixed-size arrays' capacity; num_cascades is caller-settable.
    if (config_.num_cascades < 1) config_.num_cascades = 1;
    if (config_.num_cascades > MAX_CASCADES) config_.num_cascades = MAX_CASCADES;
    splits_.resize(config_.num_cascades + 1);

    for (u32 i = 0; i < config_.num_cascades; ++i) {
        rhi::TextureDesc tex_desc;
        tex_desc.width = config_.resolution;
        tex_desc.height = config_.resolution;
        tex_desc.format = rhi::TextureFormat::Depth32F;
        tex_desc.min_filter = rhi::TextureFilter::Nearest;
        tex_desc.mag_filter = rhi::TextureFilter::Nearest;
        tex_desc.wrap_s = rhi::TextureWrap::ClampToEdge;
        tex_desc.wrap_t = rhi::TextureWrap::ClampToEdge;
        depth_textures_[i] = rhi_->create_texture(tex_desc);

        rhi::FramebufferDesc fb_desc;
        fb_desc.width = config_.resolution;
        fb_desc.height = config_.resolution;
        fb_desc.has_depth = true;
        framebuffers_[i] = rhi_->create_framebuffer(fb_desc);
    }

    // Create depth-only shader and pipeline
    depth_shader_ = rhi_->create_shader(shadow_shaders::DEPTH_VERTEX,
                                         shadow_shaders::DEPTH_FRAGMENT);
    {
        rhi::PipelineDesc desc;
        desc.shader = depth_shader_;
        desc.blend = rhi::BlendMode::None;
        desc.depth_test = true;
        desc.depth_write = true;
        desc.cull = rhi::CullMode::Front;  // Front-face culling reduces shadow acne
        desc.primitive = rhi::PrimitiveType::Triangles;
        desc.vertex_layout.stride = sizeof(float) * 8; // pos(3) + normal(3) + uv(2)
        desc.vertex_layout.attributes = {
            {0, 3, 0, false},  // only position needed
        };
        depth_pipeline_ = rhi_->create_pipeline(desc);
    }

    NX_INFO("CascadedShadowMap initialized: {} cascades, {}x{}",
            config_.num_cascades, config_.resolution, config_.resolution);
}

void CascadedShadowMap::shutdown() {
    if (!rhi_) return;
    for (u32 i = 0; i < config_.num_cascades; ++i) {
        rhi_->destroy_framebuffer(framebuffers_[i]);
        rhi_->destroy_texture(depth_textures_[i]);
    }
    if (depth_pipeline_ != rhi::INVALID_HANDLE) rhi_->destroy_pipeline(depth_pipeline_);
    if (depth_shader_ != rhi::INVALID_HANDLE) rhi_->destroy_shader(depth_shader_);
    depth_pipeline_ = rhi::INVALID_HANDLE;
    depth_shader_ = rhi::INVALID_HANDLE;
    rhi_ = nullptr;
}

void CascadedShadowMap::begin_pass(u32 cascade) {
    if (cascade >= config_.num_cascades || !rhi_) return;
    current_cascade_ = cascade;
    rhi_->bind_framebuffer(framebuffers_[cascade]);
    rhi_->set_viewport(0, 0, static_cast<i32>(config_.resolution),
                       static_cast<i32>(config_.resolution));
    rhi_->clear(Vec4(1.0f), 1.0f);
    rhi_->bind_shader(depth_shader_);
    rhi_->bind_pipeline(depth_pipeline_);
    rhi_->set_uniform_mat4(depth_shader_, "u_LightViewProjection",
                            cascades_[cascade].light_view_projection);
}

void CascadedShadowMap::submit_geometry(rhi::BufferHandle vbo, rhi::BufferHandle ibo,
                                          u32 index_count, const Mat4& model) {
    if (!rhi_ || depth_shader_ == rhi::INVALID_HANDLE) return;
    rhi_->set_uniform_mat4(depth_shader_, "u_Model", model);
    rhi_->bind_vertex_buffer(vbo);
    rhi_->bind_index_buffer(ibo);
    rhi_->draw_indexed(index_count);
}

void CascadedShadowMap::end_pass() {
    if (!rhi_) return;
    rhi_->unbind_framebuffer();
}

void CascadedShadowMap::compute_cascade_splits(float near, float far) {
    float lambda = config_.cascade_split_lambda;
    near = std::max(near, 0.001f); // avoid div-by-zero / inf splits on a 0 near clip
    float range = far - near;
    float ratio = far / near;

    splits_[0] = near;
    for (u32 i = 1; i <= config_.num_cascades; ++i) {
        float p = static_cast<float>(i) / static_cast<float>(config_.num_cascades);
        float log_split = near * std::pow(ratio, p);
        float uniform_split = near + range * p;
        splits_[i] = math::lerp(uniform_split, log_split, lambda);
    }
    splits_[config_.num_cascades] = far;
}

static std::vector<Vec4> get_frustum_corners(const Mat4& view_proj) {
    Mat4 inv = glm::inverse(view_proj);
    std::vector<Vec4> corners;
    for (int x = 0; x < 2; ++x) {
        for (int y = 0; y < 2; ++y) {
            for (int z = 0; z < 2; ++z) {
                Vec4 pt = inv * Vec4(
                    2.0f * static_cast<float>(x) - 1.0f,
                    2.0f * static_cast<float>(y) - 1.0f,
                    2.0f * static_cast<float>(z) - 1.0f,
                    1.0f);
                corners.push_back(pt / pt.w);
            }
        }
    }
    return corners;
}

Mat4 CascadedShadowMap::compute_light_matrix(Vec3 light_dir,
                                              const std::vector<Vec4>& frustum_corners) const {
    Vec3 center(0.0f);
    for (const auto& c : frustum_corners) {
        center += Vec3(c);
    }
    center /= static_cast<float>(frustum_corners.size());

    Vec3 dir_norm = glm::normalize(light_dir);
    // Choose an up vector that isn't parallel to the light direction
    Vec3 up = Vec3(0, 1, 0);
    if (std::abs(glm::dot(dir_norm, up)) > 0.99f) {
        up = Vec3(0, 0, 1);
    }
    Mat4 light_view = glm::lookAt(center - dir_norm, center, up);

    float min_x = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float min_y = std::numeric_limits<float>::max();
    float max_y = std::numeric_limits<float>::lowest();
    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();

    for (const auto& c : frustum_corners) {
        Vec4 lc = light_view * c;
        min_x = std::min(min_x, lc.x);
        max_x = std::max(max_x, lc.x);
        min_y = std::min(min_y, lc.y);
        max_y = std::max(max_y, lc.y);
        min_z = std::min(min_z, lc.z);
        max_z = std::max(max_z, lc.z);
    }

    // Extend Z range to capture shadow casters behind the frustum
    float z_mult = 10.0f;
    if (min_z < 0) min_z *= z_mult;
    else min_z /= z_mult;
    if (max_z < 0) max_z /= z_mult;
    else max_z *= z_mult;

    Mat4 light_proj = glm::ortho(min_x, max_x, min_y, max_y, min_z, max_z);
    return light_proj * light_view;
}

void CascadedShadowMap::update(const Camera3D& camera, Vec3 light_direction) {
    float near = camera.near_clip;
    float far = std::min(camera.far_clip, config_.shadow_distance);
    compute_cascade_splits(near, far);

    // Get actual camera aspect ratio for correct frustum corners
    Mat4 cam_proj = camera.get_projection_matrix();
    // Extract aspect from projection matrix: P[1][1] / P[0][0]
    float aspect = (cam_proj[0][0] != 0.0f) ? cam_proj[1][1] / cam_proj[0][0] : 16.0f / 9.0f;

    for (u32 i = 0; i < config_.num_cascades; ++i) {
        // Build sub-frustum projection for this cascade with correct aspect
        Mat4 proj = glm::perspective(
            glm::radians(camera.fov),
            aspect,
            splits_[i], splits_[i + 1]);
        Mat4 view = camera.get_view_matrix();

        auto corners = get_frustum_corners(proj * view);
        cascades_[i].light_view_projection = compute_light_matrix(light_direction, corners);
        cascades_[i].split_depth = splits_[i + 1];
    }
}

rhi::FramebufferHandle CascadedShadowMap::framebuffer(u32 cascade) const {
    return (cascade < config_.num_cascades) ? framebuffers_[cascade] : rhi::INVALID_HANDLE;
}

rhi::TextureHandle CascadedShadowMap::depth_texture(u32 cascade) const {
    return (cascade < config_.num_cascades) ? depth_textures_[cascade] : rhi::INVALID_HANDLE;
}

// ── PointLightShadow ────────────────────────────────────────────────────────

void PointLightShadow::init(rhi::RHI* rhi, const Config& config) {
    rhi_ = rhi;
    config_ = config;

    rhi::TextureDesc tex_desc;
    tex_desc.width = config_.resolution;
    tex_desc.height = config_.resolution;
    tex_desc.format = rhi::TextureFormat::Depth32F;
    tex_desc.min_filter = rhi::TextureFilter::Nearest;
    tex_desc.mag_filter = rhi::TextureFilter::Nearest;
    tex_desc.wrap_s = rhi::TextureWrap::ClampToEdge;
    tex_desc.wrap_t = rhi::TextureWrap::ClampToEdge;
    depth_texture_ = rhi_->create_texture(tex_desc);

    rhi::FramebufferDesc fb_desc;
    fb_desc.width = config_.resolution;
    fb_desc.height = config_.resolution;
    fb_desc.has_depth = true;
    framebuffer_ = rhi_->create_framebuffer(fb_desc);

    // Create point light depth shader and pipeline
    depth_shader_ = rhi_->create_shader(shadow_shaders::POINT_DEPTH_VERTEX,
                                         shadow_shaders::POINT_DEPTH_FRAGMENT);
    {
        rhi::PipelineDesc desc;
        desc.shader = depth_shader_;
        desc.blend = rhi::BlendMode::None;
        desc.depth_test = true;
        desc.depth_write = true;
        desc.cull = rhi::CullMode::Front;
        desc.primitive = rhi::PrimitiveType::Triangles;
        desc.vertex_layout.stride = sizeof(float) * 8;
        desc.vertex_layout.attributes = {
            {0, 3, 0, false},
        };
        depth_pipeline_ = rhi_->create_pipeline(desc);
    }
}

void PointLightShadow::shutdown() {
    if (!rhi_) return;
    rhi_->destroy_framebuffer(framebuffer_);
    rhi_->destroy_texture(depth_texture_);
    if (depth_pipeline_ != rhi::INVALID_HANDLE) rhi_->destroy_pipeline(depth_pipeline_);
    if (depth_shader_ != rhi::INVALID_HANDLE) rhi_->destroy_shader(depth_shader_);
    depth_pipeline_ = rhi::INVALID_HANDLE;
    depth_shader_ = rhi::INVALID_HANDLE;
    rhi_ = nullptr;
}

void PointLightShadow::begin_face(u32 face, Vec3 light_pos) {
    if (face >= 6 || !rhi_) return;
    rhi_->bind_framebuffer(framebuffer_);
    rhi_->set_viewport(0, 0, static_cast<i32>(config_.resolution),
                       static_cast<i32>(config_.resolution));
    rhi_->clear(Vec4(1.0f), 1.0f);
    rhi_->bind_shader(depth_shader_);
    rhi_->bind_pipeline(depth_pipeline_);
    rhi_->set_uniform_mat4(depth_shader_, "u_LightViewProjection", face_matrices_[face]);
    rhi_->set_uniform_vec3(depth_shader_, "u_LightPos", light_pos);
    rhi_->set_uniform_float(depth_shader_, "u_FarPlane", config_.far_plane);
}

void PointLightShadow::submit_geometry(rhi::BufferHandle vbo, rhi::BufferHandle ibo,
                                         u32 index_count, const Mat4& model) {
    if (!rhi_ || depth_shader_ == rhi::INVALID_HANDLE) return;
    rhi_->set_uniform_mat4(depth_shader_, "u_Model", model);
    rhi_->bind_vertex_buffer(vbo);
    rhi_->bind_index_buffer(ibo);
    rhi_->draw_indexed(index_count);
}

void PointLightShadow::end_face() {
    if (!rhi_) return;
    rhi_->unbind_framebuffer();
}

void PointLightShadow::update(Vec3 pos) {
    float aspect = 1.0f;
    Mat4 proj = glm::perspective(glm::radians(90.0f), aspect,
                                  config_.near_plane, config_.far_plane);

    // +X, -X, +Y, -Y, +Z, -Z
    face_matrices_[0] = proj * glm::lookAt(pos, pos + Vec3(1, 0, 0), Vec3(0, -1, 0));
    face_matrices_[1] = proj * glm::lookAt(pos, pos + Vec3(-1, 0, 0), Vec3(0, -1, 0));
    face_matrices_[2] = proj * glm::lookAt(pos, pos + Vec3(0, 1, 0), Vec3(0, 0, 1));
    face_matrices_[3] = proj * glm::lookAt(pos, pos + Vec3(0, -1, 0), Vec3(0, 0, -1));
    face_matrices_[4] = proj * glm::lookAt(pos, pos + Vec3(0, 0, 1), Vec3(0, -1, 0));
    face_matrices_[5] = proj * glm::lookAt(pos, pos + Vec3(0, 0, -1), Vec3(0, -1, 0));
}

} // namespace nexus
