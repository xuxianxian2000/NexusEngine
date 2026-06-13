#pragma once

#include <nexus/rhi/rhi.h>
#include <vector>
#include <unordered_map>
#include <string>

namespace nexus::rhi {

// ---------------------------------------------------------------------------
// WebGLRHI — WebGL 2.0 / OpenGL ES 3.0 RHI backend
//
// Targets Emscripten/WebGL 2.0 builds. On non-Emscripten platforms, this
// backend operates as a CPU-side simulation (like VulkanRHI) for testing.
// When compiled with Emscripten, it issues real WebGL calls via the
// GLES3/gl3.h header that Emscripten provides.
//
// Differences from the desktop OpenGL backend:
//   - Uses ES 3.0 shading language (#version 300 es)
//   - No geometry/compute shaders
//   - Texture format restrictions (no RGBA32F without extension)
//   - No VAO unbinding (WebGL requires a bound VAO)
//   - Smaller uniform limits
// ---------------------------------------------------------------------------

class WebGLRHI : public RHI {
public:
    WebGLRHI() = default;
    ~WebGLRHI() override = default;

    NEXUS_NON_COPYABLE(WebGLRHI)
    NEXUS_NON_MOVABLE(WebGLRHI)

    bool init() override;
    void shutdown() override;

    // Resource creation / destruction
    BufferHandle      create_buffer(const BufferDesc& desc) override;
    void              destroy_buffer(BufferHandle handle) override;
    void              update_buffer(BufferHandle handle,
                                    const void* data, size_t size, size_t offset) override;

    TextureHandle     create_texture(const TextureDesc& desc) override;
    void              destroy_texture(TextureHandle handle) override;

    ShaderHandle      create_shader(const std::string& vertex_src,
                                    const std::string& fragment_src) override;
    void              destroy_shader(ShaderHandle handle) override;

    PipelineHandle    create_pipeline(const PipelineDesc& desc) override;
    void              destroy_pipeline(PipelineHandle handle) override;

    FramebufferHandle create_framebuffer(const FramebufferDesc& desc) override;
    void              destroy_framebuffer(FramebufferHandle handle) override;

    // Render commands
    void begin_frame() override;
    void end_frame() override;

    void set_viewport(i32 x, i32 y, i32 w, i32 h) override;
    void set_scissor(i32 x, i32 y, i32 w, i32 h) override;
    void clear(Vec4 color, float depth) override;

    void bind_pipeline(PipelineHandle handle) override;
    void bind_shader(ShaderHandle handle) override;
    void bind_texture(TextureHandle handle, u32 slot) override;
    void bind_framebuffer(FramebufferHandle handle) override;
    void unbind_framebuffer() override;
    void bind_vertex_buffer(BufferHandle handle) override;
    void bind_index_buffer(BufferHandle handle) override;

    // State toggles
    void set_blend_mode(BlendMode mode) override;
    void set_depth_test(bool enabled) override;
    void set_depth_write(bool enabled) override;
    void set_cull_mode(CullMode mode) override;

    // Uniforms
    void set_uniform_int(ShaderHandle shader,
                         const std::string& name, i32 value) override;
    void set_uniform_int_array(ShaderHandle shader,
                               const std::string& name,
                               const i32* values, u32 count) override;
    void set_uniform_float(ShaderHandle shader,
                           const std::string& name, float value) override;
    void set_uniform_vec2(ShaderHandle shader,
                          const std::string& name, Vec2 value) override;
    void set_uniform_vec3(ShaderHandle shader,
                          const std::string& name, Vec3 value) override;
    void set_uniform_vec4(ShaderHandle shader,
                          const std::string& name, Vec4 value) override;
    void set_uniform_mat4(ShaderHandle shader,
                          const std::string& name,
                          const Mat4& value) override;

    // Draw calls
    void draw(u32 vertex_count, u32 first_vertex) override;
    void draw_indexed(u32 index_count, u32 first_index) override;

    // ── WebGL-specific ──────────────────────────────────────────────────────

    /// Whether WebGL extensions are available.
    [[nodiscard]] bool has_float_textures() const { return ext_float_textures_; }
    [[nodiscard]] bool has_instancing() const { return ext_instancing_; }
    [[nodiscard]] bool is_initialized() const { return initialized_; }
    [[nodiscard]] u32 draw_call_count() const { return draw_call_count_; }
    [[nodiscard]] u32 max_texture_size() const { return max_texture_size_; }
    [[nodiscard]] u32 max_vertex_attribs() const { return max_vertex_attribs_; }

    [[nodiscard]] u32 live_buffer_count() const;
    [[nodiscard]] u32 live_texture_count() const;
    [[nodiscard]] u32 live_shader_count() const;

private:
    // CPU-side resource tracking (used on all platforms; on Emscripten,
    // also paired with real GL object IDs).
    struct WGLBuffer {
        bool alive{false};
        BufferType type{BufferType::Vertex};
        BufferUsage usage{BufferUsage::Static};
        u32 gl_id{0}; // WebGL buffer object (0 when CPU-simulated)
        size_t size{0};
    };

    struct WGLTexture {
        bool alive{false};
        u32 width{0};
        u32 height{0};
        TextureFormat format{TextureFormat::RGBA8};
        u32 gl_id{0};
    };

    struct WGLShader {
        bool alive{false};
        u32 program{0};
        std::string vertex_src;
        std::string fragment_src;
        std::unordered_map<std::string, i32> uniform_cache;
    };

    struct WGLPipeline {
        bool alive{false};
        u32 vao{0};
        PipelineDesc desc;
    };

    struct WGLFramebuffer {
        bool alive{false};
        u32 fbo{0};
        u32 width{0};
        u32 height{0};
        bool has_depth{false};
        std::vector<u32> color_textures; // color attachment textures owned by this FBO
        u32 depth_rb{0};                 // depth/stencil renderbuffer (0 if none)
    };

    std::vector<WGLBuffer>      buffers_;
    std::vector<WGLTexture>     textures_;
    std::vector<WGLShader>      shaders_;
    std::vector<WGLPipeline>    pipelines_;
    std::vector<WGLFramebuffer> framebuffers_;

    bool initialized_{false};
    bool in_frame_{false};
    u32  draw_call_count_{0};

    // WebGL capability flags
    bool ext_float_textures_{false};
    bool ext_instancing_{true}; // Part of WebGL2 core
    u32  max_texture_size_{4096};
    u32  max_vertex_attribs_{16};
};

} // namespace nexus::rhi
