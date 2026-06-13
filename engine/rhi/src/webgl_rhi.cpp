// ============================================================================
// webgl_rhi.cpp — WebGL 2.0 / OpenGL ES 3.0 RHI backend
//
// On Emscripten builds: issues real WebGL 2.0 calls via GLES3/gl3.h.
// On other platforms: CPU-side simulation for testing (like VulkanRHI).
// ============================================================================

#include <nexus/rhi/webgl_rhi.h>
#include <nexus/core/log.h>

#include <algorithm>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#define WEBGL_REAL 1
#else
#define WEBGL_REAL 0
#endif

namespace nexus::rhi {

// ── Helpers (Emscripten only) ───────────────────────────────────────────────

#if WEBGL_REAL
static GLenum to_gl_buffer_target(BufferType type) {
    switch (type) {
    case BufferType::Vertex:  return GL_ARRAY_BUFFER;
    case BufferType::Index:   return GL_ELEMENT_ARRAY_BUFFER;
    case BufferType::Uniform: return GL_UNIFORM_BUFFER;
    case BufferType::Storage: return GL_ARRAY_BUFFER; // No SSBO in ES 3.0
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

static GLenum to_gl_primitive(PrimitiveType type) {
    switch (type) {
    case PrimitiveType::Triangles:     return GL_TRIANGLES;
    case PrimitiveType::Lines:         return GL_LINES;
    case PrimitiveType::Points:        return GL_POINTS;
    case PrimitiveType::TriangleStrip: return GL_TRIANGLE_STRIP;
    case PrimitiveType::LineStrip:     return GL_LINE_STRIP;
    }
    return GL_TRIANGLES;
}
#endif

// ── Lifecycle ───────────────────────────────────────────────────────────────

bool WebGLRHI::init() {
    if (initialized_) {
        NX_WARN("WebGLRHI::init() called on already-initialized backend");
        return true;
    }

    // Reserve slot 0 as invalid.
    buffers_.push_back({});
    textures_.push_back({});
    shaders_.push_back({});
    pipelines_.push_back({});
    framebuffers_.push_back({});

#if WEBGL_REAL
    // Query WebGL2 capabilities
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, reinterpret_cast<GLint*>(&max_texture_size_));
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, reinterpret_cast<GLint*>(&max_vertex_attribs_));

    // Check for float texture support (OES_texture_float is usually available in WebGL2)
    ext_float_textures_ = true; // WebGL2 supports float textures natively
    ext_instancing_ = true;     // WebGL2 has instancing in core

    NX_INFO("WebGL 2.0 RHI initialized (max tex: {}, max attribs: {})",
            max_texture_size_, max_vertex_attribs_);
#else
    max_texture_size_ = 4096;
    max_vertex_attribs_ = 16;
    ext_float_textures_ = false;
    ext_instancing_ = true;

    NX_INFO("WebGL RHI initialized (CPU-simulated, non-Emscripten build)");
#endif

    initialized_ = true;
    return true;
}

void WebGLRHI::shutdown() {
#if WEBGL_REAL
    // Destroy all GL resources
    for (u32 i = 1; i < static_cast<u32>(buffers_.size()); ++i) {
        if (buffers_[i].alive && buffers_[i].gl_id) {
            glDeleteBuffers(1, &buffers_[i].gl_id);
        }
    }
    for (u32 i = 1; i < static_cast<u32>(textures_.size()); ++i) {
        if (textures_[i].alive && textures_[i].gl_id) {
            glDeleteTextures(1, &textures_[i].gl_id);
        }
    }
    for (u32 i = 1; i < static_cast<u32>(shaders_.size()); ++i) {
        if (shaders_[i].alive && shaders_[i].program) {
            glDeleteProgram(shaders_[i].program);
        }
    }
    for (u32 i = 1; i < static_cast<u32>(pipelines_.size()); ++i) {
        if (pipelines_[i].alive && pipelines_[i].vao) {
            glDeleteVertexArrays(1, &pipelines_[i].vao);
        }
    }
    for (u32 i = 1; i < static_cast<u32>(framebuffers_.size()); ++i) {
        auto& fb = framebuffers_[i];
        if (!fb.alive) continue;
        if (!fb.color_textures.empty()) {
            glDeleteTextures(static_cast<GLsizei>(fb.color_textures.size()),
                             fb.color_textures.data());
        }
        if (fb.depth_rb) glDeleteRenderbuffers(1, &fb.depth_rb);
        if (fb.fbo) glDeleteFramebuffers(1, &fb.fbo);
    }
#endif

    buffers_.clear();
    textures_.clear();
    shaders_.clear();
    pipelines_.clear();
    framebuffers_.clear();
    initialized_ = false;
    NX_INFO("WebGL RHI shut down");
}

// ── Buffers ─────────────────────────────────────────────────────────────────

BufferHandle WebGLRHI::create_buffer(const BufferDesc& desc) {
    WGLBuffer buf;
    buf.alive = true;
    buf.type = desc.type;
    buf.usage = desc.usage;
    buf.size = desc.size;

#if WEBGL_REAL
    glGenBuffers(1, &buf.gl_id);
    GLenum target = to_gl_buffer_target(desc.type);
    glBindBuffer(target, buf.gl_id);
    glBufferData(target, static_cast<GLsizeiptr>(desc.size),
                 desc.data, to_gl_usage(desc.usage));
    glBindBuffer(target, 0);
#endif

    auto handle = static_cast<BufferHandle>(buffers_.size());
    buffers_.push_back(std::move(buf));
    return handle;
}

void WebGLRHI::destroy_buffer(BufferHandle handle) {
    if (handle == 0 || handle >= buffers_.size() || !buffers_[handle].alive) return;
#if WEBGL_REAL
    if (buffers_[handle].gl_id) {
        glDeleteBuffers(1, &buffers_[handle].gl_id);
    }
#endif
    buffers_[handle].alive = false;
    buffers_[handle].gl_id = 0;
}

void WebGLRHI::update_buffer(BufferHandle handle,
                              const void* data, size_t size, size_t offset) {
    if (handle == 0 || handle >= buffers_.size() || !buffers_[handle].alive) return;
#if WEBGL_REAL
    GLenum target = to_gl_buffer_target(buffers_[handle].type);
    glBindBuffer(target, buffers_[handle].gl_id);
    glBufferSubData(target, static_cast<GLintptr>(offset),
                    static_cast<GLsizeiptr>(size), data);
    glBindBuffer(target, 0);
#else
    (void)data; (void)size; (void)offset;
#endif
}

// ── Textures ────────────────────────────────────────────────────────────────

TextureHandle WebGLRHI::create_texture(const TextureDesc& desc) {
    WGLTexture tex;
    tex.alive = true;
    tex.width = desc.width;
    tex.height = desc.height;
    tex.format = desc.format;

#if WEBGL_REAL
    glGenTextures(1, &tex.gl_id);
    glBindTexture(GL_TEXTURE_2D, tex.gl_id);

    GLenum internal_format = GL_RGBA8;
    GLenum pixel_format = GL_RGBA;
    GLenum pixel_type = GL_UNSIGNED_BYTE;
    switch (desc.format) {
    case TextureFormat::RGBA8:  internal_format = GL_RGBA8;  pixel_format = GL_RGBA;  break;
    case TextureFormat::RGB8:   internal_format = GL_RGB8;   pixel_format = GL_RGB;   break;
    case TextureFormat::R8:     internal_format = GL_R8;     pixel_format = GL_RED;   break;
    case TextureFormat::Depth24Stencil8: internal_format = GL_DEPTH24_STENCIL8; pixel_format = GL_DEPTH_STENCIL; pixel_type = GL_UNSIGNED_INT_24_8; break;
    case TextureFormat::Depth32F: internal_format = GL_DEPTH_COMPONENT32F; pixel_format = GL_DEPTH_COMPONENT; pixel_type = GL_FLOAT; break;
    case TextureFormat::RGBA16F: internal_format = GL_RGBA16F; pixel_format = GL_RGBA; pixel_type = GL_HALF_FLOAT; break;
    case TextureFormat::RGBA32F: internal_format = GL_RGBA32F; pixel_format = GL_RGBA; pixel_type = GL_FLOAT; break;
    }

    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, desc.width, desc.height,
                 0, pixel_format, pixel_type, desc.data);

