#include "nexus/renderer/post_process.h"
#include "nexus/core/log.h"
#include <algorithm>

namespace nexus {

// ── Fullscreen quad vertex data ─────────────────────────────────────────────

static const float QUAD_VERTICES[] = {
    // pos(x,y), uv(u,v)
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
};

static rhi::BufferHandle create_quad_vbo(rhi::RHI* rhi) {
    rhi::BufferDesc desc;
    desc.type = rhi::BufferType::Vertex;
    desc.usage = rhi::BufferUsage::Static;
    desc.data = QUAD_VERTICES;
    desc.size = sizeof(QUAD_VERTICES);
    return rhi->create_buffer(desc);
}

static rhi::PipelineHandle create_fullscreen_pipeline(rhi::RHI* rhi,
                                                       rhi::ShaderHandle shader) {
    rhi::PipelineDesc desc;
    desc.shader = shader;
    desc.blend = rhi::BlendMode::None;
    desc.depth_test = false;
    desc.depth_write = false;
    desc.cull = rhi::CullMode::None;
    desc.primitive = rhi::PrimitiveType::Triangles;
    desc.vertex_layout.stride = sizeof(float) * 4;
    desc.vertex_layout.attributes = {
        {0, 2, 0, false},                    // position
        {1, 2, sizeof(float) * 2, false},    // texcoord
    };
    return rhi->create_pipeline(desc);
}

// ── ToneMappingEffect ────────────────────────────────────────────────────────

namespace tonemapping_shaders {

const char* VERTEX = R"(
#version 330 core
layout (location = 0) in vec2 a_Position;
layout (location = 1) in vec2 a_TexCoord;
out vec2 v_TexCoord;
void main() {
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 0.0, 1.0);
}
)";

const char* FRAGMENT = R"(
#version 330 core
in vec2 v_TexCoord;
out vec4 FragColor;

uniform sampler2D u_Input;
uniform float u_Exposure;
uniform float u_Gamma;
uniform int u_Mode; // 0=Reinhard, 1=ACES, 2=Uncharted2, 3=None

vec3 aces(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 uncharted2_helper(vec3 x) {
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

void main() {
    vec3 color = texture(u_Input, v_TexCoord).rgb;
    color *= u_Exposure;

    if (u_Mode == 0) {
        color = color / (color + vec3(1.0));
    } else if (u_Mode == 1) {
        color = aces(color);
    } else if (u_Mode == 2) {
        float W = 11.2;
        vec3 curr = uncharted2_helper(color);
        vec3 white_scale = vec3(1.0) / uncharted2_helper(vec3(W));
        color = curr * white_scale;
    }
    // Mode 3 = None (passthrough)

    // Gamma correction
    color = pow(color, vec3(1.0 / u_Gamma));
    FragColor = vec4(color, 1.0);
}
)";

} // namespace tonemapping_shaders

void ToneMappingEffect::init(rhi::RHI* rhi, u32 width, u32 height) {
    (void)width; (void)height;
    rhi_ = rhi;
    shader_ = rhi->create_shader(tonemapping_shaders::VERTEX,
                                  tonemapping_shaders::FRAGMENT);
    pipeline_ = create_fullscreen_pipeline(rhi, shader_);
    quad_vbo_ = create_quad_vbo(rhi);
}

void ToneMappingEffect::shutdown() {
    if (!rhi_) return;
    if (shader_ != rhi::INVALID_HANDLE)   rhi_->destroy_shader(shader_);
    if (pipeline_ != rhi::INVALID_HANDLE) rhi_->destroy_pipeline(pipeline_);
    if (quad_vbo_ != rhi::INVALID_HANDLE) rhi_->destroy_buffer(quad_vbo_);
    shader_ = rhi::INVALID_HANDLE;
    pipeline_ = rhi::INVALID_HANDLE;
    quad_vbo_ = rhi::INVALID_HANDLE;
    rhi_ = nullptr;
}

void ToneMappingEffect::resize(u32 width, u32 height) {
    (void)width; (void)height;
}

void ToneMappingEffect::apply(rhi::RHI* rhi, rhi::TextureHandle input,
                               rhi::FramebufferHandle dest) {
    rhi->bind_framebuffer(dest);
    rhi->bind_shader(shader_);
    rhi->bind_pipeline(pipeline_);

    rhi->bind_texture(input, 0);
    rhi->set_uniform_int(shader_, "u_Input", 0);
    rhi->set_uniform_float(shader_, "u_Exposure", exposure);
    rhi->set_uniform_float(shader_, "u_Gamma", gamma);
    rhi->set_uniform_int(shader_, "u_Mode", static_cast<i32>(mode));

    rhi->bind_vertex_buffer(quad_vbo_);
    rhi->draw(6);
}

