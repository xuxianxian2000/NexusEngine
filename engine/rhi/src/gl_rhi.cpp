// ============================================================================
// gl_rhi.cpp - OpenGL 4.5 RHI backend implementation
// ============================================================================

#include <nexus/rhi/gl_rhi.h>
#if NEXUS_ENABLE_VULKAN
#include <nexus/rhi/vk_rhi.h>
#endif
#if NEXUS_ENABLE_WEBGL
#include <nexus/rhi/webgl_rhi.h>
#endif
#include <nexus/core/log.h>
#include <glm/gtc/type_ptr.hpp>

namespace nexus::rhi {

// ── Helpers ──────────────────────────────────────────────────────────────────

static GLenum to_gl_buffer_target(BufferType type) {
    switch (type) {
        case BufferType::Vertex:  return GL_ARRAY_BUFFER;
        case BufferType::Index:   return GL_ELEMENT_ARRAY_BUFFER;
        case BufferType::Uniform: return GL_UNIFORM_BUFFER;
        case BufferType::Storage: return GL_SHADER_STORAGE_BUFFER;
    }
    return GL_ARRAY_BUFFER;
}

static GLenum to_gl_usage(BufferUsage usage) {
    switch (usage) {
        case BufferUsage::Static:  return GL_STATIC_DRAW;
        case BufferUsage::Dynamic: return GL_DYNAMIC_DRAW;
        case BufferUsage::Stream:  return GL_STREAM_DRAW;
    }
    return GL_STATIC_DRAW;
}

static GLenum to_gl_internal_format(TextureFormat fmt) {
    switch (fmt) {
        case TextureFormat::RGBA8:            return GL_RGBA8;
        case TextureFormat::RGB8:             return GL_RGB8;
        case TextureFormat::R8:               return GL_R8;
        case TextureFormat::Depth24Stencil8:  return GL_DEPTH24_STENCIL8;
        case TextureFormat::Depth32F:         return GL_DEPTH_COMPONENT32F;
        case TextureFormat::RGBA16F:          return GL_RGBA16F;
        case TextureFormat::RGBA32F:          return GL_RGBA32F;
    }
    return GL_RGBA8;
}

static GLenum to_gl_pixel_format(TextureFormat fmt) {
    switch (fmt) {
        case TextureFormat::RGBA8:
        case TextureFormat::RGBA16F:
        case TextureFormat::RGBA32F:          return GL_RGBA;
        case TextureFormat::RGB8:             return GL_RGB;
        case TextureFormat::R8:               return GL_RED;
        case TextureFormat::Depth24Stencil8:  return GL_DEPTH_STENCIL;
        case TextureFormat::Depth32F:         return GL_DEPTH_COMPONENT;
    }
    return GL_RGBA;
}

static GLenum to_gl_pixel_type(TextureFormat fmt) {
    switch (fmt) {
        case TextureFormat::RGBA16F:
        case TextureFormat::RGBA32F:
        case TextureFormat::Depth32F:         return GL_FLOAT;
        case TextureFormat::Depth24Stencil8:  return GL_UNSIGNED_INT_24_8;
        default:                              return GL_UNSIGNED_BYTE;
    }
}

static GLint to_gl_filter(TextureFilter f) {
    switch (f) {
        case TextureFilter::Nearest:              return GL_NEAREST;
        case TextureFilter::Linear:               return GL_LINEAR;
        case TextureFilter::NearestMipmapLinear:  return GL_NEAREST_MIPMAP_LINEAR;
        case TextureFilter::LinearMipmapLinear:   return GL_LINEAR_MIPMAP_LINEAR;
    }
    return GL_LINEAR;
}

static GLint to_gl_wrap(TextureWrap w) {
    switch (w) {
        case TextureWrap::Repeat:         return GL_REPEAT;
        case TextureWrap::ClampToEdge:    return GL_CLAMP_TO_EDGE;
        case TextureWrap::MirroredRepeat: return GL_MIRRORED_REPEAT;
    }
    return GL_REPEAT;
}

static GLenum to_gl_primitive(PrimitiveType p) {
    switch (p) {
        case PrimitiveType::Triangles:     return GL_TRIANGLES;
        case PrimitiveType::Lines:         return GL_LINES;
        case PrimitiveType::Points:        return GL_POINTS;
        case PrimitiveType::TriangleStrip: return GL_TRIANGLE_STRIP;
        case PrimitiveType::LineStrip:     return GL_LINE_STRIP;
    }
    return GL_TRIANGLES;
}

static GLenum to_gl_depth_func(DepthFunc df) {
    switch (df) {
        case DepthFunc::Less:         return GL_LESS;
        case DepthFunc::LessEqual:    return GL_LEQUAL;
        case DepthFunc::Greater:      return GL_GREATER;
        case DepthFunc::GreaterEqual: return GL_GEQUAL;
        case DepthFunc::Equal:        return GL_EQUAL;
        case DepthFunc::Always:       return GL_ALWAYS;
        case DepthFunc::Never:        return GL_NEVER;
    }
    return GL_LESS;
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

bool OpenGLRHI::init() {
    NX_INFO("OpenGL RHI initialized");
    // Reserve slot 0 as invalid for each resource type
    buffers_.push_back({});
    textures_.push_back({});
    shaders_.push_back({});
    pipelines_.push_back({});
    framebuffers_.push_back({});
    return true;
}

void OpenGLRHI::shutdown() {
    // Destroy all remaining resources in reverse
    for (size_t i = 1; i < framebuffers_.size(); ++i) {
        auto& fb = framebuffers_[i];
        if (fb.fbo) {
            for (auto tex : fb.color_textures)
                gl::DeleteTextures(1, &tex);
            if (fb.depth_renderbuffer)
                gl::DeleteRenderbuffers(1, &fb.depth_renderbuffer);
            gl::DeleteFramebuffers(1, &fb.fbo);
        }
    }
    for (size_t i = 1; i < pipelines_.size(); ++i) {
        if (pipelines_[i].vao) gl::DeleteVertexArrays(1, &pipelines_[i].vao);
    }
    for (size_t i = 1; i < shaders_.size(); ++i) {
        if (shaders_[i].program) gl::DeleteProgram(shaders_[i].program);
    }
    for (size_t i = 1; i < textures_.size(); ++i) {
        if (textures_[i].id) gl::DeleteTextures(1, &textures_[i].id);
    }
    for (size_t i = 1; i < buffers_.size(); ++i) {
        if (buffers_[i].id) gl::DeleteBuffers(1, &buffers_[i].id);
    }

    buffers_.clear();
    textures_.clear();
    shaders_.clear();
    pipelines_.clear();
    framebuffers_.clear();

    NX_INFO("OpenGL RHI shut down");
}

// ── Buffers ──────────────────────────────────────────────────────────────────

BufferHandle OpenGLRHI::create_buffer(const BufferDesc& desc) {
    GLBuffer buf;
    buf.target = to_gl_buffer_target(desc.type);
    buf.usage  = to_gl_usage(desc.usage);

    gl::GenBuffers(1, &buf.id);
    gl::BindBuffer(buf.target, buf.id);
    gl::BufferData(buf.target,
                   static_cast<GLsizeiptr>(desc.size),
                   desc.data,
                   buf.usage);
    gl::BindBuffer(buf.target, 0);

    auto handle = static_cast<BufferHandle>(buffers_.size());
    buffers_.push_back(buf);
    return handle;
}

void OpenGLRHI::destroy_buffer(BufferHandle handle) {
    if (handle == INVALID_HANDLE || handle >= buffers_.size()) return;
    auto& buf = buffers_[handle];
    if (buf.id) {
        gl::DeleteBuffers(1, &buf.id);
        buf.id = 0;
    }
}

void OpenGLRHI::update_buffer(BufferHandle handle,
                              const void* data, size_t size, size_t offset) {
    if (handle == INVALID_HANDLE || handle >= buffers_.size()) return;
    auto& buf = buffers_[handle];
    gl::BindBuffer(buf.target, buf.id);
    gl::BufferSubData(buf.target,
                      static_cast<GLintptr>(offset),
                      static_cast<GLsizeiptr>(size),
                      data);
    gl::BindBuffer(buf.target, 0);
}

// ── Textures ────────────────────────────────────────────────────────────────

TextureHandle OpenGLRHI::create_texture(const TextureDesc& desc) {
    GLTexture tex;
    tex.width  = desc.width;
    tex.height = desc.height;

    gl::GenTextures(1, &tex.id);
    gl::BindTexture(GL_TEXTURE_2D, tex.id);

    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, to_gl_filter(desc.min_filter));
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, to_gl_filter(desc.mag_filter));
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     to_gl_wrap(desc.wrap_s));
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     to_gl_wrap(desc.wrap_t));

    GLenum internal_fmt = to_gl_internal_format(desc.format);
    GLenum pixel_fmt    = to_gl_pixel_format(desc.format);
    GLenum pixel_type   = to_gl_pixel_type(desc.format);

    gl::TexImage2D(GL_TEXTURE_2D, 0,
                   static_cast<GLint>(internal_fmt),
                   static_cast<GLsizei>(desc.width),
                   static_cast<GLsizei>(desc.height),
                   0, pixel_fmt, pixel_type, desc.data);

    if (desc.generate_mipmaps && desc.data) {
        gl::GenerateMipmap(GL_TEXTURE_2D);
    }

    gl::BindTexture(GL_TEXTURE_2D, 0);

    auto handle = static_cast<TextureHandle>(textures_.size());
    textures_.push_back(tex);
    return handle;
}

