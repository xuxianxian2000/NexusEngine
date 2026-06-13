#include "nexus/scene/binary_serializer.h"
#include "nexus/scene/hierarchy.h"
#include "nexus/core/log.h"
#include <fstream>
#include <cstring>

namespace nexus {

// ── WriteBuffer helpers ────────────────────────────────────────────────────

void BinarySceneSerializer::WriteBuffer::write_u8(u8 v) { data.push_back(v); }

void BinarySceneSerializer::WriteBuffer::write_u16(u16 v) {
    data.push_back(static_cast<u8>(v & 0xFF));
    data.push_back(static_cast<u8>((v >> 8) & 0xFF));
}

void BinarySceneSerializer::WriteBuffer::write_u32(u32 v) {
    data.push_back(static_cast<u8>(v & 0xFF));
    data.push_back(static_cast<u8>((v >> 8) & 0xFF));
    data.push_back(static_cast<u8>((v >> 16) & 0xFF));
    data.push_back(static_cast<u8>((v >> 24) & 0xFF));
}

void BinarySceneSerializer::WriteBuffer::write_i32(i32 v) {
    u32 uv = 0;
    std::memcpy(&uv, &v, sizeof(v));
    write_u32(uv);
}

void BinarySceneSerializer::WriteBuffer::write_f32(float v) {
    u32 uv = 0;
    std::memcpy(&uv, &v, sizeof(v));
    write_u32(uv);
}

void BinarySceneSerializer::WriteBuffer::write_string(const std::string& s) {
    auto len = static_cast<u16>(s.size());
    write_u16(len);
    data.insert(data.end(), s.begin(), s.end());
}

void BinarySceneSerializer::WriteBuffer::write_vec2(Vec2 v) {
    write_f32(v.x); write_f32(v.y);
}

void BinarySceneSerializer::WriteBuffer::write_vec3(Vec3 v) {
    write_f32(v.x); write_f32(v.y); write_f32(v.z);
}

void BinarySceneSerializer::WriteBuffer::write_vec4(Vec4 v) {
    write_f32(v.x); write_f32(v.y); write_f32(v.z); write_f32(v.w);
}

void BinarySceneSerializer::WriteBuffer::write_quat(Quat q) {
    write_f32(q.w); write_f32(q.x); write_f32(q.y); write_f32(q.z);
}

// ── ReadCursor helpers ─────────────────────────────────────────────────────

u8 BinarySceneSerializer::ReadCursor::read_u8() {
    return data[pos++];
}

u16 BinarySceneSerializer::ReadCursor::read_u16() {
    u16 v = static_cast<u16>(data[pos]) | (static_cast<u16>(data[pos + 1]) << 8);
    pos += 2;
    return v;
}

u32 BinarySceneSerializer::ReadCursor::read_u32() {
    u32 v = static_cast<u32>(data[pos])
          | (static_cast<u32>(data[pos + 1]) << 8)
          | (static_cast<u32>(data[pos + 2]) << 16)
          | (static_cast<u32>(data[pos + 3]) << 24);
    pos += 4;
    return v;
}

i32 BinarySceneSerializer::ReadCursor::read_i32() {
    u32 uv = read_u32();
    i32 v = 0;
    std::memcpy(&v, &uv, sizeof(v));
    return v;
}

float BinarySceneSerializer::ReadCursor::read_f32() {
    u32 uv = read_u32();
    float v = 0.0f;
    std::memcpy(&v, &uv, sizeof(v));
    return v;
}

std::string BinarySceneSerializer::ReadCursor::read_string() {
    u16 len = read_u16();
    if (!can_read(len)) {
        pos = size; // mark exhausted; refuse to read past the buffer
        return {};
    }
    std::string s(reinterpret_cast<const char*>(&data[pos]), len);
    pos += len;
    return s;
}

Vec2 BinarySceneSerializer::ReadCursor::read_vec2() {
    float x = read_f32(); float y = read_f32();
    return {x, y};
}

Vec3 BinarySceneSerializer::ReadCursor::read_vec3() {
    float x = read_f32(); float y = read_f32(); float z = read_f32();
    return {x, y, z};
}

Vec4 BinarySceneSerializer::ReadCursor::read_vec4() {
    float x = read_f32(); float y = read_f32(); float z = read_f32(); float w = read_f32();
    return {x, y, z, w};
}

Quat BinarySceneSerializer::ReadCursor::read_quat() {
    float w = read_f32(); float x = read_f32(); float y = read_f32(); float z = read_f32();
    return Quat{w, x, y, z};
}

void BinarySceneSerializer::ReadCursor::skip(u32 bytes) {
    pos += bytes;
}

// ── Serialize entity ───────────────────────────────────────────────────────

void BinarySceneSerializer::serialize_entity(const Registry& reg, Entity e,
                                              WriteBuffer& buf) const {
    // Count components first, then write
    WriteBuffer entity_buf;
    u16 comp_count = 0;

    // We write each component as: type_id(u32) + data_size(u32) + data(bytes)
    auto write_component = [&](ComponentTypeId type_id, auto write_fn) {
        WriteBuffer comp_data;
        write_fn(comp_data);
        entity_buf.write_u32(static_cast<u32>(type_id));
        entity_buf.write_u32(static_cast<u32>(comp_data.data.size()));
        entity_buf.data.insert(entity_buf.data.end(),
                               comp_data.data.begin(), comp_data.data.end());
        ++comp_count;
    };

    // Entity ID
    entity_buf.write_u32(e);

    if (reg.has_component<TagComponent>(e)) {
        auto& c = reg.get_component<TagComponent>(e);
        write_component(CT_Tag, [&](WriteBuffer& d) { d.write_string(c.name); });
    }

    if (reg.has_component<Transform2DComponent>(e)) {
        auto& c = reg.get_component<Transform2DComponent>(e);
        write_component(CT_Transform2D, [&](WriteBuffer& d) {
            d.write_vec2(c.position); d.write_f32(c.rotation); d.write_vec2(c.scale);
        });
    }

    if (reg.has_component<Transform3DComponent>(e)) {
        auto& c = reg.get_component<Transform3DComponent>(e);
        write_component(CT_Transform3D, [&](WriteBuffer& d) {
            d.write_vec3(c.position); d.write_quat(c.rotation); d.write_vec3(c.scale);
        });
    }

    if (reg.has_component<SpriteRendererComponent>(e)) {
        auto& c = reg.get_component<SpriteRendererComponent>(e);
        write_component(CT_SpriteRenderer, [&](WriteBuffer& d) {
            d.write_u32(c.texture_id); d.write_vec4(c.color);
            d.write_vec2(c.uv_min); d.write_vec2(c.uv_max);
            d.write_i32(c.sort_order);
        });
    }

    if (reg.has_component<MeshRendererComponent>(e)) {
        auto& c = reg.get_component<MeshRendererComponent>(e);
        write_component(CT_MeshRenderer, [&](WriteBuffer& d) {
            d.write_u32(c.mesh_id); d.write_u32(c.material_id);
        });
    }

    if (reg.has_component<CameraComponent>(e)) {
        auto& c = reg.get_component<CameraComponent>(e);
        write_component(CT_Camera, [&](WriteBuffer& d) {
            d.write_u8(c.is_primary ? 1 : 0);
            d.write_u8(c.is_orthographic ? 1 : 0);
            d.write_f32(c.fov); d.write_f32(c.ortho_size);
            d.write_f32(c.near_clip); d.write_f32(c.far_clip);
        });
    }

    if (reg.has_component<DirectionalLightComponent>(e)) {
        auto& c = reg.get_component<DirectionalLightComponent>(e);
        write_component(CT_DirectionalLight, [&](WriteBuffer& d) {
            d.write_vec3(c.color); d.write_f32(c.intensity);
        });
    }

    if (reg.has_component<PointLightComponent>(e)) {
        auto& c = reg.get_component<PointLightComponent>(e);
        write_component(CT_PointLight, [&](WriteBuffer& d) {
            d.write_vec3(c.color); d.write_f32(c.intensity); d.write_f32(c.radius);
        });
    }

    if (reg.has_component<RigidBody2DComponent>(e)) {
        auto& c = reg.get_component<RigidBody2DComponent>(e);
        write_component(CT_RigidBody2D, [&](WriteBuffer& d) {
            d.write_u8(static_cast<u8>(c.type));
            d.write_f32(c.density); d.write_f32(c.friction); d.write_f32(c.restitution);
            d.write_f32(c.linear_damping); d.write_f32(c.angular_damping);
            d.write_f32(c.gravity_scale); d.write_u8(c.fixed_rotation ? 1 : 0);
            d.write_vec2(c.velocity); d.write_f32(c.angular_velocity);
        });
    }

    if (reg.has_component<Collider2DComponent>(e)) {
        auto& c = reg.get_component<Collider2DComponent>(e);
        write_component(CT_Collider2D, [&](WriteBuffer& d) {
            d.write_u8(static_cast<u8>(c.shape));
            d.write_vec2(c.offset); d.write_vec2(c.half_size);
            d.write_f32(c.radius); d.write_u8(c.is_trigger ? 1 : 0);
            d.write_u16(c.layer); d.write_u16(c.mask);
        });
    }

    if (reg.has_component<RigidBody3DComponent>(e)) {
        auto& c = reg.get_component<RigidBody3DComponent>(e);
        write_component(CT_RigidBody3D, [&](WriteBuffer& d) {
            d.write_u8(static_cast<u8>(c.type));
            d.write_f32(c.mass); d.write_f32(c.friction); d.write_f32(c.restitution);
            d.write_f32(c.linear_damping); d.write_f32(c.angular_damping);
            d.write_f32(c.gravity_scale);
            d.write_vec3(c.velocity); d.write_vec3(c.angular_velocity);
        });
    }

    if (reg.has_component<Collider3DComponent>(e)) {
        auto& c = reg.get_component<Collider3DComponent>(e);
        write_component(CT_Collider3D, [&](WriteBuffer& d) {
            d.write_u8(static_cast<u8>(c.shape));
            d.write_vec3(c.offset); d.write_vec3(c.half_extents);
            d.write_f32(c.radius); d.write_f32(c.height);
            d.write_u8(c.is_trigger ? 1 : 0);
            d.write_u16(c.layer); d.write_u16(c.mask);
        });
    }

    if (reg.has_component<AudioSourceComponent>(e)) {
        auto& c = reg.get_component<AudioSourceComponent>(e);
        write_component(CT_AudioSource, [&](WriteBuffer& d) {
            d.write_u32(c.clip_id); d.write_f32(c.volume); d.write_f32(c.pitch);
            d.write_f32(c.min_distance); d.write_f32(c.max_distance);
            d.write_u8(c.looping ? 1 : 0); d.write_u8(c.spatial ? 1 : 0);
            d.write_u8(c.play_on_start ? 1 : 0); d.write_u32(c.bus);
        });
    }

    if (reg.has_component<AudioListenerComponent>(e)) {
        auto& c = reg.get_component<AudioListenerComponent>(e);
        write_component(CT_AudioListener, [&](WriteBuffer& d) {
            d.write_u8(c.active ? 1 : 0);
        });
    }

    if (reg.has_component<TilemapComponent>(e)) {
        auto& c = reg.get_component<TilemapComponent>(e);
        write_component(CT_Tilemap, [&](WriteBuffer& d) {
            d.write_u32(c.width); d.write_u32(c.height);
            d.write_f32(c.tile_size); d.write_u32(c.texture_id);
            d.write_u32(c.tiles_per_row); d.write_u32(c.tiles_per_col);
            d.write_u32(static_cast<u32>(c.tiles.size()));
            for (i32 tile : c.tiles) d.write_i32(tile);
        });
    }

    if (reg.has_component<HierarchyComponent>(e)) {
        auto& c = reg.get_component<HierarchyComponent>(e);
        if (c.parent != INVALID_ENTITY) {
            write_component(CT_Hierarchy, [&](WriteBuffer& d) {
                d.write_u32(c.parent);
            });
        }
    }

    // Now write to main buffer: comp_count + entity_data
    buf.write_u16(comp_count);
    buf.data.insert(buf.data.end(), entity_buf.data.begin(), entity_buf.data.end());
}

// ── Deserialize entity ─────────────────────────────────────────────────────

Entity BinarySceneSerializer::deserialize_entity(Registry& reg, ReadCursor& cursor,
                                                  std::unordered_map<u32, Entity>& id_map) const {
    if (!cursor.can_read(6)) return INVALID_ENTITY; // u16 + u32

    u16 comp_count = cursor.read_u16();
    u32 original_id = cursor.read_u32();

    Entity e = reg.create();
    id_map[original_id] = e;

    for (u16 i = 0; i < comp_count; ++i) {
        if (!cursor.can_read(8)) break; // type_id + data_size
        u32 type_id = cursor.read_u32();
        u32 data_size = cursor.read_u32();

        if (!cursor.can_read(data_size)) break;

        u32 start_pos = cursor.pos;

        switch (static_cast<ComponentTypeId>(type_id)) {
        case CT_Tag: {
            TagComponent c;
            c.name = cursor.read_string();
            reg.add_component<TagComponent>(e, std::move(c));
            break;
        }
        case CT_Transform2D: {
            Transform2DComponent c;
            c.position = cursor.read_vec2();
            c.rotation = cursor.read_f32();
            c.scale = cursor.read_vec2();
            reg.add_component<Transform2DComponent>(e, c);
            break;
        }
        case CT_Transform3D: {
            Transform3DComponent c;
            c.position = cursor.read_vec3();
            c.rotation = cursor.read_quat();
            c.scale = cursor.read_vec3();
            reg.add_component<Transform3DComponent>(e, c);
            break;
        }
        case CT_SpriteRenderer: {
            SpriteRendererComponent c;
            c.texture_id = cursor.read_u32();
            c.color = cursor.read_vec4();
            c.uv_min = cursor.read_vec2();
            c.uv_max = cursor.read_vec2();
            c.sort_order = cursor.read_i32();
            reg.add_component<SpriteRendererComponent>(e, c);
            break;
        }
        case CT_MeshRenderer: {
            MeshRendererComponent c;
            c.mesh_id = cursor.read_u32();
            c.material_id = cursor.read_u32();
            reg.add_component<MeshRendererComponent>(e, c);
            break;
        }
        case CT_Camera: {
            CameraComponent c;
            c.is_primary = cursor.read_u8() != 0;
            c.is_orthographic = cursor.read_u8() != 0;
            c.fov = cursor.read_f32();
            c.ortho_size = cursor.read_f32();
            c.near_clip = cursor.read_f32();
            c.far_clip = cursor.read_f32();
            reg.add_component<CameraComponent>(e, c);
            break;
        }
        case CT_DirectionalLight: {
            DirectionalLightComponent c;
            c.color = cursor.read_vec3();
            c.intensity = cursor.read_f32();
            reg.add_component<DirectionalLightComponent>(e, c);
            break;
        }
        case CT_PointLight: {
            PointLightComponent c;
            c.color = cursor.read_vec3();
            c.intensity = cursor.read_f32();
            c.radius = cursor.read_f32();
            reg.add_component<PointLightComponent>(e, c);
            break;
        }
        case CT_RigidBody2D: {
            RigidBody2DComponent c;
            c.type = static_cast<RigidBody2DComponent::Type>(cursor.read_u8());
            c.density = cursor.read_f32(); c.friction = cursor.read_f32();
            c.restitution = cursor.read_f32();
            c.linear_damping = cursor.read_f32(); c.angular_damping = cursor.read_f32();
            c.gravity_scale = cursor.read_f32(); c.fixed_rotation = cursor.read_u8() != 0;
            c.velocity = cursor.read_vec2(); c.angular_velocity = cursor.read_f32();
            reg.add_component<RigidBody2DComponent>(e, c);
            break;
        }
        case CT_Collider2D: {
            Collider2DComponent c;
            c.shape = static_cast<Collider2DComponent::Shape>(cursor.read_u8());
            c.offset = cursor.read_vec2(); c.half_size = cursor.read_vec2();
            c.radius = cursor.read_f32(); c.is_trigger = cursor.read_u8() != 0;
            c.layer = cursor.read_u16(); c.mask = cursor.read_u16();
            reg.add_component<Collider2DComponent>(e, c);
            break;
        }
        case CT_RigidBody3D: {
            RigidBody3DComponent c;
            c.type = static_cast<RigidBody3DComponent::Type>(cursor.read_u8());
            c.mass = cursor.read_f32(); c.friction = cursor.read_f32();
            c.restitution = cursor.read_f32();
            c.linear_damping = cursor.read_f32(); c.angular_damping = cursor.read_f32();
            c.gravity_scale = cursor.read_f32();
            c.velocity = cursor.read_vec3(); c.angular_velocity = cursor.read_vec3();
            reg.add_component<RigidBody3DComponent>(e, c);
            break;
        }
        case CT_Collider3D: {
            Collider3DComponent c;
            c.shape = static_cast<Collider3DComponent::Shape>(cursor.read_u8());
            c.offset = cursor.read_vec3(); c.half_extents = cursor.read_vec3();
            c.radius = cursor.read_f32(); c.height = cursor.read_f32();
            c.is_trigger = cursor.read_u8() != 0;
            c.layer = cursor.read_u16(); c.mask = cursor.read_u16();
            reg.add_component<Collider3DComponent>(e, c);
            break;
        }
        case CT_AudioSource: {
            AudioSourceComponent c;
            c.clip_id = cursor.read_u32(); c.volume = cursor.read_f32();
            c.pitch = cursor.read_f32();
            c.min_distance = cursor.read_f32(); c.max_distance = cursor.read_f32();
            c.looping = cursor.read_u8() != 0; c.spatial = cursor.read_u8() != 0;
            c.play_on_start = cursor.read_u8() != 0; c.bus = cursor.read_u32();
            reg.add_component<AudioSourceComponent>(e, c);
            break;
        }
        case CT_AudioListener: {
            AudioListenerComponent c;
            c.active = cursor.read_u8() != 0;
            reg.add_component<AudioListenerComponent>(e, c);
            break;
        }
        case CT_Tilemap: {
            TilemapComponent c;
            c.width = cursor.read_u32(); c.height = cursor.read_u32();
            c.tile_size = cursor.read_f32(); c.texture_id = cursor.read_u32();
            c.tiles_per_row = cursor.read_u32(); c.tiles_per_col = cursor.read_u32();
            u32 tile_count = cursor.read_u32();
            c.tiles.resize(tile_count);
            for (u32 t = 0; t < tile_count; ++t) c.tiles[t] = cursor.read_i32();
            reg.add_component<TilemapComponent>(e, std::move(c));
            break;
        }
        case CT_Hierarchy:
            // Deferred — hierarchy restored in second pass
            cursor.skip(data_size);
            break;
        case CT_Active:
            cursor.skip(data_size);
            break;
        default:
            // Unknown component type — skip for forward compatibility
            NX_WARN("BinarySerializer: skipping unknown component type {}", type_id);
            cursor.pos = start_pos + data_size;
            break;
        }

        // Ensure cursor is at the correct position after reading
        cursor.pos = start_pos + data_size;
    }

    return e;
}

// ── Public API ──────────────────────────────────────────────────────────────

std::vector<u8> BinarySceneSerializer::to_binary() const {
    WriteBuffer buf;

    auto& reg = scene_.registry();
    auto entities = reg.view<TagComponent>();

    // Header
    buf.write_u32(MAGIC);
    buf.write_u32(FORMAT_VERSION);
    buf.write_u32(0); // flags
    buf.write_u32(static_cast<u32>(entities.size()));

    // Serialize entities
    for (Entity e : entities) {
        serialize_entity(reg, e, buf);
    }

    return buf.data;
}

bool BinarySceneSerializer::from_binary(const u8* data, u32 size) {
    ReadCursor cursor{data, size, 0};

    // Read header
    if (!cursor.can_read(16)) {
        NX_ERROR("BinarySerializer: file too small for header");
        return false;
    }

    u32 magic = cursor.read_u32();
    if (magic != MAGIC) {
        NX_ERROR("BinarySerializer: invalid magic bytes (expected NXS)");
        return false;
    }

    u32 version = cursor.read_u32();
    if (version > FORMAT_VERSION) {
        NX_ERROR("BinarySerializer: file version {} is newer than supported version {}",
                 version, FORMAT_VERSION);
        return false;
    }

    cursor.read_u32(); // flags (reserved)
    u32 entity_count = cursor.read_u32();

    scene_.clear();
    auto& reg = scene_.registry();

    std::unordered_map<u32, Entity> id_map;
    for (u32 i = 0; i < entity_count; ++i) {
        Entity e = deserialize_entity(reg, cursor, id_map);
        if (e == INVALID_ENTITY) {
            NX_ERROR("BinarySerializer: failed to deserialize entity {}", i);
            return false;
        }
    }

    // Second pass: restore hierarchy (re-read from data)
    ReadCursor cursor2{data, size, 16}; // skip header
    for (u32 i = 0; i < entity_count; ++i) {
        if (!cursor2.can_read(6)) break;
        u16 comp_count = cursor2.read_u16();
        u32 original_id = cursor2.read_u32();

        for (u16 c = 0; c < comp_count; ++c) {
            if (!cursor2.can_read(8)) break;
            u32 type_id = cursor2.read_u32();
            u32 data_size = cursor2.read_u32();

            if (type_id == static_cast<u32>(CT_Hierarchy) && data_size >= 4) {
                u32 parent_original = cursor2.read_u32();
                auto child_it = id_map.find(original_id);
                auto parent_it = id_map.find(parent_original);
                if (child_it != id_map.end() && parent_it != id_map.end()) {
                    Hierarchy::set_parent(reg, child_it->second, parent_it->second);
                }
                cursor2.skip(data_size - 4);
            } else {
                cursor2.skip(data_size);
            }
        }
    }

    NX_INFO("Binary scene loaded: {} entities (format v{})", entity_count, version);
    return true;
}

bool BinarySceneSerializer::save(const std::string& filepath) const {
    auto data = to_binary();
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        NX_ERROR("BinarySerializer: cannot open file for writing: {}", filepath);
        return false;
    }
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    NX_INFO("Binary scene saved to: {} ({} bytes)", filepath, data.size());
    return true;
}

bool BinarySceneSerializer::load(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        NX_ERROR("BinarySerializer: cannot open file for reading: {}", filepath);
        return false;
    }
    auto file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<u8> data(static_cast<size_t>(file_size));
    file.read(reinterpret_cast<char*>(data.data()), file_size);
    return from_binary(data);
}

} // namespace nexus
