// ============================================================================
// batch_renderer_2d.cpp - Auto-batching 2D sprite/shape renderer
// ============================================================================

#include <nexus/renderer/batch_renderer_2d.h>
#include <nexus/core/log.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace nexus {

// ── Default 2D shaders ──────────────────────────────────────────────────────

static const char* BATCH_VERTEX_SHADER = R"(
#version 330 core
layout (location = 0) in vec3 a_Position;
layout (location = 1) in vec4 a_Color;
layout (location = 2) in vec2 a_TexCoord;
layout (location = 3) in float a_TexIndex;

out vec4 v_Color;
out vec2 v_TexCoord;
out float v_TexIndex;

uniform mat4 u_ViewProjection;

void main() {
    v_Color = a_Color;
    v_TexCoord = a_TexCoord;
    v_TexIndex = a_TexIndex;
    gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}
)";

static const char* BATCH_FRAGMENT_SHADER = R"(
#version 330 core
in vec4 v_Color;
in vec2 v_TexCoord;
in float v_TexIndex;

out vec4 FragColor;

uniform sampler2D u_Textures[16];

void main() {
    int index = int(v_TexIndex);
    vec4 texColor = vec4(1.0);

    // Manual switch since older GLSL doesn't allow variable indexing of samplers
    switch (index) {
        case  0: texColor = texture(u_Textures[ 0], v_TexCoord); break;
        case  1: texColor = texture(u_Textures[ 1], v_TexCoord); break;
        case  2: texColor = texture(u_Textures[ 2], v_TexCoord); break;
        case  3: texColor = texture(u_Textures[ 3], v_TexCoord); break;
        case  4: texColor = texture(u_Textures[ 4], v_TexCoord); break;
        case  5: texColor = texture(u_Textures[ 5], v_TexCoord); break;
        case  6: texColor = texture(u_Textures[ 6], v_TexCoord); break;
        case  7: texColor = texture(u_Textures[ 7], v_TexCoord); break;
        case  8: texColor = texture(u_Textures[ 8], v_TexCoord); break;
        case  9: texColor = texture(u_Textures[ 9], v_TexCoord); break;
        case 10: texColor = texture(u_Textures[10], v_TexCoord); break;
        case 11: texColor = texture(u_Textures[11], v_TexCoord); break;
        case 12: texColor = texture(u_Textures[12], v_TexCoord); break;
        case 13: texColor = texture(u_Textures[13], v_TexCoord); break;
        case 14: texColor = texture(u_Textures[14], v_TexCoord); break;
        case 15: texColor = texture(u_Textures[15], v_TexCoord); break;
    }

    FragColor = texColor * v_Color;
}
)";

// ── Lifecycle ───────────────────────────────────────────────────────────────

BatchRenderer2D::BatchRenderer2D(BatchRenderer2D&& other) noexcept
    : rhi_(other.rhi_), shader_(other.shader_), pipeline_(other.pipeline_),
      vbo_(other.vbo_), ibo_(other.ibo_), white_texture_(other.white_texture_),
      vertices_(other.vertices_), vertex_count_(other.vertex_count_),
      texture_slots_(other.texture_slots_), texture_slot_index_(other.texture_slot_index_),
      stats_(other.stats_) {
    other.rhi_ = nullptr;
    other.shader_ = rhi::INVALID_HANDLE;
    other.pipeline_ = rhi::INVALID_HANDLE;
    other.vbo_ = rhi::INVALID_HANDLE;
    other.ibo_ = rhi::INVALID_HANDLE;
    other.white_texture_ = rhi::INVALID_HANDLE;
}

BatchRenderer2D& BatchRenderer2D::operator=(BatchRenderer2D&& other) noexcept {
    if (this != &other) {
        shutdown();
        rhi_ = other.rhi_; shader_ = other.shader_; pipeline_ = other.pipeline_;
        vbo_ = other.vbo_; ibo_ = other.ibo_; white_texture_ = other.white_texture_;
        vertices_ = other.vertices_; vertex_count_ = other.vertex_count_;
        texture_slots_ = other.texture_slots_; texture_slot_index_ = other.texture_slot_index_;
        stats_ = other.stats_;
        other.rhi_ = nullptr; other.shader_ = rhi::INVALID_HANDLE;
        other.pipeline_ = rhi::INVALID_HANDLE; other.vbo_ = rhi::INVALID_HANDLE;
        other.ibo_ = rhi::INVALID_HANDLE; other.white_texture_ = rhi::INVALID_HANDLE;
    }
    return *this;
}

