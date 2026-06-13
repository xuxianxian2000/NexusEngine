#include "nexus/renderer/decal_system.h"
#include "nexus/core/log.h"
#include <algorithm>

namespace nexus {

// ── Decal shaders ──────────────────────────────────────────────────────────

static const char* DECAL_VERTEX = R"(
#version 330 core
layout(location = 0) in vec3 a_Position;

uniform mat4 u_ViewProjection;
uniform mat4 u_Model;

out vec4 v_ClipPos;

void main() {
    gl_Position = u_ViewProjection * u_Model * vec4(a_Position, 1.0);
    v_ClipPos = gl_Position;
}
)";

static const char* DECAL_FRAGMENT = R"(
#version 330 core
in vec4 v_ClipPos;
out vec4 FragColor;

uniform sampler2D u_DepthTex;
uniform sampler2D u_AlbedoTex;
uniform sampler2D u_NormalTex;
uniform bool      u_HasNormalTex;

uniform mat4  u_InvViewProjection;
uniform mat4  u_InvDecalTransform;
uniform vec4  u_DecalColor;
uniform float u_NormalStrength;
uniform float u_FadeDistance;
uniform vec2  u_ScreenSize;

void main() {
    // Screen-space UV from clip position
    vec2 screenUV = gl_FragCoord.xy / u_ScreenSize;

    // Reconstruct world position from depth
    float depth = texture(u_DepthTex, screenUV).r;
    vec4 clipPos = vec4(screenUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 worldPos4 = u_InvViewProjection * clipPos;
    vec3 worldPos = worldPos4.xyz / worldPos4.w;

    // Project world position into decal space [0,1]^3
    vec4 decalPos = u_InvDecalTransform * vec4(worldPos, 1.0);
    vec3 dp = decalPos.xyz;

    // Discard if outside decal box
    if (abs(dp.x) > 0.5 || abs(dp.y) > 0.5 || abs(dp.z) > 0.5)
        discard;

    // UV from decal-space XZ
    vec2 decalUV = dp.xz + 0.5;

    // Sample decal albedo
    vec4 albedo = texture(u_AlbedoTex, decalUV) * u_DecalColor;

    // Edge fade
    vec3 edgeDist = vec3(0.5) - abs(dp);
    float fade = smoothstep(0.0, u_FadeDistance, min(edgeDist.x, min(edgeDist.y, edgeDist.z)));
    albedo.a *= fade;

    if (albedo.a < 0.01) discard;

    FragColor = albedo;
}
)";

// ── Unit cube geometry ─────────────────────────────────────────────────────

static const float CUBE_VERTICES[] = {
    // 8 corners of a unit cube centered at origin
    -0.5f, -0.5f, -0.5f,
     0.5f, -0.5f, -0.5f,
     0.5f,  0.5f, -0.5f,
    -0.5f,  0.5f, -0.5f,
    -0.5f, -0.5f,  0.5f,
     0.5f, -0.5f,  0.5f,
     0.5f,  0.5f,  0.5f,
    -0.5f,  0.5f,  0.5f,
};

static const u32 CUBE_INDICES[] = {
    // 12 triangles (6 faces)
    0, 1, 2,  2, 3, 0,  // back
    4, 6, 5,  6, 4, 7,  // front
    0, 4, 5,  5, 1, 0,  // bottom
    2, 6, 7,  7, 3, 2,  // top
    0, 3, 7,  7, 4, 0,  // left
    1, 5, 6,  6, 2, 1,  // right
};

// ── DecalSystem implementation ─────────────────────────────────────────────

void DecalSystem::init(rhi::RHI* rhi) {
    rhi_ = rhi;

    // Create unit cube buffers
    {
        rhi::BufferDesc desc;
        desc.size = sizeof(CUBE_VERTICES);
        desc.usage = rhi::BufferUsage::Static;
        desc.data = CUBE_VERTICES;
        cube_vbo_ = rhi_->create_buffer(desc);
    }
    {
        rhi::BufferDesc desc;
        desc.size = sizeof(CUBE_INDICES);
        desc.usage = rhi::BufferUsage::Static;
        desc.data = CUBE_INDICES;
        cube_ibo_ = rhi_->create_buffer(desc);
    }
    cube_index_count_ = sizeof(CUBE_INDICES) / sizeof(u32);

    // Create shader and pipeline
    shader_ = rhi_->create_shader(DECAL_VERTEX, DECAL_FRAGMENT);

    rhi::PipelineDesc pipe_desc;
    pipe_desc.shader = shader_;
    pipe_desc.blend = rhi::BlendMode::Alpha;
    pipe_desc.depth_test = true;
    pipe_desc.depth_write = false;
    pipe_desc.cull = rhi::CullMode::Front;  // render back faces of decal box
    pipe_desc.primitive = rhi::PrimitiveType::Triangles;
    pipe_desc.vertex_layout.stride = sizeof(float) * 3;
    pipe_desc.vertex_layout.attributes = {
        {0, 3, 0, false},
    };
    pipeline_ = rhi_->create_pipeline(pipe_desc);

    NX_INFO("DecalSystem initialized");
}

