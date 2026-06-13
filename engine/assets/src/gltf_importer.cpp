#include "nexus/assets/gltf_importer.h"
#include "nexus/core/log.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace nexus::assets {

using json = nlohmann::json;

// ── JSON helpers ───────────────────────────────────────────────────────────

static float json_float(const json& j, const std::string& key, float def = 0.0f) {
    if (j.contains(key) && j[key].is_number()) return j[key].get<float>();
    return def;
}

static i32 json_int(const json& j, const std::string& key, i32 def = -1) {
    if (j.contains(key) && j[key].is_number_integer()) return j[key].get<i32>();
    return def;
}

// ── Parse glTF ─────────────────────────────────────────────────────────────

bool parse_gltf(const std::string& json_str_data,
                const std::vector<u8>& glb_bin,
                const std::string& base_dir,
                GltfScene& out) {
    json root;
    try {
        root = json::parse(json_str_data);
    } catch (const json::parse_error& e) {
        NX_ERROR("glTF: JSON parse error: {}", e.what());
        return false;
    }

    // Buffers
    if (root.contains("buffers")) {
        for (const auto& buf : root["buffers"]) {
            u32 byte_length = buf.value("byteLength", 0u);
            if (!glb_bin.empty() && out.buffers.empty()) {
                // First buffer in GLB comes from binary chunk
                out.buffers.push_back(glb_bin);
            } else {
                std::string uri = buf.value("uri", std::string{});
                if (!uri.empty()) {
                    std::string path = base_dir + "/" + uri;
                    std::ifstream f(path, std::ios::binary | std::ios::ate);
                    if (f.is_open()) {
                        auto sz = f.tellg();
                        f.seekg(0);
                        std::vector<u8> data(static_cast<size_t>(sz));
                        f.read(reinterpret_cast<char*>(data.data()), sz);
                        out.buffers.push_back(std::move(data));
                    } else {
                        NX_WARN("glTF: cannot read buffer: {}", path);
                        out.buffers.emplace_back(byte_length, u8{0});
                    }
                } else {
                    out.buffers.emplace_back(byte_length, u8{0});
                }
            }
        }
    }

    // Buffer views
    if (root.contains("bufferViews")) {
        for (const auto& bv : root["bufferViews"]) {
            GltfBufferView v;
            v.buffer = bv.value("buffer", 0u);
            v.byte_offset = bv.value("byteOffset", 0u);
            v.byte_length = bv.value("byteLength", 0u);
            v.byte_stride = bv.value("byteStride", 0u);
            out.buffer_views.push_back(v);
        }
    }

    // Accessors
    if (root.contains("accessors")) {
        for (const auto& acc : root["accessors"]) {
            GltfAccessor a;
            a.buffer_view = acc.value("bufferView", 0u);
            a.byte_offset = acc.value("byteOffset", 0u);
            a.count = acc.value("count", 0u);
            a.component_type = acc.value("componentType", 0u);
            a.type = acc.value("type", std::string{"SCALAR"});
            if (acc.contains("min")) {
                u32 i = 0;
                for (const auto& v : acc["min"]) {
                    if (i < 4) a.min_vals[i++] = v.get<float>();
                }
            }
            if (acc.contains("max")) {
                u32 i = 0;
                for (const auto& v : acc["max"]) {
                    if (i < 4) a.max_vals[i++] = v.get<float>();
                }
            }
            out.accessors.push_back(a);
        }
    }

    // Meshes
    if (root.contains("meshes")) {
        for (const auto& mesh : root["meshes"]) {
            GltfMesh m;
            m.name = mesh.value("name", std::string{"mesh"});
            if (mesh.contains("primitives")) {
                for (const auto& prim : mesh["primitives"]) {
                    GltfPrimitive p;
                    if (prim.contains("attributes")) {
                        const auto& attr = prim["attributes"];
                        p.position_accessor = json_int(attr, "POSITION");
                        p.normal_accessor = json_int(attr, "NORMAL");
                        p.texcoord_accessor = json_int(attr, "TEXCOORD_0");
                        p.tangent_accessor = json_int(attr, "TANGENT");
                        p.joints_accessor = json_int(attr, "JOINTS_0");
                        p.weights_accessor = json_int(attr, "WEIGHTS_0");
                    }
                    p.indices_accessor = json_int(prim, "indices");
                    p.material = json_int(prim, "material");
                    m.primitives.push_back(p);
                }
            }
            out.meshes.push_back(std::move(m));
        }
    }

    // Materials
    if (root.contains("materials")) {
        for (const auto& mat : root["materials"]) {
            GltfMaterial m;
            m.name = mat.value("name", std::string{"material"});
            m.double_sided = mat.value("doubleSided", false);
            if (mat.contains("pbrMetallicRoughness")) {
                const auto& pbr = mat["pbrMetallicRoughness"];
                if (pbr.contains("baseColorFactor")) {
                    u32 i = 0;
                    for (const auto& v : pbr["baseColorFactor"]) {
                        if (i < 4) m.base_color[i++] = v.get<float>();
                    }
                }
                m.metallic = json_float(pbr, "metallicFactor", 1.0f);
                m.roughness = json_float(pbr, "roughnessFactor", 1.0f);
                if (pbr.contains("baseColorTexture")) {
                    m.base_color_texture = json_int(pbr["baseColorTexture"], "index");
                }
                if (pbr.contains("metallicRoughnessTexture")) {
                    m.metallic_roughness_texture = json_int(pbr["metallicRoughnessTexture"], "index");
                }
            }
            if (mat.contains("normalTexture")) {
                m.normal_texture = json_int(mat["normalTexture"], "index");
            }
            out.materials.push_back(m);
        }
    }

    // Nodes
    if (root.contains("nodes")) {
        for (const auto& node : root["nodes"]) {
            GltfNode n;
            n.name = node.value("name", std::string{"node"});
            n.mesh = json_int(node, "mesh");
            n.skin = json_int(node, "skin");
            if (node.contains("children")) {
                for (const auto& c : node["children"]) n.children.push_back(c.get<u32>());
            }
            if (node.contains("translation")) {
                u32 i = 0;
                for (const auto& v : node["translation"]) {
                    if (i < 3) n.translation[i++] = v.get<float>();
                }
            }
            if (node.contains("rotation")) {
                u32 i = 0;
                for (const auto& v : node["rotation"]) {
                    if (i < 4) n.rotation[i++] = v.get<float>();
                }
            }
            if (node.contains("scale")) {
                u32 i = 0;
                for (const auto& v : node["scale"]) {
                    if (i < 3) n.scale[i++] = v.get<float>();
                }
            }
            out.nodes.push_back(std::move(n));
        }
    }

    // Skins
    if (root.contains("skins")) {
        for (const auto& skin : root["skins"]) {
            GltfSkin s;
            s.name = skin.value("name", std::string{"skin"});
            s.inverse_bind_matrices = json_int(skin, "inverseBindMatrices");
            s.skeleton_root = json_int(skin, "skeleton");
            if (skin.contains("joints")) {
                for (const auto& j : skin["joints"]) s.joints.push_back(j.get<u32>());
            }
            out.skins.push_back(std::move(s));
        }
    }

    // Animations
    if (root.contains("animations")) {
        for (const auto& anim : root["animations"]) {
            GltfAnimation a;
            a.name = anim.value("name", std::string{"animation"});
            if (anim.contains("samplers")) {
                for (const auto& samp : anim["samplers"]) {
                    GltfAnimSampler s;
                    s.input = json_int(samp, "input");
                    s.output = json_int(samp, "output");
                    s.interpolation = samp.value("interpolation", std::string{"LINEAR"});
                    a.samplers.push_back(s);
                }
            }
            if (anim.contains("channels")) {
                for (const auto& ch : anim["channels"]) {
                    GltfAnimChannel c;
                    c.sampler = json_int(ch, "sampler");
                    if (ch.contains("target")) {
                        c.node = ch["target"].value("node", 0u);
                        c.path = ch["target"].value("path", std::string{""});
                    }
                    a.channels.push_back(c);
                }
            }
            out.animations.push_back(std::move(a));
        }
    }

    // Images
    if (root.contains("images")) {
        for (const auto& img : root["images"]) {
            out.images.push_back(img.value("uri", std::string{""}));
        }
    }

    NX_INFO("glTF: parsed {} meshes, {} materials, {} nodes, {} skins, {} animations",
            out.meshes.size(), out.materials.size(), out.nodes.size(),
            out.skins.size(), out.animations.size());
    return true;
}