void BatchRenderer2D::init(rhi::RHI* rhi) {
    rhi_ = rhi;

    // Compile batch shader
    shader_ = rhi_->create_shader(BATCH_VERTEX_SHADER, BATCH_FRAGMENT_SHADER);
    if (shader_ == rhi::INVALID_HANDLE) {
        NX_ERROR("BatchRenderer2D: Failed to compile batch shader");
        return;
    }

    // Set up texture sampler uniforms
    rhi_->bind_shader(shader_);
    i32 samplers[MAX_TEXTURE_SLOTS];
    for (u32 i = 0; i < MAX_TEXTURE_SLOTS; ++i)
        samplers[i] = static_cast<i32>(i);
    rhi_->set_uniform_int_array(shader_, "u_Textures", samplers, MAX_TEXTURE_SLOTS);

    // Create vertex buffer (dynamic)
    rhi::BufferDesc vbo_desc;
    vbo_desc.type  = rhi::BufferType::Vertex;
    vbo_desc.usage = rhi::BufferUsage::Dynamic;
    vbo_desc.size  = sizeof(Vertex) * MAX_VERTICES;
    vbo_desc.data  = nullptr;
    vbo_ = rhi_->create_buffer(vbo_desc);

    // Generate index buffer data (shared pattern for all quads)
    std::vector<u32> indices(MAX_INDICES);
    u32 offset = 0;
    for (u32 i = 0; i < MAX_INDICES; i += 6) {
        indices[i + 0] = offset + 0;
        indices[i + 1] = offset + 1;
        indices[i + 2] = offset + 2;
        indices[i + 3] = offset + 2;
        indices[i + 4] = offset + 3;
        indices[i + 5] = offset + 0;
        offset += 4;
    }

    rhi::BufferDesc ibo_desc;
    ibo_desc.type  = rhi::BufferType::Index;
    ibo_desc.usage = rhi::BufferUsage::Static;
    ibo_desc.size  = indices.size() * sizeof(u32);
    ibo_desc.data  = indices.data();
    ibo_ = rhi_->create_buffer(ibo_desc);

    // Create pipeline with vertex layout
    rhi::PipelineDesc pipe_desc;
    pipe_desc.shader     = shader_;
    pipe_desc.blend      = rhi::BlendMode::Alpha;
    pipe_desc.depth_test = false;
    pipe_desc.cull       = rhi::CullMode::None;
    pipe_desc.primitive  = rhi::PrimitiveType::Triangles;

    pipe_desc.vertex_layout.stride = sizeof(Vertex);
    pipe_desc.vertex_layout.attributes = {
        {0, 3, offsetof(Vertex, position),  false},  // position
        {1, 4, offsetof(Vertex, color),     false},  // color
        {2, 2, offsetof(Vertex, texcoord),  false},  // texcoord
        {3, 1, offsetof(Vertex, tex_index), false},  // tex_index
    };
    pipeline_ = rhi_->create_pipeline(pipe_desc);

    // Create 1x1 white texture for solid-color quads
    u32 white_pixel = 0xFFFFFFFF;
    rhi::TextureDesc white_desc;
    white_desc.width  = 1;
    white_desc.height = 1;
    white_desc.format = rhi::TextureFormat::RGBA8;
    white_desc.min_filter = rhi::TextureFilter::Nearest;
    white_desc.mag_filter = rhi::TextureFilter::Nearest;
    white_desc.generate_mipmaps = false;
    white_desc.data = &white_pixel;
    white_texture_ = rhi_->create_texture(white_desc);

    NX_INFO("BatchRenderer2D initialized (max {} quads)", MAX_QUADS);
}

void BatchRenderer2D::shutdown() {
    if (!rhi_) return;
    if (pipeline_ != rhi::INVALID_HANDLE) rhi_->destroy_pipeline(pipeline_);
    if (ibo_ != rhi::INVALID_HANDLE) rhi_->destroy_buffer(ibo_);
    if (vbo_ != rhi::INVALID_HANDLE) rhi_->destroy_buffer(vbo_);
    if (shader_ != rhi::INVALID_HANDLE) rhi_->destroy_shader(shader_);
    if (white_texture_ != rhi::INVALID_HANDLE) rhi_->destroy_texture(white_texture_);
    pipeline_ = rhi::INVALID_HANDLE;
    ibo_ = rhi::INVALID_HANDLE;
    vbo_ = rhi::INVALID_HANDLE;
    shader_ = rhi::INVALID_HANDLE;
    white_texture_ = rhi::INVALID_HANDLE;
    rhi_ = nullptr;
}

// ── Frame scope ─────────────────────────────────────────────────────────────