    // Filtering (ES 3.0 compatible)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    desc.min_filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    desc.mag_filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                    desc.wrap_s == TextureWrap::ClampToEdge ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                    desc.wrap_t == TextureWrap::ClampToEdge ? GL_CLAMP_TO_EDGE : GL_REPEAT);

    if (desc.generate_mipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
#endif

    auto handle = static_cast<TextureHandle>(textures_.size());
    textures_.push_back(std::move(tex));
    return handle;
}

void WebGLRHI::destroy_texture(TextureHandle handle) {
    if (handle == 0 || handle >= textures_.size() || !textures_[handle].alive) return;
#if WEBGL_REAL
    if (textures_[handle].gl_id) {
        glDeleteTextures(1, &textures_[handle].gl_id);
    }
#endif
    textures_[handle].alive = false;
    textures_[handle].gl_id = 0;
}

// ── Shaders ─────────────────────────────────────────────────────────────────

ShaderHandle WebGLRHI::create_shader(const std::string& vertex_src,
                                      const std::string& fragment_src) {
    if (vertex_src.empty() || fragment_src.empty()) {
        NX_ERROR("WebGLRHI: cannot create shader with empty source");
        return INVALID_HANDLE;
    }

    WGLShader shader;
    shader.alive = true;
    shader.vertex_src = vertex_src;
    shader.fragment_src = fragment_src;

#if WEBGL_REAL
    auto compile = [](GLenum type, const std::string& src) -> GLuint {
        GLuint s = glCreateShader(type);
        const char* c_str = src.c_str();
        glShaderSource(s, 1, &c_str, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char info[512];
            glGetShaderInfoLog(s, sizeof(info), nullptr, info);
            NX_ERROR("WebGL shader compile error: {}", info);
            glDeleteShader(s);
            return 0;
        }
        return s;
    };

    GLuint vs = compile(GL_VERTEX_SHADER, vertex_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragment_src);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return INVALID_HANDLE;
    }

