#pragma once

#include <nexus/rhi/rhi.h>
#include <nexus/rhi/gl_functions.h>
#include <vector>
#include <unordered_map>
#include <string>

namespace nexus::rhi {

class OpenGLRHI : public RHI {
public:
    bool init() override;
    void shutdown() override;

    // Resource creation / destruction
    BufferHandle create_buffer(const BufferDesc& desc) override;
    void         destroy_buffer(BufferHandle handle) override;
    void         update_buffer(BufferHandle handle,
                               const void* data, size_t size, size_t offset) override;

    TextureHandle create_texture(const TextureDesc& desc) override;
    void          destroy_texture(TextureHandle handle) override;

    ShaderHandle create_shader(const std::string& vertex_src,
                               const std::string& fragment_src) override;
    void         destroy_shader(ShaderHandle handle) override;

    PipelineHandle create_pipeline(const PipelineDesc& desc) override;
    void           destroy_pipeline(PipelineHandle handle) override;

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

private:
    GLint get_uniform_loc(ShaderHandle shader, const std::string& name);

    struct GLBuffer {
        GLuint id{0};
        GLenum target{GL_ARRAY_BUFFER};
        GLenum usage{GL_STATIC_DRAW};
    };

    struct GLTexture {
        GLuint id{0};
        u32 width{0};
        u32 height{0};
    };

    struct GLShader {
        GLuint program{0};
        std::unordered_map<std::string, GLint> uniform_cache;
    };

    struct GLPipeline {
        GLuint vao{0};
        PipelineDesc desc;
    };

    struct GLFramebuffer {
        GLuint fbo{0};
        std::vector<GLuint> color_textures;
        GLuint depth_renderbuffer{0};
        u32 width{0};
        u32 height{0};
    };

    std::vector<GLBuffer>      buffers_;
    std::vector<GLTexture>     textures_;
    std::vector<GLShader>      shaders_;
    std::vector<GLPipeline>    pipelines_;
    std::vector<GLFramebuffer> framebuffers_;

    ShaderHandle bound_shader_{INVALID_HANDLE};
    GLenum       bound_primitive_{GL_TRIANGLES};
    bool         depth_write_{true}; // mirrors the GL depth write mask
};

} // namespace nexus::rhi