void BatchRenderer2D::begin(const Camera2D& camera) {
    if (!rhi_ || shader_ == rhi::INVALID_HANDLE) return;
    rhi_->bind_shader(shader_);
    rhi_->set_uniform_mat4(shader_, "u_ViewProjection", camera.get_view_projection());

    // Extract camera bounds for 2D frustum culling
    Mat4 inv_vp = glm::inverse(camera.get_view_projection());
    Vec4 corners[4] = {
        inv_vp * Vec4{-1.0f, -1.0f, 0.0f, 1.0f},
        inv_vp * Vec4{ 1.0f, -1.0f, 0.0f, 1.0f},
        inv_vp * Vec4{ 1.0f,  1.0f, 0.0f, 1.0f},
        inv_vp * Vec4{-1.0f,  1.0f, 0.0f, 1.0f},
    };
    camera_min_ = Vec2(corners[0].x / corners[0].w, corners[0].y / corners[0].w);
    camera_max_ = camera_min_;
    for (int i = 1; i < 4; ++i) {
        Vec2 p(corners[i].x / corners[i].w, corners[i].y / corners[i].w);
        camera_min_ = glm::min(camera_min_, p);
        camera_max_ = glm::max(camera_max_, p);
    }
    // Add a small margin for objects partially on-screen
    Vec2 margin = (camera_max_ - camera_min_) * 0.05f;
    camera_min_ -= margin;
    camera_max_ += margin;
    culling_enabled_ = true;

    start_batch();
}

void BatchRenderer2D::end() {
    flush();
}

void BatchRenderer2D::start_batch() {
    vertex_count_ = 0;
    texture_slot_index_ = 1; // slot 0 is always the white texture
    texture_slots_[0] = white_texture_;
}

void BatchRenderer2D::flush() {
    if (vertex_count_ == 0) return;

    // Upload vertex data
    rhi_->update_buffer(vbo_, vertices_.data(), vertex_count_ * sizeof(Vertex));

    // Bind textures
    for (u32 i = 0; i < texture_slot_index_; ++i) {
        rhi_->bind_texture(texture_slots_[i], i);
    }

    // Draw
    rhi_->bind_pipeline(pipeline_);
    rhi_->bind_vertex_buffer(vbo_);
    rhi_->bind_index_buffer(ibo_);

    u32 index_count = (vertex_count_ / 4) * 6;
    rhi_->draw_indexed(index_count);

    stats_.draw_calls++;
}

// ── Texture slot management ─────────────────────────────────────────────────

float BatchRenderer2D::find_or_add_texture(rhi::TextureHandle texture) {
    // Look for existing slot
    for (u32 i = 0; i < texture_slot_index_; ++i) {
        if (texture_slots_[i] == texture) {
            return static_cast<float>(i);
        }
    }

    // Need new slot — flush if full
    if (texture_slot_index_ >= MAX_TEXTURE_SLOTS) {
        flush();
        start_batch();
    }

    texture_slots_[texture_slot_index_] = texture;
    return static_cast<float>(texture_slot_index_++);
}

// ── Quad drawing ────────────────────────────────────────────────────────────

void BatchRenderer2D::draw_quad(Vec2 position, Vec2 size, Vec4 color) {
    draw_quad(position, size, 0.0f, color);
}

void BatchRenderer2D::draw_quad(Vec2 position, Vec2 size,
                                rhi::TextureHandle texture, Vec4 tint) {
    draw_quad(position, size, 0.0f, texture, tint);
}

void BatchRenderer2D::draw_quad(Vec2 position, Vec2 size,
                                float rotation, Vec4 color) {
    if (vertex_count_ + 4 > MAX_VERTICES) {
        flush();
        start_batch();
    }

    float tex_index = 0.0f; // white texture

    Mat4 transform(1.0f);
    transform = glm::translate(transform, Vec3(position, 0.0f));
    if (rotation != 0.0f) {
        transform = glm::rotate(transform, rotation, Vec3(0.0f, 0.0f, 1.0f));
    }
    transform = glm::scale(transform, Vec3(size, 1.0f));

    constexpr Vec2 tex_coords[4] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}
    };

    for (u32 i = 0; i < 4; ++i) {
        Vec4 world_pos = transform * QUAD_POSITIONS[i];
        vertices_[vertex_count_].position  = Vec3(world_pos);
        vertices_[vertex_count_].color     = color;
        vertices_[vertex_count_].texcoord  = tex_coords[i];
        vertices_[vertex_count_].tex_index = tex_index;
        vertex_count_++;
    }

    stats_.quad_count++;
}