    shader.program = glCreateProgram();
    glAttachShader(shader.program, vs);
    glAttachShader(shader.program, fs);
    glLinkProgram(shader.program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(shader.program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char info[512];
        glGetProgramInfoLog(shader.program, sizeof(info), nullptr, info);
        NX_ERROR("WebGL program link error: {}", info);
        glDeleteProgram(shader.program);
        return INVALID_HANDLE;
    }
#endif

    auto handle = static_cast<ShaderHandle>(shaders_.size());
    shaders_.push_back(std::move(shader));
    return handle;
}

void WebGLRHI::destroy_shader(ShaderHandle handle) {
    if (handle == 0 || handle >= shaders_.size() || !shaders_[handle].alive) return;
#if WEBGL_REAL
    if (shaders_[handle].program) glDeleteProgram(shaders_[handle].program);
#endif
    shaders_[handle].alive = false;
    shaders_[handle].program = 0;
    shaders_[handle].uniform_cache.clear();
}

// ── Pipelines ───────────────────────────────────────────────────────────────

PipelineHandle WebGLRHI::create_pipeline(const PipelineDesc& desc) {
    WGLPipeline pipe;
    pipe.alive = true;
    pipe.desc = desc;

#if WEBGL_REAL
    glGenVertexArrays(1, &pipe.vao);
    glBindVertexArray(pipe.vao);
    for (const auto& attr : desc.vertex_layout.attributes) {
        glEnableVertexAttribArray(attr.location);
        glVertexAttribPointer(attr.location, static_cast<GLint>(attr.components),
                              GL_FLOAT, attr.normalized ? GL_TRUE : GL_FALSE,
                              static_cast<GLsizei>(desc.vertex_layout.stride),
                              reinterpret_cast<const void*>(static_cast<uintptr_t>(attr.offset)));
    }
    // Note: WebGL2 requires VAO to stay bound; don't unbind
#endif

    auto handle = static_cast<PipelineHandle>(pipelines_.size());
    pipelines_.push_back(std::move(pipe));
    return handle;
}

void WebGLRHI::destroy_pipeline(PipelineHandle handle) {
    if (handle == 0 || handle >= pipelines_.size() || !pipelines_[handle].alive) return;
#if WEBGL_REAL
    if (pipelines_[handle].vao) glDeleteVertexArrays(1, &pipelines_[handle].vao);
#endif
    pipelines_[handle].alive = false;
    pipelines_[handle].vao = 0;
}

// ── Framebuffers ────────────────────────────────────────────────────────────

FramebufferHandle WebGLRHI::create_framebuffer(const FramebufferDesc& desc) {
    WGLFramebuffer fb;
    fb.alive = true;
    fb.width = desc.width;
    fb.height = desc.height;
    fb.has_depth = desc.has_depth;

#if WEBGL_REAL
    glGenFramebuffers(1, &fb.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);

    // Create color attachments (owned by the framebuffer, deleted on destroy).
    for (u32 i = 0; i < static_cast<u32>(desc.color_attachments.size()); ++i) {
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, desc.width, desc.height,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i,
                               GL_TEXTURE_2D, tex, 0);
        fb.color_textures.push_back(static_cast<u32>(tex));
    }