// ── BloomEffect ──────────────────────────────────────────────────────────────

namespace bloom_shaders {

const char* VERTEX = R"(
#version 330 core
layout (location = 0) in vec2 a_Position;
layout (location = 1) in vec2 a_TexCoord;
out vec2 v_TexCoord;
void main() {
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 0.0, 1.0);
}
)";

const char* BRIGHT_PASS = R"(
#version 330 core
in vec2 v_TexCoord;
out vec4 FragColor;
uniform sampler2D u_Input;
uniform float u_Threshold;
void main() {
    vec3 color = texture(u_Input, v_TexCoord).rgb;
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    if (brightness > u_Threshold) {
        FragColor = vec4(color, 1.0);
    } else {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
    }
}
)";

const char* BLUR = R"(
#version 330 core
in vec2 v_TexCoord;
out vec4 FragColor;
uniform sampler2D u_Input;
uniform vec2 u_Direction; // (1/w, 0) or (0, 1/h)

const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main() {
    vec3 result = texture(u_Input, v_TexCoord).rgb * weights[0];
    for (int i = 1; i < 5; ++i) {
        vec2 offset = u_Direction * float(i);
        result += texture(u_Input, v_TexCoord + offset).rgb * weights[i];
        result += texture(u_Input, v_TexCoord - offset).rgb * weights[i];
    }
    FragColor = vec4(result, 1.0);
}
)";

const char* COMBINE = R"(
#version 330 core
in vec2 v_TexCoord;
out vec4 FragColor;
uniform sampler2D u_Scene;
uniform sampler2D u_Bloom;
uniform float u_Intensity;
void main() {
    vec3 scene = texture(u_Scene, v_TexCoord).rgb;
    vec3 bloom = texture(u_Bloom, v_TexCoord).rgb;
    FragColor = vec4(scene + bloom * u_Intensity, 1.0);
}
)";

} // namespace bloom_shaders

void BloomEffect::init(rhi::RHI* rhi, u32 width, u32 height) {
    rhi_ = rhi;
    width_ = width / 2;
    height_ = height / 2;

    bright_shader_ = rhi->create_shader(bloom_shaders::VERTEX, bloom_shaders::BRIGHT_PASS);
    blur_shader_ = rhi->create_shader(bloom_shaders::VERTEX, bloom_shaders::BLUR);
    combine_shader_ = rhi->create_shader(bloom_shaders::VERTEX, bloom_shaders::COMBINE);
    pipeline_ = create_fullscreen_pipeline(rhi, bright_shader_);
    quad_vbo_ = create_quad_vbo(rhi);

    // Ping-pong framebuffers at half resolution
    rhi::FramebufferDesc fb_desc;
    fb_desc.width = width_;
    fb_desc.height = height_;
    fb_desc.color_attachments = {rhi::TextureFormat::RGBA16F};
    fb_desc.has_depth = false;
    ping_fb_ = rhi->create_framebuffer(fb_desc);
    pong_fb_ = rhi->create_framebuffer(fb_desc);

    // Textures for ping-pong
    rhi::TextureDesc tex_desc;
    tex_desc.width = width_;
    tex_desc.height = height_;
    tex_desc.format = rhi::TextureFormat::RGBA16F;
    tex_desc.generate_mipmaps = false;
    ping_tex_ = rhi->create_texture(tex_desc);
    pong_tex_ = rhi->create_texture(tex_desc);
}

void BloomEffect::shutdown() {
    if (!rhi_) return;
    if (bright_shader_ != rhi::INVALID_HANDLE)  rhi_->destroy_shader(bright_shader_);
    if (blur_shader_ != rhi::INVALID_HANDLE)    rhi_->destroy_shader(blur_shader_);
    if (combine_shader_ != rhi::INVALID_HANDLE) rhi_->destroy_shader(combine_shader_);
    if (pipeline_ != rhi::INVALID_HANDLE)       rhi_->destroy_pipeline(pipeline_);
    if (quad_vbo_ != rhi::INVALID_HANDLE)       rhi_->destroy_buffer(quad_vbo_);
    if (ping_fb_ != rhi::INVALID_HANDLE)        rhi_->destroy_framebuffer(ping_fb_);
    if (pong_fb_ != rhi::INVALID_HANDLE)        rhi_->destroy_framebuffer(pong_fb_);
    if (ping_tex_ != rhi::INVALID_HANDLE)       rhi_->destroy_texture(ping_tex_);
    if (pong_tex_ != rhi::INVALID_HANDLE)       rhi_->destroy_texture(pong_tex_);
    bright_shader_ = blur_shader_ = combine_shader_ = rhi::INVALID_HANDLE;
    pipeline_ = rhi::INVALID_HANDLE;
    quad_vbo_ = rhi::INVALID_HANDLE;
    ping_fb_ = pong_fb_ = rhi::INVALID_HANDLE;
    ping_tex_ = pong_tex_ = rhi::INVALID_HANDLE;
    rhi_ = nullptr;
}

