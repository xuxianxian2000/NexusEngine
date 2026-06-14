#pragma once

#include "nexus/core/types.h"
#include "nexus/core/math.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cmath>

namespace nexus::physics {

// ─────────────────────────────────────────────────────────────────────────────
// SpatialHash3D - uniform grid spatial partitioning for 3D broadphase
// ─────────────────────────────────────────────────────────────────────────────

class SpatialHash3D {
public:
    explicit SpatialHash3D(float cell_size = 2.0f)
        : cell_size_(cell_size), inv_cell_size_(1.0f / cell_size) {}

    void clear() { cells_.clear(); }

    void set_cell_size(float size) {
        cell_size_ = size;
        inv_cell_size_ = 1.0f / size;
    }

    /// Insert a body ID with its AABB.
    void insert(u32 id, const AABB& aabb) {
        i32 x0 = cell(aabb.min.x), y0 = cell(aabb.min.y), z0 = cell(aabb.min.z);
        i32 x1 = cell(aabb.max.x), y1 = cell(aabb.max.y), z1 = cell(aabb.max.z);

        for (i32 x = x0; x <= x1; ++x)
            for (i32 y = y0; y <= y1; ++y)
                for (i32 z = z0; z <= z1; ++z)
                    cells_[hash_key(x, y, z)].push_back(id);
    }

    /// Query all IDs that share cells with the given AABB.
    void query(const AABB& aabb, std::unordered_set<u64>& pairs, u32 self_id,
               std::vector<std::pair<u32, u32>>& out) const {
        i32 x0 = cell(aabb.min.x), y0 = cell(aabb.min.y), z0 = cell(aabb.min.z);
        i32 x1 = cell(aabb.max.x), y1 = cell(aabb.max.y), z1 = cell(aabb.max.z);

        for (i32 x = x0; x <= x1; ++x) {
            for (i32 y = y0; y <= y1; ++y) {
                for (i32 z = z0; z <= z1; ++z) {
                    auto it = cells_.find(hash_key(x, y, z));
                    if (it == cells_.end()) continue;
                    for (u32 other_id : it->second) {
                        if (other_id == self_id) continue;
                        u32 lo = self_id < other_id ? self_id : other_id;
                        u32 hi = self_id < other_id ? other_id : self_id;
                        u64 pair_key = (static_cast<u64>(lo) << 32) | hi;
                        if (pairs.insert(pair_key).second) {
                            out.emplace_back(lo, hi);
                        }
                    }
                }
            }
        }
    }

private:
    i32 cell(float v) const { return static_cast<i32>(std::floor(v * inv_cell_size_)); }

    static u64 hash_key(i32 x, i32 y, i32 z) {
        // Losslessly pack three 21-bit cell coordinates so distinct cells never
        // collide. The previous multiplicative-XOR hash aliased different cells
        // into the same bucket, inflating the candidate-pair set.
        constexpr u64 MASK = 0x1FFFFF; // 21 bits
        u64 ux = static_cast<u64>(static_cast<u32>(x)) & MASK;
        u64 uy = static_cast<u64>(static_cast<u32>(y)) & MASK;
        u64 uz = static_cast<u64>(static_cast<u32>(z)) & MASK;
        return ux | (uy << 21) | (uz << 42);
    }

    float cell_size_;
    float inv_cell_size_;
    std::unordered_map<u64, std::vector<u32>> cells_;
};

// ─────────────────────────────────────────────────────────────────────────────
// SpatialHash2D - uniform grid spatial partitioning for 2D broadphase
// ─────────────────────────────────────────────────────────────────────────────

class SpatialHash2D {
public:
    explicit SpatialHash2D(float cell_size = 2.0f)
        : cell_size_(cell_size), inv_cell_size_(1.0f / cell_size) {}

    void clear() { cells_.clear(); }

    void set_cell_size(float size) {
        cell_size_ = size;
        inv_cell_size_ = 1.0f / size;
    }

    /// Insert a body ID with its 2D AABB (min/max).
    void insert(u32 id, Vec2 aabb_min, Vec2 aabb_max) {
        i32 x0 = cell(aabb_min.x), y0 = cell(aabb_min.y);
        i32 x1 = cell(aabb_max.x), y1 = cell(aabb_max.y);

        for (i32 x = x0; x <= x1; ++x)
            for (i32 y = y0; y <= y1; ++y)
                cells_[hash_key(x, y)].push_back(id);
    }

    void query(Vec2 aabb_min, Vec2 aabb_max, std::unordered_set<u64>& pairs,
               u32 self_id, std::vector<std::pair<u32, u32>>& out) const {
        i32 x0 = cell(aabb_min.x), y0 = cell(aabb_min.y);
        i32 x1 = cell(aabb_max.x), y1 = cell(aabb_max.y);

        for (i32 x = x0; x <= x1; ++x) {
            for (i32 y = y0; y <= y1; ++y) {
                auto it = cells_.find(hash_key(x, y));
                if (it == cells_.end()) continue;
                for (u32 other_id : it->second) {
                    if (other_id == self_id) continue;
                    u32 lo = self_id < other_id ? self_id : other_id;
                    u32 hi = self_id < other_id ? other_id : self_id;
                    u64 pair_key = (static_cast<u64>(lo) << 32) | hi;
                    if (pairs.insert(pair_key).second) {
                        out.emplace_back(lo, hi);
                    }
                }
            }
        }
    }

private:
    i32 cell(float v) const { return static_cast<i32>(std::floor(v * inv_cell_size_)); }

    static u64 hash_key(i32 x, i32 y) {
        // Losslessly pack two 32-bit cell coordinates so distinct cells never
        // collide (the previous multiplicative-XOR hash aliased cells).
        u64 ux = static_cast<u64>(static_cast<u32>(x));
        u64 uy = static_cast<u64>(static_cast<u32>(y));
        return ux | (uy << 32);
    }

    float cell_size_;
    float inv_cell_size_;
    std::unordered_map<u64, std::vector<u32>> cells_;
};

} // namespace nexus::physics