void OpenGLRHI::destroy_texture(TextureHandle handle) {
    if (handle == INVALID_HANDLE || handle >= textures_.size()) return;
    auto& tex = textures_[handle];
    if (tex.id) {
        gl::DeleteTextures(1, &tex.id);
        tex.id = 0;
    }
}

// ── Shaders ─────────────────────────────────────────────────────────────────

ShaderHandle OpenGLRHI::create_shader(const std::string& vertex_src,
                                      const std::string& fragment_src) {
    auto compile = [](GLenum type, const std::string& src) -> GLuint {
        GLuint s = gl::CreateShader(type);
        const char* c_str = src.c_str();
        gl::ShaderSource(s, 1, &c_str, nullptr);
        gl::CompileShader(s);

        GLint status = 0;
        gl::GetShaderiv(s, GL_COMPILE_STATUS, &status);
        if (!status) {
            GLint len = 0;
            gl::GetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
            std::string log(static_cast<size_t>(len), '\0');
            gl::GetShaderInfoLog(s, len, nullptr, log.data());
            NX_ERROR("Shader compile error: {}", log);
            gl::DeleteShader(s);
            return 0;
        }
        return s;
    };

    GLuint vs = compile(GL_VERTEX_SHADER, vertex_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragment_src);
    if (!vs || !fs) {
        if (vs) gl::DeleteShader(vs);
        if (fs) gl::DeleteShader(fs);
        return INVALID_HANDLE;
    }

    GLuint prog = gl::CreateProgram();
    gl::AttachShader(prog, vs);
    gl::AttachShader(prog, fs);
    gl::LinkProgram(prog);

    GLint status = 0;
    gl::GetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        GLint len = 0;
        gl::GetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(len), '\0');
        gl::GetProgramInfoLog(prog, len, nullptr, log.data());
        NX_ERROR("Shader link error: {}", log);
        gl::DeleteProgram(prog);
        gl::DeleteShader(vs);
        gl::DeleteShader(fs);
        return INVALID_HANDLE;
    }

    gl::DeleteShader(vs);
    gl::DeleteShader(fs);

    GLShader shader;
    shader.program = prog;

    auto handle = static_cast<ShaderHandle>(shaders_.size());
    shaders_.push_back(std::move(shader));
    return handle;
}