void BloomEffect::resize(u32 width, u32 height) {
    width_ = width / 2;
    height_ = height / 2;
}

void BloomEffect::apply(rhi::RHI* rhi, rhi::TextureHandle input,
                          rhi::FramebufferHandle dest) {
    // Bright pass → ping
    rhi->bind_framebuffer(ping_fb_);
    rhi->set_viewport(0, 0, static_cast<i32>(width_), static_cast<i32>(height_));
    rhi->bind_shader(bright_shader_);
    rhi->bind_pipeline(pipeline_);
    rhi->bind_texture(input, 0);
    rhi->set_uniform_int(bright_shader_, "u_Input", 0);
    rhi->set_uniform_float(bright_shader_, "u_Threshold", threshold);
    rhi->bind_vertex_buffer(quad_vbo_);
    rhi->draw(6);

    // Gaussian blur passes (ping-pong)
    for (u32 i = 0; i < blur_passes; ++i) {
        // Horizontal blur: ping → pong
        rhi->bind_framebuffer(pong_fb_);
        rhi->bind_shader(blur_shader_);
        rhi->bind_texture(ping_tex_, 0);
        rhi->set_uniform_int(blur_shader_, "u_Input", 0);
        rhi->set_uniform_vec2(blur_shader_, "u_Direction",
                               Vec2(1.0f / static_cast<float>(width_), 0.0f));
        rhi->bind_vertex_buffer(quad_vbo_);
        rhi->draw(6);

        // Vertical blur: pong → ping
        rhi->bind_framebuffer(ping_fb_);
        rhi->bind_shader(blur_shader_);
        rhi->bind_texture(pong_tex_, 0);
        rhi->set_uniform_int(blur_shader_, "u_Input", 0);
        rhi->set_uniform_vec2(blur_shader_, "u_Direction",
                               Vec2(0.0f, 1.0f / static_cast<float>(height_)));
        rhi->bind_vertex_buffer(quad_vbo_);
        rhi->draw(6);
    }

    // Combine: scene + bloom → dest
    rhi->bind_framebuffer(dest);
    rhi->bind_shader(combine_shader_);
    rhi->bind_texture(input, 0);
    rhi->set_uniform_int(combine_shader_, "u_Scene", 0);
    rhi->bind_texture(ping_tex_, 1);
    rhi->set_uniform_int(combine_shader_, "u_Bloom", 1);
    rhi->set_uniform_float(combine_shader_, "u_Intensity", intensity);
    rhi->bind_vertex_buffer(quad_vbo_);
    rhi->draw(6);
}

// ── FXAAEffect ───────────────────────────────────────────────────────────────

namespace fxaa_shaders {

const char* VERTEX = R"(
#version 330 core
layout (location = 0) in vec2 a_Position;
layout (location = 1) in vec2 a_TexCoord;
out vec2 v_TexCoord;
void main() {
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 0.0, 1.0);
}
)";

const char* FRAGMENT = R"(
#version 330 core
in vec2 v_TexCoord;
out vec4 FragColor;

uniform sampler2D u_Input;
uniform vec2 u_InverseScreenSize;
uniform float u_SubpixelQuality;
uniform float u_EdgeThreshold;
uniform float u_EdgeThresholdMin;