void BatchRenderer2D::draw_quad(Vec2 position, Vec2 size, float rotation,
                                rhi::TextureHandle texture,
                                Vec4 tint, Vec2 uv_min, Vec2 uv_max) {
    if (vertex_count_ + 4 > MAX_VERTICES) {
        flush();
        start_batch();
    }

    float tex_index = find_or_add_texture(texture);

    Mat4 transform(1.0f);
    transform = glm::translate(transform, Vec3(position, 0.0f));
    if (rotation != 0.0f) {
        transform = glm::rotate(transform, rotation, Vec3(0.0f, 0.0f, 1.0f));
    }
    transform = glm::scale(transform, Vec3(size, 1.0f));

    Vec2 tex_coords[4] = {
        {uv_min.x, uv_min.y},
        {uv_max.x, uv_min.y},
        {uv_max.x, uv_max.y},
        {uv_min.x, uv_max.y}
    };

    for (u32 i = 0; i < 4; ++i) {
        Vec4 world_pos = transform * QUAD_POSITIONS[i];
        vertices_[vertex_count_].position  = Vec3(world_pos);
        vertices_[vertex_count_].color     = tint;
        vertices_[vertex_count_].texcoord  = tex_coords[i];
        vertices_[vertex_count_].tex_index = tex_index;
        vertex_count_++;
    }

    stats_.quad_count++;
}

// ── Shape drawing ───────────────────────────────────────────────────────────

void BatchRenderer2D::draw_line(Vec2 start, Vec2 end, Vec4 color, float thickness) {
    Vec2 direction = end - start;
    float length = glm::length(direction);
    if (length < 0.001f) return;

    Vec2 normalized = direction / length;
    Vec2 perpendicular(-normalized.y, normalized.x);
    Vec2 offset = perpendicular * (thickness * 0.5f);

    if (vertex_count_ + 4 > MAX_VERTICES) {
        flush();
        start_batch();
    }

    Vec3 corners[4] = {
        Vec3(start - offset, 0.0f),
        Vec3(start + offset, 0.0f),
        Vec3(end + offset, 0.0f),
        Vec3(end - offset, 0.0f)
    };

    for (u32 i = 0; i < 4; ++i) {
        vertices_[vertex_count_].position  = corners[i];
        vertices_[vertex_count_].color     = color;
        vertices_[vertex_count_].texcoord  = Vec2(0.0f);
        vertices_[vertex_count_].tex_index = 0.0f;
        vertex_count_++;
    }

    stats_.quad_count++;
}

void BatchRenderer2D::draw_circle(Vec2 center, float radius, Vec4 color, i32 segments) {
    if (segments <= 0) return; // avoid div-by-zero / degenerate circle
    float angle_step = math::TWO_PI / static_cast<float>(segments);

    for (i32 i = 0; i < segments; ++i) {
        float a0 = angle_step * static_cast<float>(i);
        float a1 = angle_step * static_cast<float>(i + 1);

        Vec2 p0 = center;
        Vec2 p1 = center + Vec2(std::cos(a0), std::sin(a0)) * radius;
        Vec2 p2 = center + Vec2(std::cos(a1), std::sin(a1)) * radius;

        // Draw as a thin quad (triangle fan approximated with quads)
        if (vertex_count_ + 4 > MAX_VERTICES) {
            flush();
            start_batch();
        }

        vertices_[vertex_count_].position  = Vec3(p0, 0.0f);
        vertices_[vertex_count_].color     = color;
        vertices_[vertex_count_].texcoord  = Vec2(0.0f);
        vertices_[vertex_count_].tex_index = 0.0f;
        vertex_count_++;

        vertices_[vertex_count_].position  = Vec3(p1, 0.0f);
        vertices_[vertex_count_].color     = color;
        vertices_[vertex_count_].texcoord  = Vec2(0.0f);
        vertices_[vertex_count_].tex_index = 0.0f;
        vertex_count_++;

        vertices_[vertex_count_].position  = Vec3(p2, 0.0f);
        vertices_[vertex_count_].color     = color;
        vertices_[vertex_count_].texcoord  = Vec2(0.0f);
        vertices_[vertex_count_].tex_index = 0.0f;
        vertex_count_++;

        // Duplicate last vertex to complete quad (degenerate triangle)
        vertices_[vertex_count_].position  = Vec3(p2, 0.0f);
        vertices_[vertex_count_].color     = color;
        vertices_[vertex_count_].texcoord  = Vec2(0.0f);
        vertices_[vertex_count_].tex_index = 0.0f;
        vertex_count_++;

        stats_.quad_count++;
    }
}