    if (desc.has_depth) {
        GLuint depth_rb;
        glGenRenderbuffers(1, &depth_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                              desc.width, desc.height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, depth_rb);
        fb.depth_rb = static_cast<u32>(depth_rb);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif

    auto handle = static_cast<FramebufferHandle>(framebuffers_.size());
    framebuffers_.push_back(std::move(fb));
    return handle;
}

void WebGLRHI::destroy_framebuffer(FramebufferHandle handle) {
    if (handle == 0 || handle >= framebuffers_.size() || !framebuffers_[handle].alive) return;
    auto& fb = framebuffers_[handle];
#if WEBGL_REAL
    if (!fb.color_textures.empty()) {
        glDeleteTextures(static_cast<GLsizei>(fb.color_textures.size()),
                         fb.color_textures.data());
    }
    if (fb.depth_rb) glDeleteRenderbuffers(1, &fb.depth_rb);
    if (fb.fbo) glDeleteFramebuffers(1, &fb.fbo);
#endif
    fb.color_textures.clear();
    fb.depth_rb = 0;
    fb.alive = false;
    fb.fbo = 0;
}

// ── Frame ───────────────────────────────────────────────────────────────────

void WebGLRHI::begin_frame() {
    in_frame_ = true;
    draw_call_count_ = 0;
}

void WebGLRHI::end_frame() {
    in_frame_ = false;
}

// ── Viewport / Scissor / Clear ──────────────────────────────────────────────

void WebGLRHI::set_viewport(i32 x, i32 y, i32 w, i32 h) {
#if WEBGL_REAL
    glViewport(x, y, w, h);
#else
    (void)x; (void)y; (void)w; (void)h;
#endif
}

void WebGLRHI::set_scissor(i32 x, i32 y, i32 w, i32 h) {
#if WEBGL_REAL
    // Match the desktop GL backend: a non-positive rect disables scissoring,
    // otherwise enable the test and set the box.
    if (w <= 0 || h <= 0) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
#else
    (void)x; (void)y; (void)w; (void)h;
#endif
}

void WebGLRHI::clear(Vec4 color, float depth) {
#if WEBGL_REAL
    glClearColor(color.r, color.g, color.b, color.a);
    glClearDepthf(depth);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#else
    (void)color; (void)depth;
#endif
}

// ── Bind ────────────────────────────────────────────────────────────────────

void WebGLRHI::bind_pipeline(PipelineHandle handle) {
    if (handle == 0 || handle >= pipelines_.size() || !pipelines_[handle].alive) return;
#if WEBGL_REAL
    glBindVertexArray(pipelines_[handle].vao);
#endif
}

void WebGLRHI::bind_shader(ShaderHandle handle) {
    if (handle == 0 || handle >= shaders_.size() || !shaders_[handle].alive) return;
#if WEBGL_REAL
    glUseProgram(shaders_[handle].program);
#endif
}

void WebGLRHI::bind_texture(TextureHandle handle, u32 slot) {
    const bool valid = handle != 0 && handle < textures_.size() && textures_[handle].alive;
#if WEBGL_REAL
    glActiveTexture(GL_TEXTURE0 + slot);
    // Match the GL/Vulkan backends: an invalid handle unbinds the slot rather
    // than silently leaving the previously bound texture active.
    glBindTexture(GL_TEXTURE_2D, valid ? textures_[handle].gl_id : 0);
#else
    (void)slot;
    (void)valid;
#endif
}

void WebGLRHI::bind_framebuffer(FramebufferHandle handle) {
    if (handle == 0 || handle >= framebuffers_.size() || !framebuffers_[handle].alive) return;
#if WEBGL_REAL
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffers_[handle].fbo);
    glViewport(0, 0, static_cast<GLsizei>(framebuffers_[handle].width),
               static_cast<GLsizei>(framebuffers_[handle].height));
#endif
}