float luminance(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec2 uv = v_TexCoord;
    vec3 rgbM = texture(u_Input, uv).rgb;
    float lumaM = luminance(rgbM);

    float lumaS = luminance(texture(u_Input, uv + vec2(0, u_InverseScreenSize.y)).rgb);
    float lumaN = luminance(texture(u_Input, uv - vec2(0, u_InverseScreenSize.y)).rgb);
    float lumaE = luminance(texture(u_Input, uv + vec2(u_InverseScreenSize.x, 0)).rgb);
    float lumaW = luminance(texture(u_Input, uv - vec2(u_InverseScreenSize.x, 0)).rgb);

    float lumaMin = min(lumaM, min(min(lumaS, lumaN), min(lumaE, lumaW)));
    float lumaMax = max(lumaM, max(max(lumaS, lumaN), max(lumaE, lumaW)));
    float lumaRange = lumaMax - lumaMin;

    if (lumaRange < max(u_EdgeThresholdMin, lumaMax * u_EdgeThreshold)) {
        FragColor = vec4(rgbM, 1.0);
        return;
    }

    float lumaAvg = (lumaN + lumaS + lumaE + lumaW) * 0.25;
    float subpixel = clamp(abs(lumaAvg - lumaM) / lumaRange, 0.0, 1.0);
    subpixel = smoothstep(0.0, 1.0, subpixel);
    subpixel = subpixel * subpixel * u_SubpixelQuality;

    bool isHorizontal = abs(lumaN + lumaS - 2.0 * lumaM) >=
                        abs(lumaE + lumaW - 2.0 * lumaM);

    float stepLength = isHorizontal ? u_InverseScreenSize.y : u_InverseScreenSize.x;
    float gradientPos = isHorizontal ? abs(lumaN - lumaM) : abs(lumaE - lumaM);
    float gradientNeg = isHorizontal ? abs(lumaS - lumaM) : abs(lumaW - lumaM);

    if (gradientNeg > gradientPos) stepLength = -stepLength;

    vec2 offset = isHorizontal ? vec2(0.0, stepLength * 0.5) : vec2(stepLength * 0.5, 0.0);
    vec3 result = texture(u_Input, uv + offset).rgb;

    FragColor = vec4(mix(result, rgbM, 1.0 - subpixel), 1.0);
}
)";

} // namespace fxaa_shaders

void FXAAEffect::init(rhi::RHI* rhi, u32 width, u32 height) {
    rhi_ = rhi;
    width_ = width;
    height_ = height;
    shader_ = rhi->create_shader(fxaa_shaders::VERTEX, fxaa_shaders::FRAGMENT);
    pipeline_ = create_fullscreen_pipeline(rhi, shader_);
    quad_vbo_ = create_quad_vbo(rhi);
}

void FXAAEffect::shutdown() {
    if (!rhi_) return;
    if (shader_ != rhi::INVALID_HANDLE)   rhi_->destroy_shader(shader_);
    if (pipeline_ != rhi::INVALID_HANDLE) rhi_->destroy_pipeline(pipeline_);
    if (quad_vbo_ != rhi::INVALID_HANDLE) rhi_->destroy_buffer(quad_vbo_);
    shader_ = rhi::INVALID_HANDLE;
    pipeline_ = rhi::INVALID_HANDLE;
    quad_vbo_ = rhi::INVALID_HANDLE;
    rhi_ = nullptr;
}

void FXAAEffect::resize(u32 width, u32 height) {
    width_ = width;
    height_ = height;
}

void FXAAEffect::apply(rhi::RHI* rhi, rhi::TextureHandle input,
                         rhi::FramebufferHandle dest) {
    rhi->bind_framebuffer(dest);
    rhi->bind_shader(shader_);
    rhi->bind_pipeline(pipeline_);

    rhi->bind_texture(input, 0);
    rhi->set_uniform_int(shader_, "u_Input", 0);
    rhi->set_uniform_vec2(shader_, "u_InverseScreenSize",
                           Vec2(1.0f / static_cast<float>(width_),
                                1.0f / static_cast<float>(height_)));
    rhi->set_uniform_float(shader_, "u_SubpixelQuality", subpixel_quality);
    rhi->set_uniform_float(shader_, "u_EdgeThreshold", edge_threshold);
    rhi->set_uniform_float(shader_, "u_EdgeThresholdMin", edge_threshold_min);

    rhi->bind_vertex_buffer(quad_vbo_);
    rhi->draw(6);
}

// ── VignetteEffect ──────────────────────────────────────────────────────────

namespace vignette_shaders {

const char* VERTEX = R"(
#version 330 core
layout (location = 0) in vec2 a_Position;
layout (location = 1) in vec2 a_TexCoord;
out vec2 v_TexCoord;
void main() {
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 0.0, 1.0);
}
)";

const char* FRAGMENT = R"(
#version 330 core
in vec2 v_TexCoord;
out vec4 FragColor;
uniform sampler2D u_Input;
uniform float u_Intensity;
uniform float u_Smoothness;
void main() {
    vec3 color = texture(u_Input, v_TexCoord).rgb;
    vec2 uv = v_TexCoord * 2.0 - 1.0;
    float dist = length(uv);
    float vignette = 1.0 - smoothstep(1.0 - u_Smoothness, 1.0, dist * u_Intensity);
    FragColor = vec4(color * vignette, 1.0);
}
)";

} // namespace vignette_shaders