void BatchRenderer2D::draw_rect(Vec2 position, Vec2 size, Vec4 color, float thickness) {
    Vec2 tl = position;
    Vec2 tr = position + Vec2(size.x, 0.0f);
    Vec2 br = position + size;
    Vec2 bl = position + Vec2(0.0f, size.y);

    draw_line(tl, tr, color, thickness);
    draw_line(tr, br, color, thickness);
    draw_line(br, bl, color, thickness);
    draw_line(bl, tl, color, thickness);
}

// ── 2D frustum culling ──────────────────────────────────────────────────────

bool BatchRenderer2D::is_visible_2d(Vec2 center, Vec2 half_size) const {
    if (!culling_enabled_) return true;
    // AABB vs AABB test
    if (center.x + half_size.x < camera_min_.x) return false;
    if (center.x - half_size.x > camera_max_.x) return false;
    if (center.y + half_size.y < camera_min_.y) return false;
    if (center.y - half_size.y > camera_max_.y) return false;
    return true;
}

// ── Tilemap drawing ─────────────────────────────────────────────────────────

void BatchRenderer2D::draw_tilemap(const i32* tile_data, u32 map_width, u32 map_height,
                                    float tile_size, Vec2 origin,
                                    rhi::TextureHandle atlas_texture,
                                    u32 tiles_per_row, u32 tiles_per_col,
                                    Vec4 tint) {
    if (!tile_data || tiles_per_row == 0 || tiles_per_col == 0) return;

    float inv_row = 1.0f / static_cast<float>(tiles_per_row);
    float inv_col = 1.0f / static_cast<float>(tiles_per_col);

    for (u32 y = 0; y < map_height; ++y) {
        for (u32 x = 0; x < map_width; ++x) {
            i32 tile_id = tile_data[y * map_width + x];
            if (tile_id < 0) continue; // empty tile

            Vec2 tile_center = origin + Vec2{
                (static_cast<float>(x) + 0.5f) * tile_size,
                (static_cast<float>(y) + 0.5f) * tile_size
            };
            Vec2 half = Vec2(tile_size * 0.5f);

            // Frustum cull individual tiles
            if (!is_visible_2d(tile_center, half)) continue;

            // Compute UV from tile atlas
            u32 tx = static_cast<u32>(tile_id) % tiles_per_row;
            u32 ty = static_cast<u32>(tile_id) / tiles_per_row;
            Vec2 uv_min{static_cast<float>(tx) * inv_row, static_cast<float>(ty) * inv_col};
            Vec2 uv_max{static_cast<float>(tx + 1) * inv_row, static_cast<float>(ty + 1) * inv_col};

            draw_quad(tile_center, Vec2(tile_size), 0.0f, atlas_texture, tint, uv_min, uv_max);
        }
    }
}

// ── Glyph drawing ───────────────────────────────────────────────────────────

void BatchRenderer2D::draw_glyph(Vec2 position, Vec2 size,
                                  rhi::TextureHandle atlas_texture,
                                  Vec2 uv_min, Vec2 uv_max,
                                  Vec4 color) {
    if (vertex_count_ + 4 > MAX_VERTICES) {
        flush();
        start_batch();
    }

    float tex_index = find_or_add_texture(atlas_texture);

    // Glyph quads are axis-aligned, no rotation
    float x0 = position.x;
    float y0 = position.y;
    float x1 = position.x + size.x;
    float y1 = position.y + size.y;

    vertices_[vertex_count_].position  = Vec3(x0, y0, 0.0f);
    vertices_[vertex_count_].color     = color;
    vertices_[vertex_count_].texcoord  = Vec2(uv_min.x, uv_min.y);
    vertices_[vertex_count_].tex_index = tex_index;
    vertex_count_++;

    vertices_[vertex_count_].position  = Vec3(x1, y0, 0.0f);
    vertices_[vertex_count_].color     = color;
    vertices_[vertex_count_].texcoord  = Vec2(uv_max.x, uv_min.y);
    vertices_[vertex_count_].tex_index = tex_index;
    vertex_count_++;

    vertices_[vertex_count_].position  = Vec3(x1, y1, 0.0f);
    vertices_[vertex_count_].color     = color;
    vertices_[vertex_count_].texcoord  = Vec2(uv_max.x, uv_max.y);
    vertices_[vertex_count_].tex_index = tex_index;
    vertex_count_++;

    vertices_[vertex_count_].position  = Vec3(x0, y1, 0.0f);
    vertices_[vertex_count_].color     = color;
    vertices_[vertex_count_].texcoord  = Vec2(uv_min.x, uv_max.y);
    vertices_[vertex_count_].tex_index = tex_index;
    vertex_count_++;

    stats_.quad_count++;
}

} // namespace nexus
