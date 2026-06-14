#pragma once

#include "nexus/core/types.h"
#include "nexus/core/math.h"
#include "nexus/rhi/rhi.h"
#include <vector>
#include <array>

namespace nexus {

// ============================================================================
// Spherical Harmonics (order 2, 9 coefficients per channel = 27 floats)
// ============================================================================

struct SH9 {
    std::array<Vec3, 9> coefficients{};

    SH9() { coefficients.fill(Vec3(0.0f)); }

    Vec3 evaluate(Vec3 dir) const {
        // SH basis functions (order 2)
        float basis[9];
        basis[0] = 0.282095f;                                       // Y00
        basis[1] = 0.488603f * dir.y;                                // Y1-1
        basis[2] = 0.488603f * dir.z;                                // Y10
        basis[3] = 0.488603f * dir.x;                                // Y11
        basis[4] = 1.092548f * dir.x * dir.y;                       // Y2-2
        basis[5] = 1.092548f * dir.y * dir.z;                       // Y2-1
        basis[6] = 0.315392f * (3.0f * dir.z * dir.z - 1.0f);      // Y20
        basis[7] = 1.092548f * dir.x * dir.z;                       // Y21
        basis[8] = 0.546274f * (dir.x * dir.x - dir.y * dir.y);    // Y22

        Vec3 result(0.0f);
        for (u32 i = 0; i < 9; ++i) {
            result += coefficients[i] * basis[i];
        }
        return glm::max(result, Vec3(0.0f));
    }

    SH9 operator+(const SH9& other) const {
        SH9 result;
        for (u32 i = 0; i < 9; ++i)
            result.coefficients[i] = coefficients[i] + other.coefficients[i];
        return result;
    }

    SH9 operator*(float s) const {
        SH9 result;
        for (u32 i = 0; i < 9; ++i)
            result.coefficients[i] = coefficients[i] * s;
        return result;
    }
};

// ============================================================================
// LightProbe - a point in space storing SH irradiance
// ============================================================================

struct LightProbe {
    Vec3 position{0.0f};
    SH9  irradiance;
    bool valid{false};
};

// ============================================================================
// LightProbeGrid - manages a 3D grid of light probes
// ============================================================================

class LightProbeGrid {
public:
    struct Config {
        Vec3 origin{0.0f};          // grid origin (min corner)
        Vec3 extent{20.0f};         // grid size
        IVec3 resolution{4, 2, 4};  // probes per axis (X, Y, Z)
    };

    void init(rhi::RHI* rhi, const Config& config);
    void shutdown();

    /// Place probes uniformly in the grid.
    void generate_probe_positions();

    /// Bake a single probe by rendering 6 cubemap faces and projecting to SH.
    void bake_probe(u32 index);

    /// Bake all probes in the grid.
    void bake_all();

    /// Sample GI irradiance at a world position using trilinear interpolation
    /// of the 8 nearest probes.
    Vec3 sample_irradiance(Vec3 world_pos, Vec3 normal) const;

    /// Upload SH data to GPU (for shader access).
    void upload_to_gpu();

    /// Access individual probes (out-of-range returns a default probe).
    const LightProbe& probe(u32 index) const {
        static const LightProbe s_invalid{};
        return index < probes_.size() ? probes_[index] : s_invalid;
    }
    u32 probe_count() const { return static_cast<u32>(probes_.size()); }
    const Config& config() const { return config_; }

    /// Convert world position to grid-local [0,1] coordinates.
    Vec3 world_to_grid(Vec3 world_pos) const;

    /// Get the probe index from grid coordinates.
    u32 grid_index(u32 x, u32 y, u32 z) const;

private:
    rhi::RHI* rhi_{nullptr};
    Config config_;
    std::vector<LightProbe> probes_;
    rhi::BufferHandle sh_buffer_{rhi::INVALID_HANDLE};
};

} // namespace nexus
