#include "nexus/renderer/tilemap_renderer.h"
#include <algorithm>
#include <cmath>

namespace nexus {

void TilemapRenderer::render(BatchRenderer2D& renderer, const Camera2D& camera,
                              Vec2 screen_size, const TilemapData& tilemap) {
    if (!tilemap.tiles || tilemap.width == 0 || tilemap.height == 0) return;
    // tiles_per_row/col feed integer modulo and 1/x below; a zero would be a
    // divide-by-zero (UB) and infinite UVs.
    if (tilemap.tiles_per_row == 0 || tilemap.tiles_per_col == 0) return;

    float ts = tilemap.tile_size;
    float inv_zoom = 1.0f / std::max(camera.zoom, 0.001f);

    // Compute visible tile range from camera
    float half_w = screen_size.x * 0.5f * inv_zoom;
    float half_h = screen_size.y * 0.5f * inv_zoom;

    float view_min_x = camera.position.x - half_w;
    float view_min_y = camera.position.y - half_h;
    float view_max_x = camera.position.x + half_w;
    float view_max_y = camera.position.y + half_h;

    // Convert to tile coordinates (add 1 tile margin for safety)
    i32 min_tx = static_cast<i32>(std::floor((view_min_x - tilemap.offset.x) / ts)) - 1;
    i32 min_ty = static_cast<i32>(std::floor((view_min_y - tilemap.offset.y) / ts)) - 1;
    i32 max_tx = static_cast<i32>(std::ceil((view_max_x - tilemap.offset.x) / ts)) + 1;
    i32 max_ty = static_cast<i32>(std::ceil((view_max_y - tilemap.offset.y) / ts)) + 1;

    // Clamp to tilemap bounds
    min_tx = std::max(min_tx, 0);
    min_ty = std::max(min_ty, 0);
    max_tx = std::min(max_tx, static_cast<i32>(tilemap.width));
    max_ty = std::min(max_ty, static_cast<i32>(tilemap.height));

    float uv_w = 1.0f / static_cast<float>(tilemap.tiles_per_row);
    float uv_h = 1.0f / static_cast<float>(tilemap.tiles_per_col);

    for (i32 ty = min_ty; ty < max_ty; ++ty) {
        for (i32 tx = min_tx; tx < max_tx; ++tx) {
            i32 tile_id = tilemap.tiles[static_cast<u32>(ty) * tilemap.width + static_cast<u32>(tx)];
            if (tile_id < 0) continue;

            // Compute UV from tile index in atlas
            u32 atlas_x = static_cast<u32>(tile_id) % tilemap.tiles_per_row;
            u32 atlas_y = static_cast<u32>(tile_id) / tilemap.tiles_per_row;

            Vec2 uv_min{static_cast<float>(atlas_x) * uv_w,
                        static_cast<float>(atlas_y) * uv_h};
            Vec2 uv_max{uv_min.x + uv_w, uv_min.y + uv_h};

            Vec2 pos{
                tilemap.offset.x + static_cast<float>(tx) * ts + ts * 0.5f,
                tilemap.offset.y + static_cast<float>(ty) * ts + ts * 0.5f
            };

            renderer.draw_quad(pos, Vec2{ts, ts}, 0.0f,
                              tilemap.texture, tilemap.tint, uv_min, uv_max);
        }
    }
}

} // namespace nexus