// ── Accessor reading ───────────────────────────────────────────────────────

static u32 component_size(u32 component_type) {
    switch (component_type) {
        case 5120: case 5121: return 1;
        case 5122: case 5123: return 2;
        case 5125: case 5126: return 4;
    }
    return 4;
}

static u32 type_components(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    return 1;
}

std::vector<float> read_accessor_floats(const GltfScene& scene, u32 accessor_index) {
    if (accessor_index >= scene.accessors.size()) return {};

    const auto& acc = scene.accessors[accessor_index];
    if (acc.buffer_view >= scene.buffer_views.size()) return {};
    const auto& bv = scene.buffer_views[acc.buffer_view];
    if (bv.buffer >= scene.buffers.size()) return {};
    const auto& buf = scene.buffers[bv.buffer];

    u32 comp_count = type_components(acc.type);
    u32 comp_sz = component_size(acc.component_type);
    u32 elem_size = comp_count * comp_sz;
    u32 stride = (bv.byte_stride > 0) ? bv.byte_stride : elem_size;

    std::vector<float> result(static_cast<size_t>(acc.count) * comp_count);

    for (u32 i = 0; i < acc.count; ++i) {
        // Compute offsets in size_t: u32 arithmetic on attacker-controlled
        // byte_offset/stride can wrap and slip past the bounds check below.
        size_t offset = static_cast<size_t>(bv.byte_offset) + acc.byte_offset
                      + static_cast<size_t>(i) * stride;
        for (u32 c = 0; c < comp_count; ++c) {
            size_t byte_pos = offset + static_cast<size_t>(c) * comp_sz;
            if (byte_pos + comp_sz > buf.size()) {
                result[static_cast<size_t>(i) * comp_count + c] = 0.0f;
                continue;
            }
            const u8* ptr = buf.data() + byte_pos;
            float val = 0.0f;
            switch (acc.component_type) {
                case 5120: { int8_t v; std::memcpy(&v, ptr, 1); val = static_cast<float>(v) / 127.0f; break; }
                case 5121: { val = static_cast<float>(*ptr) / 255.0f; break; }
                case 5122: { int16_t v; std::memcpy(&v, ptr, 2); val = static_cast<float>(v) / 32767.0f; break; }
                case 5123: { uint16_t v; std::memcpy(&v, ptr, 2); val = static_cast<float>(v) / 65535.0f; break; }
                case 5125: { uint32_t v; std::memcpy(&v, ptr, 4); val = static_cast<float>(v); break; }
                case 5126: { std::memcpy(&val, ptr, 4); break; }
            }
            result[static_cast<size_t>(i) * comp_count + c] = val;
        }
    }
    return result;
}