void OpenGLRHI::destroy_shader(ShaderHandle handle) {
    if (handle == INVALID_HANDLE || handle >= shaders_.size()) return;
    auto& s = shaders_[handle];
    if (s.program) {
        gl::DeleteProgram(s.program);
        s.program = 0;
        s.uniform_cache.clear();
    }
}

// ── Pipelines ───────────────────────────────────────────────────────────────

PipelineHandle OpenGLRHI::create_pipeline(const PipelineDesc& desc) {
    GLPipeline pipe;
    pipe.desc = desc;

    gl::GenVertexArrays(1, &pipe.vao);
    gl::BindVertexArray(pipe.vao);

    // Set up vertex attributes from layout
    for (const auto& attr : desc.vertex_layout.attributes) {
        gl::EnableVertexAttribArray(attr.location);
        gl::VertexAttribPointer(
            attr.location,
            static_cast<GLint>(attr.components),
            GL_FLOAT,
            attr.normalized ? GL_TRUE : GL_FALSE,
            static_cast<GLsizei>(desc.vertex_layout.stride),
            reinterpret_cast<const void*>(static_cast<uintptr_t>(attr.offset))
        );
    }

    gl::BindVertexArray(0);

    auto handle = static_cast<PipelineHandle>(pipelines_.size());
    pipelines_.push_back(pipe);
    return handle;
}