void WebGLRHI::unbind_framebuffer() {
#if WEBGL_REAL
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
}

void WebGLRHI::bind_vertex_buffer(BufferHandle handle) {
    if (handle == 0 || handle >= buffers_.size() || !buffers_[handle].alive) return;
#if WEBGL_REAL
    glBindBuffer(GL_ARRAY_BUFFER, buffers_[handle].gl_id);
#endif
}

void WebGLRHI::bind_index_buffer(BufferHandle handle) {
    if (handle == 0 || handle >= buffers_.size() || !buffers_[handle].alive) return;
#if WEBGL_REAL
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers_[handle].gl_id);
#endif
}

// ── State ───────────────────────────────────────────────────────────────────

void WebGLRHI::set_blend_mode(BlendMode mode) {
#if WEBGL_REAL
    if (mode == BlendMode::None) {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);
        switch (mode) {
        case BlendMode::Alpha:    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
        case BlendMode::Additive: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
        case BlendMode::Multiply: glBlendFunc(GL_DST_COLOR, GL_ZERO); break;
        default: break;
        }
    }
#else
    (void)mode;
#endif
}

void WebGLRHI::set_depth_test(bool enabled) {
#if WEBGL_REAL
    enabled ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
#else
    (void)enabled;
#endif
}

void WebGLRHI::set_depth_write(bool enabled) {
#if WEBGL_REAL
    glDepthMask(enabled ? GL_TRUE : GL_FALSE);
#else
    (void)enabled;
#endif
}

void WebGLRHI::set_cull_mode(CullMode mode) {
#if WEBGL_REAL
    if (mode == CullMode::None) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(mode == CullMode::Front ? GL_FRONT : GL_BACK);
    }
#else
    (void)mode;
#endif
}

// ── Uniforms ────────────────────────────────────────────────────────────────

#if WEBGL_REAL
static GLint get_location(WebGLRHI::WGLShader& s, const std::string& name) {
    auto it = s.uniform_cache.find(name);
    if (it != s.uniform_cache.end()) return it->second;
    GLint loc = glGetUniformLocation(s.program, name.c_str());
    s.uniform_cache[name] = loc;
    return loc;
}
#endif

void WebGLRHI::set_uniform_int(ShaderHandle shader,
                                const std::string& name, i32 value) {
    if (shader == 0 || shader >= shaders_.size() || !shaders_[shader].alive) return;
#if WEBGL_REAL
    auto& s = shaders_[shader];
    auto it = s.uniform_cache.find(name);
    GLint loc;
    if (it != s.uniform_cache.end()) {
        loc = it->second;
    } else {
        loc = glGetUniformLocation(s.program, name.c_str());
        s.uniform_cache[name] = loc;
    }
    if (loc >= 0) glUniform1i(loc, value);
#else
    (void)name; (void)value;
#endif
}

void WebGLRHI::set_uniform_int_array(ShaderHandle shader,
                                      const std::string& name,
                                      const i32* values, u32 count) {
    if (shader == 0 || shader >= shaders_.size() || !shaders_[shader].alive) return;
#if WEBGL_REAL
    auto& s = shaders_[shader];
    GLint loc = glGetUniformLocation(s.program, name.c_str());
    if (loc >= 0) glUniform1iv(loc, static_cast<GLsizei>(count), values);
#else
    (void)name; (void)values; (void)count;
#endif
}