std::vector<u32> read_accessor_indices(const GltfScene& scene, u32 accessor_index) {
    if (accessor_index >= scene.accessors.size()) return {};

    const auto& acc = scene.accessors[accessor_index];
    if (acc.buffer_view >= scene.buffer_views.size()) return {};
    const auto& bv = scene.buffer_views[acc.buffer_view];
    if (bv.buffer >= scene.buffers.size()) return {};
    const auto& buf = scene.buffers[bv.buffer];

    u32 comp_sz = component_size(acc.component_type);
    u32 stride = (bv.byte_stride > 0) ? bv.byte_stride : comp_sz;

    std::vector<u32> result(acc.count);
    for (u32 i = 0; i < acc.count; ++i) {
        size_t offset = static_cast<size_t>(bv.byte_offset) + acc.byte_offset
                      + static_cast<size_t>(i) * stride;
        if (offset + comp_sz > buf.size()) { result[i] = 0; continue; }
        const u8* ptr = buf.data() + offset;
        switch (acc.component_type) {
            case 5121: result[i] = static_cast<u32>(*ptr); break;
            case 5123: { uint16_t v; std::memcpy(&v, ptr, 2); result[i] = static_cast<u32>(v); break; }
            case 5125: { std::memcpy(&result[i], ptr, 4); break; }
            default: result[i] = 0; break;
        }
    }
    return result;
}

