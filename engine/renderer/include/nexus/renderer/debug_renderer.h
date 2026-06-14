#pragma once

#include <nexus/core/types.h>
#include <nexus/core/math.h>
#include <nexus/rhi/rhi.h>
#include <nexus/renderer/camera.h>
#include <vector>

namespace nexus {

// ─────────────────────────────────────────────────────────────────────────────
// DebugRenderer - immediate-mode 3D debug drawing (lines, boxes, spheres, grid)
// ─────────────────────────────────────────────────────────────────────────────

class DebugRenderer {
public:
    static constexpr u32 MAX_VERTICES = 65536;

    struct LineVertex {
        Vec3 position;
        Vec4 color;
    };

    DebugRenderer() = default;
    ~DebugRenderer() { shutdown(); }

    NEXUS_NON_COPYABLE(DebugRenderer)

    void init(rhi::RHI* rhi);
    void shutdown();

    void begin(const Camera3D& camera);
    void end();

    // Primitives
    void draw_line(Vec3 start, Vec3 end, Vec4 color);
    void draw_box(Vec3 center, Vec3 extents, Vec4 color);
    void draw_aabb(const AABB& aabb, Vec4 color);
    void draw_sphere(Vec3 center, float radius, Vec4 color, u32 segments = 16);
    void draw_grid(float size, float step, Vec4 color);
    void draw_axis(Vec3 origin, float length);
    void draw_frustum(const Mat4& view_projection, Vec4 color);
    void draw_ray(Vec3 origin, Vec3 direction, float length, Vec4 color);

private:
    void flush();
    void add_line(Vec3 start, Vec3 end, Vec4 color);

    rhi::RHI*         rhi_{nullptr};
    rhi::ShaderHandle shader_{rhi::INVALID_HANDLE};
    rhi::PipelineHandle pipeline_{rhi::INVALID_HANDLE};
    rhi::BufferHandle vbo_{rhi::INVALID_HANDLE};

    std::vector<LineVertex> vertices_;
    Mat4 view_projection_{1.0f};
};

} // namespace nexus