void OpenGLRHI::destroy_pipeline(PipelineHandle handle) {
    if (handle == INVALID_HANDLE || handle >= pipelines_.size()) return;
    auto& pipe = pipelines_[handle];
    if (pipe.vao) {
        gl::DeleteVertexArrays(1, &pipe.vao);
        pipe.vao = 0;
    }
}

// ── Framebuffers ────────────────────────────────────────────────────────────

FramebufferHandle OpenGLRHI::create_framebuffer(const FramebufferDesc& desc) {
    GLFramebuffer fb;
    fb.width  = desc.width;
    fb.height = desc.height;

    gl::GenFramebuffers(1, &fb.fbo);
    gl::BindFramebuffer(GL_FRAMEBUFFER, fb.fbo);

    // Color attachments
    for (size_t i = 0; i < desc.color_attachments.size(); ++i) {
        GLuint color_tex;
        gl::GenTextures(1, &color_tex);
        gl::BindTexture(GL_TEXTURE_2D, color_tex);

        GLenum internal_fmt = to_gl_internal_format(desc.color_attachments[i]);
        GLenum pixel_fmt    = to_gl_pixel_format(desc.color_attachments[i]);
        GLenum pixel_type   = to_gl_pixel_type(desc.color_attachments[i]);

        gl::TexImage2D(GL_TEXTURE_2D, 0,
                       static_cast<GLint>(internal_fmt),
                       static_cast<GLsizei>(desc.width),
                       static_cast<GLsizei>(desc.height),
                       0, pixel_fmt, pixel_type, nullptr);

        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        gl::FramebufferTexture2D(GL_FRAMEBUFFER,
                                 static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + i),
                                 GL_TEXTURE_2D, color_tex, 0);
        fb.color_textures.push_back(color_tex);
    }

    // Depth attachment
    if (desc.has_depth) {
        gl::GenRenderbuffers(1, &fb.depth_renderbuffer);
        gl::BindRenderbuffer(GL_RENDERBUFFER, fb.depth_renderbuffer);
        gl::RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                                static_cast<GLsizei>(desc.width),
                                static_cast<GLsizei>(desc.height));
        gl::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                    GL_RENDERBUFFER, fb.depth_renderbuffer);
    }

    if (gl::CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        NX_ERROR("Framebuffer is not complete!");
    }

    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);

    auto handle = static_cast<FramebufferHandle>(framebuffers_.size());
    framebuffers_.push_back(fb);
    return handle;
}

void OpenGLRHI::destroy_framebuffer(FramebufferHandle handle) {
    if (handle == INVALID_HANDLE || handle >= framebuffers_.size()) return;
    auto& fb = framebuffers_[handle];
    if (fb.fbo) {
        for (auto tex : fb.color_textures)
            gl::DeleteTextures(1, &tex);
        fb.color_textures.clear();
        if (fb.depth_renderbuffer)
            gl::DeleteRenderbuffers(1, &fb.depth_renderbuffer);
        gl::DeleteFramebuffers(1, &fb.fbo);
        fb.fbo = 0;
    }
}

// ── Frame ───────────────────────────────────────────────────────────────────

void OpenGLRHI::begin_frame() {
    // Nothing special needed for OpenGL
}

void OpenGLRHI::end_frame() {
    // Nothing special needed for OpenGL
}

// ── Render commands ─────────────────────────────────────────────────────────

void OpenGLRHI::set_viewport(i32 x, i32 y, i32 w, i32 h) {
    gl::Viewport(x, y, w, h);
}

void OpenGLRHI::set_scissor(i32 x, i32 y, i32 w, i32 h) {
    // A non-positive rect disables the scissor test, so a single scissor call
    // can no longer leak into every subsequent draw.
    if (w <= 0 || h <= 0) {
        gl::Disable(GL_SCISSOR_TEST);
        return;
    }
    gl::Enable(GL_SCISSOR_TEST);
    gl::Scissor(x, y, w, h);
}