// ── Conversion to engine types ─────────────────────────────────────────────

std::shared_ptr<MeshData> gltf_primitive_to_mesh(const GltfScene& scene,
                                                   const GltfPrimitive& prim,
                                                   const std::string& name) {
    auto mesh = std::make_shared<MeshData>();
    mesh->name = name;

    if (prim.position_accessor < 0) return mesh;

    auto positions = read_accessor_floats(scene, static_cast<u32>(prim.position_accessor));
    u32 vert_count = static_cast<u32>(positions.size() / 3);

    std::vector<float> normals;
    if (prim.normal_accessor >= 0) {
        normals = read_accessor_floats(scene, static_cast<u32>(prim.normal_accessor));
    }

    std::vector<float> texcoords;
    if (prim.texcoord_accessor >= 0) {
        texcoords = read_accessor_floats(scene, static_cast<u32>(prim.texcoord_accessor));
    }

    std::vector<float> tangents;
    if (prim.tangent_accessor >= 0) {
        tangents = read_accessor_floats(scene, static_cast<u32>(prim.tangent_accessor));
    }

    mesh->vertices.resize(vert_count);
    for (u32 i = 0; i < vert_count; ++i) {
        auto& v = mesh->vertices[i];
        v.position[0] = positions[static_cast<size_t>(i) * 3 + 0];
        v.position[1] = positions[static_cast<size_t>(i) * 3 + 1];
        v.position[2] = positions[static_cast<size_t>(i) * 3 + 2];

        if (static_cast<size_t>(i) * 3 + 2 < normals.size()) {
            v.normal[0] = normals[static_cast<size_t>(i) * 3 + 0];
            v.normal[1] = normals[static_cast<size_t>(i) * 3 + 1];
            v.normal[2] = normals[static_cast<size_t>(i) * 3 + 2];
        }

        if (static_cast<size_t>(i) * 2 + 1 < texcoords.size()) {
            v.texcoord[0] = texcoords[static_cast<size_t>(i) * 2 + 0];
            v.texcoord[1] = texcoords[static_cast<size_t>(i) * 2 + 1];
        }

        if (static_cast<size_t>(i) * 4 + 3 < tangents.size()) {
            v.tangent[0] = tangents[static_cast<size_t>(i) * 4 + 0];
            v.tangent[1] = tangents[static_cast<size_t>(i) * 4 + 1];
            v.tangent[2] = tangents[static_cast<size_t>(i) * 4 + 2];
            v.tangent[3] = tangents[static_cast<size_t>(i) * 4 + 3];
        }
    }

    if (prim.indices_accessor >= 0) {
        mesh->indices = read_accessor_indices(scene, static_cast<u32>(prim.indices_accessor));
    } else {
        mesh->indices.resize(vert_count);
        for (u32 i = 0; i < vert_count; ++i) mesh->indices[i] = i;
    }

    // Generate flat normals if none provided
    if (normals.empty()) {
        for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
            auto& v0 = mesh->vertices[mesh->indices[i]];
            auto& v1 = mesh->vertices[mesh->indices[i + 1]];
            auto& v2 = mesh->vertices[mesh->indices[i + 2]];
            float e1[3] = {v1.position[0] - v0.position[0], v1.position[1] - v0.position[1], v1.position[2] - v0.position[2]};
            float e2[3] = {v2.position[0] - v0.position[0], v2.position[1] - v0.position[1], v2.position[2] - v0.position[2]};
            float n[3] = {e1[1]*e2[2] - e1[2]*e2[1], e1[2]*e2[0] - e1[0]*e2[2], e1[0]*e2[1] - e1[1]*e2[0]};
            float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            if (len > 0.0f) { n[0] /= len; n[1] /= len; n[2] /= len; }
            for (int k = 0; k < 3; ++k) {
                auto& vk = mesh->vertices[mesh->indices[i + static_cast<size_t>(k)]];
                vk.normal[0] = n[0]; vk.normal[1] = n[1]; vk.normal[2] = n[2];
            }
        }
    }

    NX_INFO("glTF mesh '{}': {} verts, {} indices", name, vert_count, mesh->indices.size());
    return mesh;
}

