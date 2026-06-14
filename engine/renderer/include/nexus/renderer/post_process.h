#pragma once

#include "nexus/core/types.h"
#include "nexus/core/math.h"
#include "nexus/rhi/rhi.h"
#include <vector>
#include <string>
#include <functional>
#include <memory>

namespace nexus {

// ─────────────────────────────────────────────────────────────────────────────
// PostProcessEffect - base class for post-processing effects
// ─────────────────────────────────────────────────────────────────────────────

class PostProcessEffect {
public:
    virtual ~PostProcessEffect() = default;

    virtual void init(rhi::RHI* rhi, u32 width, u32 height) = 0;
    virtual void shutdown() = 0;
    virtual void resize(u32 width, u32 height) = 0;

    /// Apply the effect. Input is the source texture, output goes to dest framebuffer.
    virtual void apply(rhi::RHI* rhi, rhi::TextureHandle input,
                       rhi::FramebufferHandle dest) = 0;

    virtual const std::string& name() const = 0;

    bool enabled{true};

protected:
    // Set by each effect's init(); used by shutdown() to free GPU resources.
    rhi::RHI* rhi_{nullptr};
};

// ─────────────────────────────────────────────────────────────────────────────
// ToneMappingEffect - ACES tone mapping + gamma correction
// ─────────────────────────────────────────────────────────────────────────────

class ToneMappingEffect : public PostProcessEffect {
public:
    enum Mode : u8 { Reinhard, ACES, Uncharted2, None };

    void init(rhi::RHI* rhi, u32 width, u32 height) override;
    void shutdown() override;
    void resize(u32 width, u32 height) override;
    void apply(rhi::RHI* rhi, rhi::TextureHandle input,
               rhi::FramebufferHandle dest) override;
    const std::string& name() const override { static std::string n = "ToneMapping"; return n; }

    float exposure{1.0f};
    float gamma{2.2f};
    Mode  mode{ACES};

private:
    rhi::ShaderHandle shader_{rhi::INVALID_HANDLE};
    rhi::PipelineHandle pipeline_{rhi::INVALID_HANDLE};
    rhi::BufferHandle quad_vbo_{rhi::INVALID_HANDLE};
};

// ─────────────────────────────────────────────────────────────────────────────
// BloomEffect - bright-pass + gaussian blur + additive blend
// ─────────────────────────────────────────────────────────────────────────────

class BloomEffect : public PostProcessEffect {
public:
    void init(rhi::RHI* rhi, u32 width, u32 height) override;
    void shutdown() override;
    void resize(u32 width, u32 height) override;
    void apply(rhi::RHI* rhi, rhi::TextureHandle input,
               rhi::FramebufferHandle dest) override;
    const std::string& name() const override { static std::string n = "Bloom"; return n; }

    float threshold{1.0f};
    float intensity{0.5f};
    u32   blur_passes{5};

private:
    rhi::ShaderHandle bright_shader_{rhi::INVALID_HANDLE};
    rhi::ShaderHandle blur_shader_{rhi::INVALID_HANDLE};
    rhi::ShaderHandle combine_shader_{rhi::INVALID_HANDLE};
    rhi::PipelineHandle pipeline_{rhi::INVALID_HANDLE};
    rhi::BufferHandle quad_vbo_{rhi::INVALID_HANDLE};
    rhi::FramebufferHandle ping_fb_{rhi::INVALID_HANDLE};
    rhi::FramebufferHandle pong_fb_{rhi::INVALID_HANDLE};
    rhi::TextureHandle ping_tex_{rhi::INVALID_HANDLE};
    rhi::TextureHandle pong_tex_{rhi::INVALID_HANDLE};
    u32 width_{0}, height_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// FXAAEffect - fast approximate anti-aliasing
// ─────────────────────────────────────────────────────────────────────────────

class FXAAEffect : public PostProcessEffect {
public:
    void init(rhi::RHI* rhi, u32 width, u32 height) override;
    void shutdown() override;
    void resize(u32 width, u32 height) override;
    void apply(rhi::RHI* rhi, rhi::TextureHandle input,
               rhi::FramebufferHandle dest) override;
    const std::string& name() const override { static std::string n = "FXAA"; return n; }

    float subpixel_quality{0.75f};
    float edge_threshold{0.125f};
    float edge_threshold_min{0.0625f};

private:
    rhi::ShaderHandle shader_{rhi::INVALID_HANDLE};
    rhi::PipelineHandle pipeline_{rhi::INVALID_HANDLE};
    rhi::BufferHandle quad_vbo_{rhi::INVALID_HANDLE};
    u32 width_{0}, height_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// VignetteEffect
// ─────────────────────────────────────────────────────────────────────────────

class VignetteEffect : public PostProcessEffect {
public:
    void init(rhi::RHI* rhi, u32 width, u32 height) override;
    void shutdown() override;
    void resize(u32 width, u32 height) override;
    void apply(rhi::RHI* rhi, rhi::TextureHandle input,
               rhi::FramebufferHandle dest) override;
    const std::string& name() const override { static std::string n = "Vignette"; return n; }

    float intensity{0.3f};
    float smoothness{0.5f};

private:
    rhi::ShaderHandle shader_{rhi::INVALID_HANDLE};
    rhi::PipelineHandle pipeline_{rhi::INVALID_HANDLE};
    rhi::BufferHandle quad_vbo_{rhi::INVALID_HANDLE};
};

// ─────────────────────────────────────────────────────────────────────────────
// PostProcessStack - manages an ordered chain of effects
// ─────────────────────────────────────────────────────────────────────────────

class PostProcessStack {
public:
    void init(rhi::RHI* rhi, u32 width, u32 height);
    void shutdown();
    void resize(u32 width, u32 height);

    /// Add an effect to the stack. Ownership is transferred.
    template <typename T, typename... Args>
    T* add_effect(Args&&... args) {
        auto effect = std::make_unique<T>(std::forward<Args>(args)...);
        auto* ptr = effect.get();
        if (rhi_) ptr->init(rhi_, width_, height_);
        effects_.push_back(std::move(effect));
        return ptr;
    }

    /// Process all enabled effects in order.
    void execute(rhi::TextureHandle scene_color, rhi::FramebufferHandle output);

    /// Get an effect by name.
    PostProcessEffect* get_effect(const std::string& name);

    /// Number of effects.
    size_t effect_count() const { return effects_.size(); }

private:
    rhi::RHI* rhi_{nullptr};
    u32 width_{0}, height_{0};
    std::vector<std::unique_ptr<PostProcessEffect>> effects_;
    rhi::FramebufferHandle intermediate_fb_{rhi::INVALID_HANDLE};
    rhi::TextureHandle intermediate_tex_{rhi::INVALID_HANDLE};
};

} // namespace nexus