void OpenGLRHI::clear(Vec4 color, float depth) {
    gl::ClearColor(color.r, color.g, color.b, color.a);
    gl::ClearDepth(static_cast<GLdouble>(depth));
    // glClear of the depth buffer is gated by the depth write mask; force it on
    // so the clear is never silently dropped when the last pipeline left depth
    // writes disabled, then restore the tracked state.
    if (!depth_write_) gl::DepthMask(GL_TRUE);
    gl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!depth_write_) gl::DepthMask(GL_FALSE);
}

void OpenGLRHI::bind_pipeline(PipelineHandle handle) {
    if (handle == INVALID_HANDLE || handle >= pipelines_.size()) return;
    auto& pipe = pipelines_[handle];

    gl::BindVertexArray(pipe.vao);
    bound_primitive_ = to_gl_primitive(pipe.desc.primitive);

    // Apply pipeline state
    if (pipe.desc.depth_test) {
        gl::Enable(GL_DEPTH_TEST);
        gl::DepthFunc_(to_gl_depth_func(pipe.desc.depth));
    } else {
        gl::Disable(GL_DEPTH_TEST);
    }

    gl::DepthMask(pipe.desc.depth_write ? GL_TRUE : GL_FALSE);
    depth_write_ = pipe.desc.depth_write;

    // Cull mode
    if (pipe.desc.cull == CullMode::None) {
        gl::Disable(GL_CULL_FACE);
    } else {
        gl::Enable(GL_CULL_FACE);
        gl::CullFace(pipe.desc.cull == CullMode::Front ? GL_FRONT : GL_BACK);
    }

    // Blend mode
    set_blend_mode(pipe.desc.blend);

    // Bind shader if present
    if (pipe.desc.shader != INVALID_HANDLE) {
        bind_shader(pipe.desc.shader);
    }
}

void OpenGLRHI::bind_shader(ShaderHandle handle) {
    if (handle == INVALID_HANDLE || handle >= shaders_.size()) return;
    gl::UseProgram(shaders_[handle].program);
    bound_shader_ = handle;
}

void OpenGLRHI::bind_texture(TextureHandle handle, u32 slot) {
    gl::ActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + slot));
    if (handle == INVALID_HANDLE || handle >= textures_.size()) {
        gl::BindTexture(GL_TEXTURE_2D, 0);
        return;
    }
    gl::BindTexture(GL_TEXTURE_2D, textures_[handle].id);
}

void OpenGLRHI::bind_framebuffer(FramebufferHandle handle) {
    if (handle == INVALID_HANDLE || handle >= framebuffers_.size()) return;
    gl::BindFramebuffer(GL_FRAMEBUFFER, framebuffers_[handle].fbo);
    gl::Viewport(0, 0,
                 static_cast<GLsizei>(framebuffers_[handle].width),
                 static_cast<GLsizei>(framebuffers_[handle].height));
}

void OpenGLRHI::unbind_framebuffer() {
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
}

void OpenGLRHI::bind_vertex_buffer(BufferHandle handle) {
    if (handle == INVALID_HANDLE || handle >= buffers_.size()) return;
    gl::BindBuffer(GL_ARRAY_BUFFER, buffers_[handle].id);
}

void OpenGLRHI::bind_index_buffer(BufferHandle handle) {
    if (handle == INVALID_HANDLE || handle >= buffers_.size()) return;
    gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers_[handle].id);
}

// ── State toggles ───────────────────────────────────────────────────────────

void OpenGLRHI::set_blend_mode(BlendMode mode) {
    switch (mode) {
        case BlendMode::None:
            gl::Disable(GL_BLEND);
            break;
        case BlendMode::Alpha:
            gl::Enable(GL_BLEND);
            gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            gl::BlendEquation(GL_FUNC_ADD);
            break;
        case BlendMode::Additive:
            gl::Enable(GL_BLEND);
            gl::BlendFunc(GL_SRC_ALPHA, GL_ONE);
            gl::BlendEquation(GL_FUNC_ADD);
            break;
        case BlendMode::Multiply:
            gl::Enable(GL_BLEND);
            gl::BlendFunc(GL_DST_COLOR, GL_ZERO);
            gl::BlendEquation(GL_FUNC_ADD);
            break;
    }
}

void OpenGLRHI::set_depth_test(bool enabled) {
    if (enabled)
        gl::Enable(GL_DEPTH_TEST);
    else
        gl::Disable(GL_DEPTH_TEST);
}

