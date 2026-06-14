#include "nexus/renderer/light_probe.h"
#include "nexus/core/log.h"
#include <cmath>
#include <algorithm>

namespace nexus {

// ── LightProbeGrid ─────────────────────────────────────────────────────────

void LightProbeGrid::init(rhi::RHI* rhi, const Config& config) {
    rhi_ = rhi;
    config_ = config;
    // resolution is signed; a zero/negative axis would cast to a huge u32 and
    // request a massive allocation (and underflow the clamps in sample_irradiance).
    constexpr i32 kMaxAxis = 256;
    config_.resolution.x = std::clamp(config_.resolution.x, 1, kMaxAxis);
    config_.resolution.y = std::clamp(config_.resolution.y, 1, kMaxAxis);
    config_.resolution.z = std::clamp(config_.resolution.z, 1, kMaxAxis);
    generate_probe_positions();
    NX_INFO("LightProbeGrid initialized: {}x{}x{} = {} probes",
            config_.resolution.x, config_.resolution.y, config_.resolution.z,
            probe_count());
}

void LightProbeGrid::shutdown() {
    if (!rhi_) return;
    if (sh_buffer_ != rhi::INVALID_HANDLE) {
        rhi_->destroy_buffer(sh_buffer_);
        sh_buffer_ = rhi::INVALID_HANDLE;
    }
    probes_.clear();
    rhi_ = nullptr;
}

void LightProbeGrid::generate_probe_positions() {
    u32 total = static_cast<u32>(config_.resolution.x) *
                static_cast<u32>(config_.resolution.y) *
                static_cast<u32>(config_.resolution.z);
    probes_.resize(total);

    Vec3 step;
    step.x = (config_.resolution.x > 1)
        ? config_.extent.x / static_cast<float>(config_.resolution.x - 1) : 0.0f;
    step.y = (config_.resolution.y > 1)
        ? config_.extent.y / static_cast<float>(config_.resolution.y - 1) : 0.0f;
    step.z = (config_.resolution.z > 1)
        ? config_.extent.z / static_cast<float>(config_.resolution.z - 1) : 0.0f;

    for (i32 z = 0; z < config_.resolution.z; ++z) {
        for (i32 y = 0; y < config_.resolution.y; ++y) {
            for (i32 x = 0; x < config_.resolution.x; ++x) {
                u32 idx = grid_index(static_cast<u32>(x), static_cast<u32>(y), static_cast<u32>(z));
                probes_[idx].position = config_.origin + Vec3(
                    static_cast<float>(x) * step.x,
                    static_cast<float>(y) * step.y,
                    static_cast<float>(z) * step.z
                );
                probes_[idx].valid = false;
            }
        }
    }
}

void LightProbeGrid::bake_probe(u32 index) {
    if (index >= probes_.size() || !rhi_) return;

    auto& probe = probes_[index];

    // Project environment into SH by sampling cubemap faces.
    // For each face direction, evaluate the SH basis and accumulate.
    // In production this would render 6 faces to a cubemap, then integrate.
    // Here we use an analytical approximation for the irradiance projection.

    // 6 cubemap face directions + up vectors
    static const Vec3 face_dirs[6] = {
        { 1, 0, 0}, {-1, 0, 0}, { 0, 1, 0}, { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1}
    };

    // Monte Carlo integration over hemisphere using 64 samples per face
    SH9 sh;
    constexpr u32 GRID = 8; // 8x8 = 64
    float weight_sum = 0.0f;

    for (u32 face = 0; face < 6; ++face) {
        Vec3 forward = face_dirs[face];
        Vec3 up = (std::abs(forward.y) > 0.9f) ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
        Vec3 right = glm::normalize(glm::cross(forward, up));
        up = glm::cross(right, forward);

        for (u32 j = 0; j < GRID; ++j) {
            for (u32 i = 0; i < GRID; ++i) {
                // UV in [-1, 1]
                float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(GRID) * 2.0f - 1.0f;
                float v = (static_cast<float>(j) + 0.5f) / static_cast<float>(GRID) * 2.0f - 1.0f;

                Vec3 dir = glm::normalize(forward + right * u + up * v);

                // Solid angle weight (cubemap texel area correction)
                float tmp = 1.0f + u * u + v * v;
                float weight = 4.0f / (std::sqrt(tmp) * tmp);

                // Sample environment color (sky contribution approximation)
                // In production, this reads from a rendered cubemap.
                // Here we use a simple gradient sky for default baking.
                float sky_factor = glm::clamp(dir.y * 0.5f + 0.5f, 0.0f, 1.0f);
                Vec3 sample_color = glm::mix(Vec3(0.3f, 0.25f, 0.2f),  // ground
                                             Vec3(0.5f, 0.7f, 1.0f),   // sky
                                             sky_factor);

                // Project into SH
                float basis[9];
                basis[0] = 0.282095f;
                basis[1] = 0.488603f * dir.y;
                basis[2] = 0.488603f * dir.z;
                basis[3] = 0.488603f * dir.x;
                basis[4] = 1.092548f * dir.x * dir.y;
                basis[5] = 1.092548f * dir.y * dir.z;
                basis[6] = 0.315392f * (3.0f * dir.z * dir.z - 1.0f);
                basis[7] = 1.092548f * dir.x * dir.z;
                basis[8] = 0.546274f * (dir.x * dir.x - dir.y * dir.y);

                for (u32 k = 0; k < 9; ++k) {
                    sh.coefficients[k] += sample_color * basis[k] * weight;
                }
                weight_sum += weight;
            }
        }
    }

    // Normalize
    if (weight_sum > 0.0f) {
        float norm = 4.0f * 3.14159265f / weight_sum;
        for (u32 k = 0; k < 9; ++k) {
            sh.coefficients[k] *= norm;
        }
    }

    probe.irradiance = sh;
    probe.valid = true;
}

void LightProbeGrid::bake_all() {
    for (u32 i = 0; i < probe_count(); ++i) {
        bake_probe(i);
    }
    NX_INFO("Baked {} light probes", probe_count());
}

Vec3 LightProbeGrid::world_to_grid(Vec3 world_pos) const {
    Vec3 local = world_pos - config_.origin;
    Vec3 norm;
    norm.x = (config_.extent.x > 0.0f) ? local.x / config_.extent.x : 0.0f;
    norm.y = (config_.extent.y > 0.0f) ? local.y / config_.extent.y : 0.0f;
    norm.z = (config_.extent.z > 0.0f) ? local.z / config_.extent.z : 0.0f;
    return norm;
}

u32 LightProbeGrid::grid_index(u32 x, u32 y, u32 z) const {
    return z * static_cast<u32>(config_.resolution.x) * static_cast<u32>(config_.resolution.y)
         + y * static_cast<u32>(config_.resolution.x)
         + x;
}

Vec3 LightProbeGrid::sample_irradiance(Vec3 world_pos, Vec3 normal) const {
    Vec3 grid_pos = world_to_grid(world_pos);

    // Clamp to grid bounds
    grid_pos = glm::clamp(grid_pos, Vec3(0.0f), Vec3(1.0f));

    // Convert to fractional grid coordinates
    float fx = grid_pos.x * static_cast<float>(config_.resolution.x - 1);
    float fy = grid_pos.y * static_cast<float>(config_.resolution.y - 1);
    float fz = grid_pos.z * static_cast<float>(config_.resolution.z - 1);

    u32 x0 = static_cast<u32>(std::floor(fx));
    u32 y0 = static_cast<u32>(std::floor(fy));
    u32 z0 = static_cast<u32>(std::floor(fz));
    u32 x1 = std::min(x0 + 1, static_cast<u32>(config_.resolution.x - 1));
    u32 y1 = std::min(y0 + 1, static_cast<u32>(config_.resolution.y - 1));
    u32 z1 = std::min(z0 + 1, static_cast<u32>(config_.resolution.z - 1));

    float tx = fx - static_cast<float>(x0);
    float ty = fy - static_cast<float>(y0);
    float tz = fz - static_cast<float>(z0);

    // Trilinear interpolation of 8 surrounding probes
    auto sample_probe = [&](u32 x, u32 y, u32 z) -> Vec3 {
        u32 idx = grid_index(x, y, z);
        if (idx >= probes_.size() || !probes_[idx].valid) return Vec3(0.0f);
        return probes_[idx].irradiance.evaluate(normal);
    };

    Vec3 c000 = sample_probe(x0, y0, z0);
    Vec3 c100 = sample_probe(x1, y0, z0);
    Vec3 c010 = sample_probe(x0, y1, z0);
    Vec3 c110 = sample_probe(x1, y1, z0);
    Vec3 c001 = sample_probe(x0, y0, z1);
    Vec3 c101 = sample_probe(x1, y0, z1);
    Vec3 c011 = sample_probe(x0, y1, z1);
    Vec3 c111 = sample_probe(x1, y1, z1);

    Vec3 c00 = glm::mix(c000, c100, tx);
    Vec3 c10 = glm::mix(c010, c110, tx);
    Vec3 c01 = glm::mix(c001, c101, tx);
    Vec3 c11 = glm::mix(c011, c111, tx);

    Vec3 c0 = glm::mix(c00, c10, ty);
    Vec3 c1 = glm::mix(c01, c11, ty);

    return glm::mix(c0, c1, tz);
}

void LightProbeGrid::upload_to_gpu() {
    if (!rhi_ || probes_.empty()) return;

    // Pack SH data: 9 Vec3 per probe = 27 floats per probe
    std::vector<float> data(probes_.size() * 27);
    for (u32 i = 0; i < probes_.size(); ++i) {
        for (u32 k = 0; k < 9; ++k) {
            data[i * 27 + k * 3 + 0] = probes_[i].irradiance.coefficients[k].x;
            data[i * 27 + k * 3 + 1] = probes_[i].irradiance.coefficients[k].y;
            data[i * 27 + k * 3 + 2] = probes_[i].irradiance.coefficients[k].z;
        }
    }

    if (sh_buffer_ != rhi::INVALID_HANDLE) {
        rhi_->destroy_buffer(sh_buffer_);
    }

    rhi::BufferDesc desc;
    desc.size = static_cast<u32>(data.size() * sizeof(float));
    desc.usage = rhi::BufferUsage::Dynamic;
    desc.data = data.data();
    sh_buffer_ = rhi_->create_buffer(desc);
}

} // namespace nexus