void VignetteEffect::init(rhi::RHI* rhi, u32 width, u32 height) {
    (void)width; (void)height;
    rhi_ = rhi;
    shader_ = rhi->create_shader(vignette_shaders::VERTEX, vignette_shaders::FRAGMENT);
    pipeline_ = create_fullscreen_pipeline(rhi, shader_);
    quad_vbo_ = create_quad_vbo(rhi);
}

void VignetteEffect::shutdown() {
    if (!rhi_) return;
    if (shader_ != rhi::INVALID_HANDLE)   rhi_->destroy_shader(shader_);
    if (pipeline_ != rhi::INVALID_HANDLE) rhi_->destroy_pipeline(pipeline_);
    if (quad_vbo_ != rhi::INVALID_HANDLE) rhi_->destroy_buffer(quad_vbo_);
    shader_ = rhi::INVALID_HANDLE;
    pipeline_ = rhi::INVALID_HANDLE;
    quad_vbo_ = rhi::INVALID_HANDLE;
    rhi_ = nullptr;
}

void VignetteEffect::resize(u32 width, u32 height) {
    (void)width; (void)height;
}

void VignetteEffect::apply(rhi::RHI* rhi, rhi::TextureHandle input,
                             rhi::FramebufferHandle dest) {
    rhi->bind_framebuffer(dest);
    rhi->bind_shader(shader_);
    rhi->bind_pipeline(pipeline_);

    rhi->bind_texture(input, 0);
    rhi->set_uniform_int(shader_, "u_Input", 0);
    rhi->set_uniform_float(shader_, "u_Intensity", intensity);
    rhi->set_uniform_float(shader_, "u_Smoothness", smoothness);

    rhi->bind_vertex_buffer(quad_vbo_);
    rhi->draw(6);
}

// ── PostProcessStack ─────────────────────────────────────────────────────────

void PostProcessStack::init(rhi::RHI* rhi, u32 width, u32 height) {
    rhi_ = rhi;
    width_ = width;
    height_ = height;

    // Create intermediate framebuffer/texture for chaining effects
    rhi::TextureDesc tex_desc;
    tex_desc.width = width;
    tex_desc.height = height;
    tex_desc.format = rhi::TextureFormat::RGBA16F;
    tex_desc.generate_mipmaps = false;
    intermediate_tex_ = rhi->create_texture(tex_desc);

    rhi::FramebufferDesc fb_desc;
    fb_desc.width = width;
    fb_desc.height = height;
    fb_desc.color_attachments = {rhi::TextureFormat::RGBA16F};
    fb_desc.has_depth = false;
    intermediate_fb_ = rhi->create_framebuffer(fb_desc);
}

void PostProcessStack::shutdown() {
    for (auto& effect : effects_) {
        effect->shutdown();
    }
    effects_.clear();
    if (rhi_) {
        rhi_->destroy_framebuffer(intermediate_fb_);
        rhi_->destroy_texture(intermediate_tex_);
    }
}

void PostProcessStack::resize(u32 width, u32 height) {
    width_ = width;
    height_ = height;
    for (auto& effect : effects_) {
        effect->resize(width, height);
    }
}

void PostProcessStack::execute(rhi::TextureHandle scene_color,
                                rhi::FramebufferHandle output) {
    // Collect enabled effects
    std::vector<PostProcessEffect*> active;
    for (auto& effect : effects_) {
        if (effect->enabled) {
            active.push_back(effect.get());
        }
    }

    if (active.empty()) return;

    // Single effect: input → output directly
    if (active.size() == 1) {
        active[0]->apply(rhi_, scene_color, output);
        return;
    }

    // Multiple effects: chain through intermediate buffer
    rhi::TextureHandle current_input = scene_color;
    for (size_t i = 0; i < active.size(); ++i) {
        bool is_last = (i == active.size() - 1);
        rhi::FramebufferHandle dest = is_last ? output : intermediate_fb_;
        active[i]->apply(rhi_, current_input, dest);
        if (!is_last) {
            current_input = intermediate_tex_;
        }
    }
}

PostProcessEffect* PostProcessStack::get_effect(const std::string& name) {
    for (auto& effect : effects_) {
        if (effect->name() == name) {
            return effect.get();
        }
    }
    return nullptr;
}

} // namespace nexus