std::shared_ptr<MaterialData> gltf_material_to_material(const GltfScene& scene,
                                                          const GltfMaterial& mat) {
    auto m = std::make_shared<MaterialData>();
    m->metallic = mat.metallic;
    m->roughness = mat.roughness;
    m->color[0] = mat.base_color[0];
    m->color[1] = mat.base_color[1];
    m->color[2] = mat.base_color[2];
    m->color[3] = mat.base_color[3];

    if (mat.base_color_texture >= 0 && static_cast<size_t>(mat.base_color_texture) < scene.images.size()) {
        m->albedo_texture = scene.images[static_cast<size_t>(mat.base_color_texture)];
    }
    if (mat.normal_texture >= 0 && static_cast<size_t>(mat.normal_texture) < scene.images.size()) {
        m->normal_texture = scene.images[static_cast<size_t>(mat.normal_texture)];
    }
    if (mat.metallic_roughness_texture >= 0 &&
        static_cast<size_t>(mat.metallic_roughness_texture) < scene.images.size()) {
        m->metallic_roughness_texture = scene.images[static_cast<size_t>(mat.metallic_roughness_texture)];
    }
    return m;
}

std::shared_ptr<AnimationData> gltf_animation_to_anim(const GltfScene& scene,
                                                        const GltfAnimation& anim) {
    auto a = std::make_shared<AnimationData>();
    a->name = anim.name;
    a->duration = 0.0f;

    for (const auto& ch : anim.channels) {
        if (ch.sampler < 0 || static_cast<size_t>(ch.sampler) >= anim.samplers.size()) continue;
        const auto& samp = anim.samplers[static_cast<size_t>(ch.sampler)];

        AnimationData::Channel channel;
        channel.target_node = (ch.node < scene.nodes.size()) ? scene.nodes[ch.node].name : "";
        channel.property = ch.path;

        if (samp.input >= 0) {
            auto timestamps = read_accessor_floats(scene, static_cast<u32>(samp.input));
            std::vector<float> values;
            if (samp.output >= 0) {
                values = read_accessor_floats(scene, static_cast<u32>(samp.output));
            }

            u32 comp = 1;
            if (ch.path == "translation" || ch.path == "scale") comp = 3;
            else if (ch.path == "rotation") comp = 4;

            for (size_t i = 0; i < timestamps.size(); ++i) {
                AnimationData::Keyframe kf;
                kf.time = timestamps[i];
                kf.components = comp;
                for (u32 c = 0; c < comp && (i * comp + c) < values.size(); ++c) {
                    kf.value[c] = values[i * comp + c];
                }
                channel.keyframes.push_back(kf);

                if (kf.time > a->duration) a->duration = kf.time;
            }
        }
        a->channels.push_back(std::move(channel));
    }

    NX_INFO("glTF animation '{}': {:.2f}s, {} channels", a->name, a->duration, a->channels.size());
    return a;
}

} // namespace nexus::assets
