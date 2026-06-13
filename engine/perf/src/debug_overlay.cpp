#include "nexus/perf/debug_overlay.h"
#include "nexus/perf/profiler.h"
#include "nexus/renderer/batch_renderer_2d.h"
#include "nexus/ui/bitmap_font.h"
#include "nexus/rhi/rhi_types.h"

#include <cstdio>

namespace nexus {

DebugOverlay::DebugOverlay(Profiler* profiler, BatchRenderer2D* renderer, ui::BitmapFont* font)
    : profiler_(profiler)
    , renderer_(renderer)
    , font_(font)
{
}

void DebugOverlay::render(int screen_width, int screen_height) {
    if (!profiler_ || !renderer_ || !font_ || !font_->is_valid()) {
        return;
    }

    // Set up a screen-space orthographic camera (origin at top-left).
    screen_camera_.position = {0.0f, 0.0f};
    screen_camera_.rotation = 0.0f;
    screen_camera_.zoom     = 1.0f;
    screen_camera_.set_projection(static_cast<float>(screen_width),
                                  static_cast<float>(screen_height));

    renderer_->begin(screen_camera_);

    // Collect stats to display.
    f64 avg_cpu_us   = profiler_->average_cpu_us();
    f64 fps          = (avg_cpu_us > 0.0) ? 1'000'000.0 / avg_cpu_us : 0.0;
    f64 frame_ms     = avg_cpu_us / 1'000.0;
    u32 draw_calls   = renderer_->get_stats().draw_calls;

    // Build text lines.
    struct Line { std::string text; };
    std::vector<Line> lines;

    char buf[128];

    if (show_fps) {
        std::snprintf(buf, sizeof(buf), "FPS: %.1f", fps);
        lines.push_back({buf});
    }

    if (show_frame_time) {
        std::snprintf(buf, sizeof(buf), "Frame: %.2f ms", frame_ms);
        lines.push_back({buf});
    }

    if (show_draw_calls) {
        std::snprintf(buf, sizeof(buf), "Draw Calls: %u", draw_calls);
        lines.push_back({buf});
    }

    if (lines.empty()) {
        renderer_->end();
        return;
    }

    // Measure text to compute background size.
    float max_width  = 0.0f;
    float total_height = 0.0f;
    float line_h = font_->line_height();

    for (auto& l : lines) {
        Vec2 size = font_->measure_text(l.text);
        if (size.x > max_width) max_width = size.x;
    }
    total_height = static_cast<float>(lines.size()) * line_h
                 + static_cast<float>(lines.size() - 1) * line_spacing;

    // Draw semi-transparent background quad.
    float bg_w = max_width + padding * 2.0f;
    float bg_h = total_height + padding * 2.0f;
    Vec2  bg_pos{padding, padding};   // top-left corner of overlay
    Vec2  bg_center{bg_pos.x + bg_w * 0.5f, bg_pos.y + bg_h * 0.5f};

    renderer_->draw_quad(bg_center, {bg_w, bg_h}, background_color);

    // Render each line of text.
    float cursor_y = bg_pos.y + padding;
    for (auto& l : lines) {
        render_text(l.text, {bg_pos.x + padding, cursor_y});
        cursor_y += line_h + line_spacing;
    }

    renderer_->end();
}

void DebugOverlay::render_text(const std::string& text, Vec2 position) {
    if (!font_ || text.empty()) return;

    auto vertices = font_->generate_vertices(text, position);
    rhi::TextureHandle atlas = static_cast<rhi::TextureHandle>(font_texture_);

    // Each glyph has 4 vertices; render as individual quads.
    for (size_t i = 0; i + 3 < vertices.size(); i += 4) {
        auto& v0 = vertices[i];
        auto& v1 = vertices[i + 1];
        auto& v2 = vertices[i + 2];
        auto& v3 = vertices[i + 3];

        Vec2 glyph_min{v0.x, v0.y};
        Vec2 glyph_max{v2.x, v2.y};
        Vec2 glyph_size = glyph_max - glyph_min;
        Vec2 glyph_center = glyph_min + glyph_size * 0.5f;

        Vec2 uv_min{v0.u, v0.v};
        Vec2 uv_max{v2.u, v2.v};

        renderer_->draw_quad(glyph_center, glyph_size, 0.0f,
                             atlas, text_color, uv_min, uv_max);
    }
}

} // namespace nexus
