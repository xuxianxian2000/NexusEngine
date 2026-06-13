// ============================================================================
// vk_rhi.cpp — Vulkan-style RHI backend (CPU-simulated)
//
// Tracks all resources, validates state, and records draw commands.
// Can be swapped in via RHI::create() when Vulkan is the chosen backend.
// ============================================================================

#include <nexus/rhi/vk_rhi.h>
#include <nexus/core/log.h>

#include <algorithm>
#include <cstring>

namespace nexus::rhi {

// ── Lifecycle ────────────────────────────────────────────────────────────────

bool VulkanRHI::init() {
    if (initialized_) {
        NX_WARN("VulkanRHI::init() called on already-initialized backend");
        return true;
    }

    // Reserve slot 0 as invalid for each resource type.
    buffers_.push_back({});
    textures_.push_back({});
    shaders_.push_back({});
    pipelines_.push_back({});
    framebuffers_.push_back({});

    initialized_ = true;
    NX_INFO("Vulkan RHI initialized (CPU-simulated backend)");
    return true;
}

void VulkanRHI::shutdown() {
    buffers_.clear();
    textures_.clear();
    shaders_.clear();
    pipelines_.clear();
    framebuffers_.clear();
    state_ = BoundState{};
    in_frame_ = false;
    initialized_ = false;
    NX_INFO("Vulkan RHI shut down");
}

// ── Buffers ──────────────────────────────────────────────────────────────────

BufferHandle VulkanRHI::create_buffer(const BufferDesc& desc) {
    VkBuffer buf;
    buf.alive = true;
    buf.type  = desc.type;
    buf.usage = desc.usage;
    buf.data.resize(desc.size);
    if (desc.data && desc.size > 0) {
        std::memcpy(buf.data.data(), desc.data, desc.size);
    }

    auto handle = static_cast<BufferHandle>(buffers_.size());
    buffers_.push_back(std::move(buf));
    return handle;
}

void VulkanRHI::destroy_buffer(BufferHandle handle) {
    if (handle == INVALID_HANDLE || handle >= static_cast<u32>(buffers_.size())) return;
    buffers_[handle].alive = false;
    buffers_[handle].data.clear();
    buffers_[handle].data.shrink_to_fit();
}

void VulkanRHI::update_buffer(BufferHandle handle,
                               const void* data, size_t size, size_t offset) {
    if (handle == INVALID_HANDLE || handle >= static_cast<u32>(buffers_.size())) return;
    auto& buf = buffers_[handle];
    if (!buf.alive) return;
    if (size == 0) return;

    // Guard against size_t overflow in offset+size before using it.
    if (offset > SIZE_MAX - size) {
        NX_WARN("VulkanRHI: update_buffer ignored — offset+size overflows");
        return;
    }
    if (offset + size > buf.data.size()) {
        NX_WARN("VulkanRHI: update_buffer grows buffer from {} to {} bytes",
                buf.data.size(), offset + size);
        buf.data.resize(offset + size);
    }
    if (data) {
        std::memcpy(buf.data.data() + offset, data, size);
    }
}

// ── Textures ─────────────────────────────────────────────────────────────────

TextureHandle VulkanRHI::create_texture(const TextureDesc& desc) {
    VkTexture tex;
    tex.alive  = true;
    tex.width  = desc.width;
    tex.height = desc.height;
    tex.format = desc.format;

    // Compute bytes per pixel based on format.
    u32 bpp = 0;
    switch (desc.format) {
        case TextureFormat::RGBA8:            bpp = 4;  break;
        case TextureFormat::RGB8:             bpp = 3;  break;
        case TextureFormat::R8:               bpp = 1;  break;
        case TextureFormat::RGBA16F:          bpp = 8;  break;
        case TextureFormat::RGBA32F:          bpp = 16; break;
        case TextureFormat::Depth32F:         bpp = 4;  break;
        case TextureFormat::Depth24Stencil8:  bpp = 4;  break;
    }
    if (bpp == 0) {
        NX_WARN("VulkanRHI: create_texture received an unsupported format");
        return INVALID_HANDLE;
    }

    size_t total = static_cast<size_t>(desc.width)
                 * static_cast<size_t>(desc.height) * bpp;
    tex.pixels.resize(total);
    if (desc.data && total > 0) {
        std::memcpy(tex.pixels.data(), desc.data, total);
    }

    auto handle = static_cast<TextureHandle>(textures_.size());
    textures_.push_back(std::move(tex));
    return handle;
}

void VulkanRHI::destroy_texture(TextureHandle handle) {
    if (handle == INVALID_HANDLE || handle >= static_cast<u32>(textures_.size())) return;
    textures_[handle].alive = false;
    textures_[handle].pixels.clear();
    textures_[handle].pixels.shrink_to_fit();
}

// ── Shaders ──────────────────────────────────────────────────────────────────

ShaderHandle VulkanRHI::create_shader(const std::string& vertex_src,
                                       const std::string& fragment_src) {
    if (vertex_src.empty() || fragment_src.empty()) {
        NX_ERROR("VulkanRHI: cannot create shader with empty source");
        return INVALID_HANDLE;
    }

    VkShader shader;
    shader.alive        = true;
    shader.vertex_src   = vertex_src;
    shader.fragment_src = fragment_src;

    auto handle = static_cast<ShaderHandle>(shaders_.size());
    shaders_.push_back(std::move(shader));
    return handle;
}

void VulkanRHI::destroy_shader(ShaderHandle handle) {
    if (handle == INVALID_HANDLE || handle >= static_cast<u32>(shaders_.size())) return;
    auto& s = shaders_[handle];
    s.alive = false;
    s.vertex_src.clear();
    s.fragment_src.clear();
    s.uniform_ints.clear();
    s.uniform_floats.clear();
    s.uniform_vec2s.clear();
    s.uniform_vec3s.clear();
    s.uniform_vec4s.clear();
    s.uniform_mat4s.clear();
}

// ── Pipelines ────────────────────────────────────────────────────────────────

PipelineHandle VulkanRHI::create_pipeline(const PipelineDesc& desc) {
    VkPipeline pipe;
    pipe.alive = true;
    pipe.desc  = desc;

    auto handle = static_cast<PipelineHandle>(pipelines_.size());
    pipelines_.push_back(std::move(pipe));
    return handle;
}

void VulkanRHI::destroy_pipeline(PipelineHandle handle) {
    if (handle == INVALID_HANDLE || handle >= static_cast<u32>(pipelines_.size())) return;
    pipelines_[handle].alive = false;
}

// ── Framebuffers ─────────────────────────────────────────────────────────────

FramebufferHandle VulkanRHI::create_framebuffer(const FramebufferDesc& desc) {
    VkFramebuffer fb;
    fb.alive         = true;
    fb.width         = desc.width;
    fb.height        = desc.height;
    fb.color_formats = desc.color_attachments;
    fb.has_depth     = desc.has_depth;

    auto handle = static_cast<FramebufferHandle>(framebuffers_.size());
    framebuffers_.push_back(std::move(fb));
    return handle;
}

void VulkanRHI::destroy_framebuffer(FramebufferHandle handle) {
    if (handle == INVALID_HANDLE || handle >= static_cast<u32>(framebuffers_.size())) return;
    framebuffers_[handle].alive = false;
    framebuffers_[handle].color_formats.clear();
}

// ── Frame ────────────────────────────────────────────────────────────────────

void VulkanRHI::begin_frame() {
    if (in_frame_) {
        NX_WARN("VulkanRHI: begin_frame() called while already in frame");
    }
    draw_call_count_    = 0;
    state_change_count_ = 0;
    in_frame_ = true;
}

void VulkanRHI::end_frame() {
    if (!in_frame_) {
        NX_WARN("VulkanRHI: end_frame() called without matching begin_frame()");
    }
    in_frame_ = false;
}

// ── Render commands ──────────────────────────────────────────────────────────

void VulkanRHI::set_viewport(i32 x, i32 y, i32 w, i32 h) {
    state_.viewport_x = x;
    state_.viewport_y = y;
    state_.viewport_w = w;
    state_.viewport_h = h;
    ++state_change_count_;
}

void VulkanRHI::set_scissor(i32 x, i32 y, i32 w, i32 h) {
    state_.scissor_x = x;
    state_.scissor_y = y;
    state_.scissor_w = w;
    state_.scissor_h = h;
    ++state_change_count_;
}

void VulkanRHI::clear(Vec4 /*color*/, float /*depth*/) {
    ++state_change_count_;
}

void VulkanRHI::bind_pipeline(PipelineHandle handle) {
    if (handle == INVALID_HANDLE || handle >= static_cast<u32>(pipelines_.size())) return;
    if (!pipelines_[handle].alive) return;
    state_.pipeline = handle;
    ++state_change_count_;

    // Also bind the shader attached to the pipeline.
    auto& pipe = pipelines_[handle];
    if (pipe.desc.shader != INVALID_HANDLE) {
        bind_shader(pipe.desc.shader);
    }
}

void VulkanRHI::bind_shader(ShaderHandle handle) {
    if (handle == INVALID_HANDLE || handle >= static_cast<u32>(shaders_.size())) return;
    if (!shaders_[handle].alive) return;
    state_.shader = handle;
    ++state_change_count_;
}

void VulkanRHI::bind_texture(TextureHandle handle, u32 /*slot*/) {
    // INVALID_HANDLE is valid (unbinds the texture slot).
    if (handle != INVALID_HANDLE) {
        if (handle >= static_cast<u32>(textures_.size())) return;
        if (!textures_[handle].alive) return;
    }
    ++state_change_count_;
}

void VulkanRHI::bind_framebuffer(FramebufferHandle handle) {
    if (handle == INVALID_HANDLE || handle >= static_cast<u32>(framebuffers_.size())) return;
    if (!framebuffers_[handle].alive) return;
    state_.framebuffer = handle;
    ++state_change_count_;
}

void VulkanRHI::unbind_framebuffer() {
    state_.framebuffer = INVALID_HANDLE;
    ++state_change_count_;
}

void VulkanRHI::bind_vertex_buffer(BufferHandle handle) {
    if (handle == INVALID_HANDLE || handle >= static_cast<u32>(buffers_.size())) return;
    if (!buffers_[handle].alive) return;
    state_.vertex_buffer = handle;
    ++state_change_count_;
}

void VulkanRHI::bind_index_buffer(BufferHandle handle) {
    if (handle == INVALID_HANDLE || handle >= static_cast<u32>(buffers_.size())) return;
    if (!buffers_[handle].alive) return;
    state_.index_buffer = handle;
    ++state_change_count_;
}

// ── State toggles ────────────────────────────────────────────────────────────

void VulkanRHI::set_blend_mode(BlendMode mode) {
    state_.blend = mode;
    ++state_change_count_;
}

void VulkanRHI::set_depth_test(bool enabled) {
    state_.depth_test = enabled;
    ++state_change_count_;
}

void VulkanRHI::set_depth_write(bool enabled) {
    state_.depth_write = enabled;
    ++state_change_count_;
}

void VulkanRHI::set_cull_mode(CullMode mode) {
    state_.cull = mode;
    ++state_change_count_;
}

// ── Uniforms ─────────────────────────────────────────────────────────────────

void VulkanRHI::set_uniform_int(ShaderHandle shader,
                                 const std::string& name, i32 value) {
    if (shader == INVALID_HANDLE || shader >= static_cast<u32>(shaders_.size())) return;
    if (!shaders_[shader].alive) return;
    shaders_[shader].uniform_ints[name] = value;
}

void VulkanRHI::set_uniform_int_array(ShaderHandle shader,
                                       const std::string& name,
                                       const i32* values, u32 count) {
    if (shader == INVALID_HANDLE || shader >= static_cast<u32>(shaders_.size())) return;
    if (!shaders_[shader].alive || !values || count == 0) return;
    // CPU simulation stores only the first element.  Full array storage is
    // deferred to the native Vulkan backend.
    shaders_[shader].uniform_ints[name] = values[0];
}

void VulkanRHI::set_uniform_float(ShaderHandle shader,
                                   const std::string& name, float value) {
    if (shader == INVALID_HANDLE || shader >= static_cast<u32>(shaders_.size())) return;
    if (!shaders_[shader].alive) return;
    shaders_[shader].uniform_floats[name] = value;
}

void VulkanRHI::set_uniform_vec2(ShaderHandle shader,
                                  const std::string& name, Vec2 value) {
    if (shader == INVALID_HANDLE || shader >= static_cast<u32>(shaders_.size())) return;
    if (!shaders_[shader].alive) return;
    shaders_[shader].uniform_vec2s[name] = value;
}

void VulkanRHI::set_uniform_vec3(ShaderHandle shader,
                                  const std::string& name, Vec3 value) {
    if (shader == INVALID_HANDLE || shader >= static_cast<u32>(shaders_.size())) return;
    if (!shaders_[shader].alive) return;
    shaders_[shader].uniform_vec3s[name] = value;
}

void VulkanRHI::set_uniform_vec4(ShaderHandle shader,
                                  const std::string& name, Vec4 value) {
    if (shader == INVALID_HANDLE || shader >= static_cast<u32>(shaders_.size())) return;
    if (!shaders_[shader].alive) return;
    shaders_[shader].uniform_vec4s[name] = value;
}

void VulkanRHI::set_uniform_mat4(ShaderHandle shader,
                                  const std::string& name,
                                  const Mat4& value) {
    if (shader == INVALID_HANDLE || shader >= static_cast<u32>(shaders_.size())) return;
    if (!shaders_[shader].alive) return;
    shaders_[shader].uniform_mat4s[name] = value;
}

// ── Draw calls ───────────────────────────────────────────────────────────────

void VulkanRHI::draw(u32 /*vertex_count*/, u32 /*first_vertex*/) {
    ++draw_call_count_;
}

void VulkanRHI::draw_indexed(u32 /*index_count*/, u32 /*first_index*/) {
    ++draw_call_count_;
}

// ── Queries ──────────────────────────────────────────────────────────────────

u32 VulkanRHI::live_buffer_count() const {
    u32 count = 0;
    for (size_t i = 1; i < buffers_.size(); ++i) {
        if (buffers_[i].alive) ++count;
    }
    return count;
}

u32 VulkanRHI::live_texture_count() const {
    u32 count = 0;
    for (size_t i = 1; i < textures_.size(); ++i) {
        if (textures_[i].alive) ++count;
    }
    return count;
}

u32 VulkanRHI::live_shader_count() const {
    u32 count = 0;
    for (size_t i = 1; i < shaders_.size(); ++i) {
        if (shaders_[i].alive) ++count;
    }
    return count;
}

u32 VulkanRHI::live_pipeline_count() const {
    u32 count = 0;
    for (size_t i = 1; i < pipelines_.size(); ++i) {
        if (pipelines_[i].alive) ++count;
    }
    return count;
}

u32 VulkanRHI::live_framebuffer_count() const {
    u32 count = 0;
    for (size_t i = 1; i < framebuffers_.size(); ++i) {
        if (framebuffers_[i].alive) ++count;
    }
    return count;
}

} // namespace nexus::rhi
