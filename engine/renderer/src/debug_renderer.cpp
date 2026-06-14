#include <nexus/renderer/debug_renderer.h>
#include <nexus/core/log.h>
#include <algorithm>
#include <cmath>

namespace nexus {

static const char* DEBUG_VERTEX_SHADER = R"(
#version 330 core
layout (location = 0) in vec3 a_Position;
layout (location = 1) in vec4 a_Color;

out vec4 v_Color;

uniform mat4 u_ViewProjection;

void main() {
    v_Color = a_Color;
    gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}
)";

static const char* DEBUG_FRAGMENT_SHADER = R"(
#version 330 core
in vec4 v_Color;
out vec4 FragColor;

void main() {
    FragColor = v_Color;
}
)";

void DebugRenderer::init(rhi::RHI* rhi) {
    rhi_ = rhi;
    vertices_.reserve(MAX_VERTICES);

    shader_ = rhi_->create_shader(DEBUG_VERTEX_SHADER, DEBUG_FRAGMENT_SHADER);
    if (shader_ == rhi::INVALID_HANDLE) {
        NX_ERROR("DebugRenderer: Failed to compile debug shader");
        return;
    }

    // Dynamic line buffer
    rhi::BufferDesc vbo_desc;
    vbo_desc.type  = rhi::BufferType::Vertex;
    vbo_desc.usage = rhi::BufferUsage::Dynamic;
    vbo_desc.size  = sizeof(LineVertex) * MAX_VERTICES;
    vbo_desc.data  = nullptr;
    vbo_ = rhi_->create_buffer(vbo_desc);

    // Pipeline for lines
    rhi::PipelineDesc pipe_desc;
    pipe_desc.shader     = shader_;
    pipe_desc.blend      = rhi::BlendMode::Alpha;
    pipe_desc.depth_test = true;
    pipe_desc.depth_write = false;
    pipe_desc.cull       = rhi::CullMode::None;
    pipe_desc.primitive  = rhi::PrimitiveType::Lines;

    pipe_desc.vertex_layout.stride = sizeof(LineVertex);
    pipe_desc.vertex_layout.attributes = {
        {0, 3, offsetof(LineVertex, position), false},
        {1, 4, offsetof(LineVertex, color),    false},
    };
    pipeline_ = rhi_->create_pipeline(pipe_desc);

    NX_INFO("DebugRenderer initialized");
}

void DebugRenderer::shutdown() {
    if (!rhi_) return;
    if (pipeline_ != rhi::INVALID_HANDLE) rhi_->destroy_pipeline(pipeline_);
    if (vbo_ != rhi::INVALID_HANDLE)      rhi_->destroy_buffer(vbo_);
    if (shader_ != rhi::INVALID_HANDLE)   rhi_->destroy_shader(shader_);
    pipeline_ = rhi::INVALID_HANDLE;
    vbo_      = rhi::INVALID_HANDLE;
    shader_   = rhi::INVALID_HANDLE;
    rhi_ = nullptr;
}

void DebugRenderer::begin(const Camera3D& camera) {
    view_projection_ = camera.get_view_projection();
    vertices_.clear();
}

void DebugRenderer::end() {
    flush();
}

void DebugRenderer::flush() {
    if (vertices_.empty()) return;

    rhi_->update_buffer(vbo_, vertices_.data(),
                        vertices_.size() * sizeof(LineVertex));

    rhi_->bind_shader(shader_);
    rhi_->set_uniform_mat4(shader_, "u_ViewProjection", view_projection_);
    rhi_->bind_pipeline(pipeline_);
    rhi_->bind_vertex_buffer(vbo_);
    rhi_->draw(static_cast<u32>(vertices_.size()));
}

void DebugRenderer::add_line(Vec3 start, Vec3 end, Vec4 color) {
    if (vertices_.size() + 2 > MAX_VERTICES) {
        flush();
        vertices_.clear();
    }
    vertices_.push_back({start, color});
    vertices_.push_back({end, color});
}

void DebugRenderer::draw_line(Vec3 start, Vec3 end, Vec4 color) {
    add_line(start, end, color);
}