void WebGLRHI::set_uniform_float(ShaderHandle shader,
                                  const std::string& name, float value) {
    if (shader == 0 || shader >= shaders_.size() || !shaders_[shader].alive) return;
#if WEBGL_REAL
    auto& s = shaders_[shader];
    auto it = s.uniform_cache.find(name);
    GLint loc;
    if (it != s.uniform_cache.end()) {
        loc = it->second;
    } else {
        loc = glGetUniformLocation(s.program, name.c_str());
        s.uniform_cache[name] = loc;
    }
    if (loc >= 0) glUniform1f(loc, value);
#else
    (void)name; (void)value;
#endif
}

void WebGLRHI::set_uniform_vec2(ShaderHandle shader,
                                 const std::string& name, Vec2 value) {
    if (shader == 0 || shader >= shaders_.size() || !shaders_[shader].alive) return;
#if WEBGL_REAL
    GLint loc = glGetUniformLocation(shaders_[shader].program, name.c_str());
    if (loc >= 0) glUniform2f(loc, value.x, value.y);
#else
    (void)name; (void)value;
#endif
}

void WebGLRHI::set_uniform_vec3(ShaderHandle shader,
                                 const std::string& name, Vec3 value) {
    if (shader == 0 || shader >= shaders_.size() || !shaders_[shader].alive) return;
#if WEBGL_REAL
    GLint loc = glGetUniformLocation(shaders_[shader].program, name.c_str());
    if (loc >= 0) glUniform3f(loc, value.x, value.y, value.z);
#else
    (void)name; (void)value;
#endif
}

void WebGLRHI::set_uniform_vec4(ShaderHandle shader,
                                 const std::string& name, Vec4 value) {
    if (shader == 0 || shader >= shaders_.size() || !shaders_[shader].alive) return;
#if WEBGL_REAL
    GLint loc = glGetUniformLocation(shaders_[shader].program, name.c_str());
    if (loc >= 0) glUniform4f(loc, value.x, value.y, value.z, value.w);
#else
    (void)name; (void)value;
#endif
}

void WebGLRHI::set_uniform_mat4(ShaderHandle shader,
                                 const std::string& name,
                                 const Mat4& value) {
    if (shader == 0 || shader >= shaders_.size() || !shaders_[shader].alive) return;
#if WEBGL_REAL
    GLint loc = glGetUniformLocation(shaders_[shader].program, name.c_str());
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, &value[0][0]);
#else
    (void)name; (void)value;
#endif
}

// ── Draw ────────────────────────────────────────────────────────────────────

void WebGLRHI::draw(u32 vertex_count, u32 first_vertex) {
#if WEBGL_REAL
    glDrawArrays(GL_TRIANGLES, static_cast<GLint>(first_vertex),
                 static_cast<GLsizei>(vertex_count));
#else
    (void)vertex_count; (void)first_vertex;
#endif
    ++draw_call_count_;
}

void WebGLRHI::draw_indexed(u32 index_count, u32 first_index) {
#if WEBGL_REAL
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count),
                   GL_UNSIGNED_INT,
                   reinterpret_cast<const void*>(
                       static_cast<uintptr_t>(first_index * sizeof(u32))));
#else
    (void)index_count; (void)first_index;
#endif
    ++draw_call_count_;
}

// ── Queries ─────────────────────────────────────────────────────────────────

u32 WebGLRHI::live_buffer_count() const {
    u32 count = 0;
    for (u32 i = 1; i < static_cast<u32>(buffers_.size()); ++i)
        if (buffers_[i].alive) ++count;
    return count;
}

u32 WebGLRHI::live_texture_count() const {
    u32 count = 0;
    for (u32 i = 1; i < static_cast<u32>(textures_.size()); ++i)
        if (textures_[i].alive) ++count;
    return count;
}

u32 WebGLRHI::live_shader_count() const {
    u32 count = 0;
    for (u32 i = 1; i < static_cast<u32>(shaders_.size()); ++i)
        if (shaders_[i].alive) ++count;
    return count;
}

} // namespace nexus::rhi