void DecalSystem::shutdown() {
    if (!rhi_) return;
    if (cube_vbo_ != rhi::INVALID_HANDLE) rhi_->destroy_buffer(cube_vbo_);
    if (cube_ibo_ != rhi::INVALID_HANDLE) rhi_->destroy_buffer(cube_ibo_);
    if (pipeline_ != rhi::INVALID_HANDLE) rhi_->destroy_pipeline(pipeline_);
    if (shader_ != rhi::INVALID_HANDLE) rhi_->destroy_shader(shader_);
    cube_vbo_ = cube_ibo_ = rhi::INVALID_HANDLE;
    pipeline_ = shader_ = rhi::INVALID_HANDLE;
    decals_.clear();
    rhi_ = nullptr;
}

u32 DecalSystem::add_decal(const Decal& decal) {
    decals_.push_back(decal);
    return static_cast<u32>(decals_.size() - 1);
}

void DecalSystem::remove_decal(u32 index) {
    if (index < decals_.size()) {
        decals_.erase(decals_.begin() + index);
    }
}

void DecalSystem::clear() {
    decals_.clear();
}

void DecalSystem::render(rhi::TextureHandle depth_tex, rhi::TextureHandle /*normal_tex*/,
                          const Mat4& view, const Mat4& projection,
                          const Mat4& inv_view_projection) {
    if (!rhi_ || decals_.empty()) return;

    Mat4 vp = projection * view;

    rhi_->bind_shader(shader_);
    rhi_->bind_pipeline(pipeline_);
    rhi_->set_uniform_mat4(shader_, "u_ViewProjection", vp);
    rhi_->set_uniform_mat4(shader_, "u_InvViewProjection", inv_view_projection);

    // Bind depth texture
    rhi_->bind_texture(depth_tex, 0);
    rhi_->set_uniform_int(shader_, "u_DepthTex", 0);

    // Set screen size (would come from framebuffer, using defaults)
    rhi_->set_uniform_vec2(shader_, "u_ScreenSize", Vec2(1920.0f, 1080.0f));

    rhi_->bind_vertex_buffer(cube_vbo_);
    rhi_->bind_index_buffer(cube_ibo_);

    // Sort by layer for correct ordering
    std::vector<u32> sorted_indices;
    sorted_indices.reserve(decals_.size());
    for (u32 i = 0; i < decals_.size(); ++i) {
        if (decals_[i].active) sorted_indices.push_back(i);
    }
    std::sort(sorted_indices.begin(), sorted_indices.end(),
              [this](u32 a, u32 b) { return decals_[a].layer < decals_[b].layer; });

    for (u32 idx : sorted_indices) {
        const auto& d = decals_[idx];

        rhi_->set_uniform_mat4(shader_, "u_Model", d.transform);
        rhi_->set_uniform_mat4(shader_, "u_InvDecalTransform", d.inv_transform);
        rhi_->set_uniform_vec4(shader_, "u_DecalColor", d.color);
        rhi_->set_uniform_float(shader_, "u_NormalStrength", d.normal_strength);
        rhi_->set_uniform_float(shader_, "u_FadeDistance", d.fade_distance);

        // Bind albedo texture
        if (d.albedo_tex != rhi::INVALID_HANDLE) {
            rhi_->bind_texture(d.albedo_tex, 1);
            rhi_->set_uniform_int(shader_, "u_AlbedoTex", 1);
        }

        // Bind normal texture
        bool has_normal = (d.normal_tex != rhi::INVALID_HANDLE);
        rhi_->set_uniform_int(shader_, "u_HasNormalTex", has_normal ? 1 : 0);
        if (has_normal) {
            rhi_->bind_texture(d.normal_tex, 2);
            rhi_->set_uniform_int(shader_, "u_NormalTex", 2);
        }

        rhi_->draw_indexed(cube_index_count_);
    }
}

} // namespace nexus