void DebugRenderer::draw_box(Vec3 center, Vec3 extents, Vec4 color) {
    Vec3 min = center - extents;
    Vec3 max = center + extents;

    // Bottom face
    add_line({min.x, min.y, min.z}, {max.x, min.y, min.z}, color);
    add_line({max.x, min.y, min.z}, {max.x, min.y, max.z}, color);
    add_line({max.x, min.y, max.z}, {min.x, min.y, max.z}, color);
    add_line({min.x, min.y, max.z}, {min.x, min.y, min.z}, color);

    // Top face
    add_line({min.x, max.y, min.z}, {max.x, max.y, min.z}, color);
    add_line({max.x, max.y, min.z}, {max.x, max.y, max.z}, color);
    add_line({max.x, max.y, max.z}, {min.x, max.y, max.z}, color);
    add_line({min.x, max.y, max.z}, {min.x, max.y, min.z}, color);

    // Vertical edges
    add_line({min.x, min.y, min.z}, {min.x, max.y, min.z}, color);
    add_line({max.x, min.y, min.z}, {max.x, max.y, min.z}, color);
    add_line({max.x, min.y, max.z}, {max.x, max.y, max.z}, color);
    add_line({min.x, min.y, max.z}, {min.x, max.y, max.z}, color);
}

void DebugRenderer::draw_aabb(const AABB& aabb, Vec4 color) {
    draw_box(aabb.center(), aabb.extents(), color);
}

void DebugRenderer::draw_sphere(Vec3 center, float radius, Vec4 color, u32 segments) {
    if (segments == 0) return; // avoid div-by-zero / degenerate sphere
    float step = math::TWO_PI / static_cast<float>(segments);

    // Three perpendicular circles
    for (u32 i = 0; i < segments; ++i) {
        float a0 = step * static_cast<float>(i);
        float a1 = step * static_cast<float>(i + 1);
        float c0 = std::cos(a0), s0 = std::sin(a0);
        float c1 = std::cos(a1), s1 = std::sin(a1);

        // XY circle
        add_line(center + Vec3(c0, s0, 0.0f) * radius,
                 center + Vec3(c1, s1, 0.0f) * radius, color);
        // XZ circle
        add_line(center + Vec3(c0, 0.0f, s0) * radius,
                 center + Vec3(c1, 0.0f, s1) * radius, color);
        // YZ circle
        add_line(center + Vec3(0.0f, c0, s0) * radius,
                 center + Vec3(0.0f, c1, s1) * radius, color);
    }
}

void DebugRenderer::draw_grid(float size, float step, Vec4 color) {
    // Guard against a zero/negative step (inf or billion-iteration loop, casting
    // inf to int is UB) and clamp the line count to a sane maximum.
    if (step <= 0.0f || size <= 0.0f) return;
    float half = size * 0.5f;
    int count = static_cast<int>(size / step);
    count = std::min(count, 4096);

    for (int i = 0; i <= count; ++i) {
        float pos = -half + static_cast<float>(i) * step;
        // Lines along Z
        add_line({pos, 0.0f, -half}, {pos, 0.0f, half}, color);
        // Lines along X
        add_line({-half, 0.0f, pos}, {half, 0.0f, pos}, color);
    }
}

void DebugRenderer::draw_axis(Vec3 origin, float length) {
    add_line(origin, origin + Vec3(length, 0.0f, 0.0f), {1.0f, 0.0f, 0.0f, 1.0f}); // X = red
    add_line(origin, origin + Vec3(0.0f, length, 0.0f), {0.0f, 1.0f, 0.0f, 1.0f}); // Y = green
    add_line(origin, origin + Vec3(0.0f, 0.0f, length), {0.0f, 0.0f, 1.0f, 1.0f}); // Z = blue
}

void DebugRenderer::draw_frustum(const Mat4& view_projection, Vec4 color) {
    Mat4 inv = glm::inverse(view_projection);

    // NDC corners
    Vec4 ndc_corners[8] = {
        {-1, -1, -1, 1}, { 1, -1, -1, 1}, { 1,  1, -1, 1}, {-1,  1, -1, 1}, // near
        {-1, -1,  1, 1}, { 1, -1,  1, 1}, { 1,  1,  1, 1}, {-1,  1,  1, 1}, // far
    };

    Vec3 corners[8];
    for (int i = 0; i < 8; ++i) {
        Vec4 world = inv * ndc_corners[i];
        corners[i] = Vec3(world) / world.w;
    }

    // Near face
    for (int i = 0; i < 4; ++i) add_line(corners[i], corners[(i+1)%4], color);
    // Far face
    for (int i = 0; i < 4; ++i) add_line(corners[4+i], corners[4+(i+1)%4], color);
    // Edges connecting near to far
    for (int i = 0; i < 4; ++i) add_line(corners[i], corners[i+4], color);
}

void DebugRenderer::draw_ray(Vec3 origin, Vec3 direction, float length, Vec4 color) {
    add_line(origin, origin + glm::normalize(direction) * length, color);
}

} // namespace nexus
