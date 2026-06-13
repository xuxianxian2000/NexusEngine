#pragma once

#include "nexus/scene/scene.h"
#include "nexus/core/types.h"
#include <string>
#include <vector>

namespace nexus {

// ─────────────────────────────────────────────────────────────────────────────
// BinarySceneSerializer — fast binary scene format with schema versioning
// ─────────────────────────────────────────────────────────────────────────────
//
// File format (.nxs):
//   [Header]
//     magic:       4 bytes "NXS\0"
//     version:     u32 (schema version)
//     flags:       u32 (reserved)
//     entity_count:u32
//     data_offset: u32 (byte offset to entity data)
//   [ComponentTypeTable]
//     type_count:  u32
//     For each type: type_id(u32) + name_len(u16) + name(chars)
//   [EntityData]
//     For each entity:
//       component_count: u16
//       For each component:
//         type_id:   u32
//         data_size: u32
//         data:      raw bytes
//
// Schema versioning:
//   - Reader skips unknown component type_ids (forward compatible)
//   - Version bump only needed when header format changes

class BinarySceneSerializer {
public:
    explicit BinarySceneSerializer(Scene& scene) : scene_(scene) {}

    /// Serialize the scene to a binary buffer.
    [[nodiscard]] std::vector<u8> to_binary() const;

    /// Deserialize a scene from a binary buffer. Returns false on failure.
    bool from_binary(const u8* data, u32 size);
    bool from_binary(const std::vector<u8>& data) {
        return from_binary(data.data(), static_cast<u32>(data.size()));
    }

    /// Save to file.
    bool save(const std::string& filepath) const;

    /// Load from file.
    bool load(const std::string& filepath);

    /// Current format version.
    static constexpr u32 FORMAT_VERSION = 1;

    /// Magic bytes.
    static constexpr u32 MAGIC = 0x0053584E; // "NXS\0" little-endian

private:
    Scene& scene_;

    // Component type IDs (stable, never reorder)
    enum ComponentTypeId : u32 {
        CT_Tag = 1,
        CT_Transform2D = 2,
        CT_Transform3D = 3,
        CT_SpriteRenderer = 4,
        CT_MeshRenderer = 5,
        CT_Camera = 6,
        CT_DirectionalLight = 7,
        CT_PointLight = 8,
        CT_RigidBody2D = 9,
        CT_Collider2D = 10,
        CT_RigidBody3D = 11,
        CT_Collider3D = 12,
        CT_AudioSource = 13,
        CT_AudioListener = 14,
        CT_Tilemap = 15,
        CT_Hierarchy = 16,
        CT_Active = 17,
    };

    // Writer helpers
    struct WriteBuffer {
        std::vector<u8> data;
        void write_u8(u8 v);
        void write_u16(u16 v);
        void write_u32(u32 v);
        void write_i32(i32 v);
        void write_f32(float v);
        void write_string(const std::string& s);
        void write_vec2(Vec2 v);
        void write_vec3(Vec3 v);
        void write_vec4(Vec4 v);
        void write_quat(Quat q);
    };

    // Reader helpers
    struct ReadCursor {
        const u8* data;
        u32 size;
        u32 pos{0};

        // Written as `bytes <= size - pos` to avoid the pos+bytes overflow that
        // a corrupt length field could exploit to pass the bounds check.
        bool can_read(u32 bytes) const { return pos <= size && bytes <= size - pos; }
        u8 read_u8();
        u16 read_u16();
        u32 read_u32();
        i32 read_i32();
        float read_f32();
        std::string read_string();
        Vec2 read_vec2();
        Vec3 read_vec3();
        Vec4 read_vec4();
        Quat read_quat();
        void skip(u32 bytes);
    };

    void serialize_entity(const Registry& reg, Entity e, WriteBuffer& buf) const;
    Entity deserialize_entity(Registry& reg, ReadCursor& cursor,
                              std::unordered_map<u32, Entity>& id_map) const;
};

} // namespace nexus