void OpenGLRHI::set_depth_write(bool enabled) {
    gl::DepthMask(enabled ? GL_TRUE : GL_FALSE);
    depth_write_ = enabled;
}

void OpenGLRHI::set_cull_mode(CullMode mode) {
    if (mode == CullMode::None) {
        gl::Disable(GL_CULL_FACE);
    } else {
        gl::Enable(GL_CULL_FACE);
        gl::CullFace(mode == CullMode::Front ? GL_FRONT : GL_BACK);
    }
}

// ── Uniforms ────────────────────────────────────────────────────────────────

GLint OpenGLRHI::get_uniform_loc(ShaderHandle shader, const std::string& name) {
    if (shader == INVALID_HANDLE || shader >= shaders_.size()) return -1;
    auto& s = shaders_[shader];
    auto it = s.uniform_cache.find(name);
    if (it != s.uniform_cache.end()) return it->second;
    GLint loc = gl::GetUniformLocation(s.program, name.c_str());
    s.uniform_cache[name] = loc;
    return loc;
}

void OpenGLRHI::set_uniform_int(ShaderHandle shader,
                                const std::string& name, i32 value) {
    GLint loc = get_uniform_loc(shader, name);
    if (loc >= 0) gl::Uniform1i(loc, value);
}

void OpenGLRHI::set_uniform_int_array(ShaderHandle shader,
                                      const std::string& name,
                                      const i32* values, u32 count) {
    GLint loc = get_uniform_loc(shader, name);
    if (loc >= 0) gl::Uniform1iv(loc, static_cast<GLsizei>(count), values);
}

void OpenGLRHI::set_uniform_float(ShaderHandle shader,
                                  const std::string& name, float value) {
    GLint loc = get_uniform_loc(shader, name);
    if (loc >= 0) gl::Uniform1f(loc, value);
}

void OpenGLRHI::set_uniform_vec2(ShaderHandle shader,
                                 const std::string& name, Vec2 value) {
    GLint loc = get_uniform_loc(shader, name);
    if (loc >= 0) gl::Uniform2f(loc, value.x, value.y);
}

void OpenGLRHI::set_uniform_vec3(ShaderHandle shader,
                                 const std::string& name, Vec3 value) {
    GLint loc = get_uniform_loc(shader, name);
    if (loc >= 0) gl::Uniform3f(loc, value.x, value.y, value.z);
}

void OpenGLRHI::set_uniform_vec4(ShaderHandle shader,
                                 const std::string& name, Vec4 value) {
    GLint loc = get_uniform_loc(shader, name);
    if (loc >= 0) gl::Uniform4f(loc, value.x, value.y, value.z, value.w);
}

void OpenGLRHI::set_uniform_mat4(ShaderHandle shader,
                                 const std::string& name,
                                 const Mat4& value) {
    GLint loc = get_uniform_loc(shader, name);
    if (loc >= 0) gl::UniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(value));
}

// ── Draw calls ──────────────────────────────────────────────────────────────

void OpenGLRHI::draw(u32 vertex_count, u32 first_vertex) {
    gl::DrawArrays(bound_primitive_,
                   static_cast<GLint>(first_vertex),
                   static_cast<GLsizei>(vertex_count));
}

void OpenGLRHI::draw_indexed(u32 index_count, u32 first_index) {
    gl::DrawElements(
        bound_primitive_,
        static_cast<GLsizei>(index_count),
        GL_UNSIGNED_INT,
        reinterpret_cast<const void*>(static_cast<uintptr_t>(first_index * sizeof(u32)))
    );
}

// ── Factory ─────────────────────────────────────────────────────────────────

std::unique_ptr<RHI> RHI::create() {
    return std::make_unique<OpenGLRHI>();
}

std::unique_ptr<RHI> RHI::create(Backend backend) {
    switch (backend) {
#if NEXUS_ENABLE_OPENGL
        case Backend::OpenGL:
            return std::make_unique<OpenGLRHI>();
#endif
#if NEXUS_ENABLE_VULKAN
        case Backend::Vulkan:
            return std::make_unique<VulkanRHI>();
#endif
#if NEXUS_ENABLE_WEBGL
        case Backend::WebGL:
            return std::make_unique<WebGLRHI>();
#endif
        default:
            NX_ERROR("Requested RHI backend is not available");
            return nullptr;
    }
}

} // namespace nexus::rhi
