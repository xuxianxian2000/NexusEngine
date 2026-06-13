#include "nexus/assets/asset_loader.h"
#include "nexus/core/log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace nexus::assets {

// ── AssetImporter base ──────────────────────────────────────────────────────

bool AssetImporter::supports(const std::string& extension) const {
    std::string lower = extension;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto exts = supported_extensions();
    return std::find(exts.begin(), exts.end(), lower) != exts.end();
}

// ── TextureImporter ─────────────────────────────────────────────────────────

std::vector<std::string> TextureImporter::supported_extensions() const {
    return {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".ppm", ".pgm"};
}

// ── BMP decoder ─────────────────────────────────────────────────────────────
// Parses BITMAPFILEHEADER + BITMAPINFOHEADER, extracts uncompressed 24/32-bit
// pixel data, and flips vertically (BMP stores rows bottom-up).

static std::shared_ptr<TextureData> decode_bmp(const std::vector<u8>& raw,
                                                const std::string& filename) {
    if (raw.size() < 54 || raw[0] != 'B' || raw[1] != 'M') {
        NX_ERROR("TextureImporter: invalid BMP header in '{}'", filename);
        return nullptr;
    }

    auto read_u32_le = [&](size_t off) -> u32 {
        return u32(raw[off]) | (u32(raw[off+1]) << 8) |
               (u32(raw[off+2]) << 16) | (u32(raw[off+3]) << 24);
    };
    auto read_u16_le = [&](size_t off) -> u16 {
        return static_cast<u16>(u16(raw[off]) | (u16(raw[off+1]) << 8));
    };
    auto read_i32_le = [&](size_t off) -> i32 {
        u32 v = read_u32_le(off);
        i32 result;
        std::memcpy(&result, &v, sizeof(result));
        return result;
    };

    u32 pixel_offset = read_u32_le(10);
    i32 width_signed = read_i32_le(18);
    i32 height_signed = read_i32_le(22);
    u16 bpp = read_u16_le(28);
    u32 compression = read_u32_le(30);

    if (width_signed <= 0) {
        NX_ERROR("TextureImporter: BMP has invalid width {} in '{}'", width_signed, filename);
        return nullptr;
    }
    if (compression != 0) {
        NX_ERROR("TextureImporter: compressed BMP not supported in '{}'", filename);
        return nullptr;
    }
    if (bpp != 24 && bpp != 32) {
        NX_ERROR("TextureImporter: only 24/32-bit BMP supported, got {}bpp in '{}'", bpp, filename);
        return nullptr;
    }

    u32 width = static_cast<u32>(width_signed);
    bool top_down = (height_signed < 0);
    u32 height = static_cast<u32>(top_down ? -height_signed : height_signed);
    u32 src_channels = bpp / 8;

    // BMP rows are padded to 4-byte boundaries
    u32 row_stride = (width * src_channels + 3) & ~u32(3);

    if (pixel_offset + static_cast<u64>(row_stride) * height > raw.size()) {
        NX_ERROR("TextureImporter: BMP pixel data truncated in '{}'", filename);
        return nullptr;
    }

    auto data = std::make_shared<TextureData>();
    data->width = width;
    data->height = height;
    data->channels = 4; // always output RGBA
    data->is_hdr = false;
    data->pixels.resize(static_cast<size_t>(width) * height * 4);

    for (u32 y = 0; y < height; ++y) {
        // BMP is bottom-up unless height is negative (top-down)
        u32 src_row = top_down ? y : (height - 1 - y);
        const u8* src = raw.data() + pixel_offset + src_row * row_stride;
        u8* dst = data->pixels.data() + static_cast<size_t>(y) * width * 4;

        for (u32 x = 0; x < width; ++x) {
            // BMP stores BGR(A)
            dst[x * 4 + 0] = src[x * src_channels + 2]; // R
            dst[x * 4 + 1] = src[x * src_channels + 1]; // G
            dst[x * 4 + 2] = src[x * src_channels + 0]; // B
            dst[x * 4 + 3] = (src_channels == 4) ? src[x * src_channels + 3] : 255;
        }
    }

    NX_INFO("TextureImporter: loaded BMP '{}' ({}x{}, {}bpp -> RGBA)",
            filename, width, height, bpp);
    return data;
}

// ── TGA decoder ─────────────────────────────────────────────────────────────
// Supports uncompressed true-color (type 2) and RLE compressed (type 10),
// 24-bit and 32-bit.

static std::shared_ptr<TextureData> decode_tga(const std::vector<u8>& raw,
                                                const std::string& filename) {
    if (raw.size() < 18) {
        NX_ERROR("TextureImporter: TGA file too small in '{}'", filename);
        return nullptr;
    }

    u8 id_length = raw[0];
    u8 image_type = raw[2];
    u32 width  = u32(raw[12]) | (u32(raw[13]) << 8);
    u32 height = u32(raw[14]) | (u32(raw[15]) << 8);
    u8 bpp = raw[16];
    u8 descriptor = raw[17];
    bool top_down = (descriptor & 0x20) != 0;

    if (width == 0 || height == 0) {
        NX_ERROR("TextureImporter: TGA has zero dimensions in '{}'", filename);
        return nullptr;
    }
    // Reject absurd dimensions before reserving, so a forged header cannot
    // trigger a multi-gigabyte allocation (the decode buffer is reserved up
    // front, before any input-size validation).
    constexpr u32 MAX_TGA_DIM = 16384;
    if (width > MAX_TGA_DIM || height > MAX_TGA_DIM) {
        NX_ERROR("TextureImporter: TGA dimensions {}x{} exceed maximum {} in '{}'",
                 width, height, MAX_TGA_DIM, filename);
        return nullptr;
    }
    if (bpp != 24 && bpp != 32) {
        NX_ERROR("TextureImporter: only 24/32-bit TGA supported, got {}bpp in '{}'", bpp, filename);
        return nullptr;
    }
    if (image_type != 2 && image_type != 10) {
        NX_ERROR("TextureImporter: unsupported TGA image type {} in '{}' (only type 2 and 10 supported)",
                 image_type, filename);
        return nullptr;
    }

    u32 src_channels = bpp / 8;
    size_t pixel_start = 18 + id_length;
    u32 total_pixels = width * height;

    // Decode pixels into a temporary BGR(A) buffer
    std::vector<u8> decoded;
    decoded.reserve(static_cast<size_t>(total_pixels) * src_channels);

    if (image_type == 2) {
        // Uncompressed true-color
        size_t needed = pixel_start + static_cast<size_t>(total_pixels) * src_channels;
        if (raw.size() < needed) {
            NX_ERROR("TextureImporter: TGA pixel data truncated in '{}'", filename);
            return nullptr;
        }
        decoded.assign(raw.begin() + static_cast<std::ptrdiff_t>(pixel_start),
                       raw.begin() + static_cast<std::ptrdiff_t>(needed));
    } else {
        // RLE compressed (type 10)
        size_t src_pos = pixel_start;
        u32 pixels_decoded = 0;

        while (pixels_decoded < total_pixels && src_pos < raw.size()) {
            u8 header = raw[src_pos++];
            u32 count = (header & 0x7F) + 1;

            if (header & 0x80) {
                // Run-length packet: one pixel repeated 'count' times
                if (src_pos + src_channels > raw.size()) break;
                for (u32 i = 0; i < count && pixels_decoded < total_pixels; ++i) {
                    for (u32 c = 0; c < src_channels; ++c) {
                        decoded.push_back(raw[src_pos + c]);
                    }
                    ++pixels_decoded;
                }
                src_pos += src_channels;
            } else {
                // Raw packet: 'count' individual pixels follow
                for (u32 i = 0; i < count && pixels_decoded < total_pixels; ++i) {
                    if (src_pos + src_channels > raw.size()) break;
                    for (u32 c = 0; c < src_channels; ++c) {
                        decoded.push_back(raw[src_pos + c]);
                    }
                    src_pos += src_channels;
                    ++pixels_decoded;
                }
            }
        }

        if (pixels_decoded < total_pixels) {
            NX_ERROR("TextureImporter: TGA RLE data incomplete in '{}'", filename);
            return nullptr;
        }
    }

    // Convert BGR(A) to RGBA, handling row order
    auto data = std::make_shared<TextureData>();
    data->width = width;
    data->height = height;
    data->channels = 4;
    data->is_hdr = false;
    data->pixels.resize(static_cast<size_t>(width) * height * 4);

    for (u32 y = 0; y < height; ++y) {
        u32 src_row = top_down ? y : (height - 1 - y);
        const u8* src = decoded.data() + static_cast<size_t>(src_row) * width * src_channels;
        u8* dst = data->pixels.data() + static_cast<size_t>(y) * width * 4;

        for (u32 x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x * src_channels + 2]; // R (from B)
            dst[x * 4 + 1] = src[x * src_channels + 1]; // G
            dst[x * 4 + 2] = src[x * src_channels + 0]; // B (from R)
            dst[x * 4 + 3] = (src_channels == 4) ? src[x * src_channels + 3] : 255;
        }
    }

    NX_INFO("TextureImporter: loaded TGA '{}' ({}x{}, type {}, {}bpp -> RGBA)",
            filename, width, height, image_type, bpp);
    return data;
}

// ── PPM/PGM decoder ─────────────────────────────────────────────────────────
// Supports binary P5 (PGM) and P6 (PPM) formats with 8-bit depth.

static std::shared_ptr<TextureData> decode_ppm_pgm(const std::vector<u8>& raw,
                                                     const std::string& filename) {
    if (raw.size() < 3) {
        NX_ERROR("TextureImporter: PPM/PGM file too small in '{}'", filename);
        return nullptr;
    }

    // Check magic number
    bool is_pgm_text = (raw[0] == 'P' && raw[1] == '2');
    bool is_ppm_text = (raw[0] == 'P' && raw[1] == '3');
    bool is_pgm_bin  = (raw[0] == 'P' && raw[1] == '5');
    bool is_ppm_bin  = (raw[0] == 'P' && raw[1] == '6');
    bool is_grayscale = is_pgm_text || is_pgm_bin;

    if (!is_pgm_text && !is_ppm_text && !is_pgm_bin && !is_ppm_bin) {
        NX_ERROR("TextureImporter: unsupported PPM/PGM format in '{}'", filename);
        return nullptr;
    }

    // Parse header: skip magic, read width, height, maxval
    // Comments start with '#' and go to end of line
    size_t pos = 2;
    auto skip_whitespace_and_comments = [&]() {
        while (pos < raw.size()) {
            if (raw[pos] == '#') {
                while (pos < raw.size() && raw[pos] != '\n') ++pos;
                if (pos < raw.size()) ++pos;
            } else if (raw[pos] == ' ' || raw[pos] == '\t' ||
                       raw[pos] == '\n' || raw[pos] == '\r') {
                ++pos;
            } else {
                break;
            }
        }
    };

    auto read_int = [&]() -> u32 {
        skip_whitespace_and_comments();
        u32 val = 0;
        while (pos < raw.size() && raw[pos] >= '0' && raw[pos] <= '9') {
            val = val * 10 + (raw[pos] - '0');
            ++pos;
        }
        return val;
    };

    u32 width = read_int();
    u32 height = read_int();
    u32 maxval = read_int();

    if (width == 0 || height == 0 || maxval == 0) {
        NX_ERROR("TextureImporter: invalid PPM/PGM header in '{}'", filename);
        return nullptr;
    }
    if (maxval > 255) {
        NX_ERROR("TextureImporter: 16-bit PPM/PGM not supported in '{}'", filename);
        return nullptr;
    }

    // After maxval, exactly one whitespace character precedes pixel data
    if (pos < raw.size() && (raw[pos] == ' ' || raw[pos] == '\t' ||
                              raw[pos] == '\n' || raw[pos] == '\r')) {
        ++pos;
    }

    auto data = std::make_shared<TextureData>();
    data->width = width;
    data->height = height;
    data->channels = 4; // output RGBA
    data->is_hdr = false;
    data->pixels.resize(static_cast<size_t>(width) * height * 4);

    u32 src_channels = is_grayscale ? 1 : 3;

    if (is_pgm_bin || is_ppm_bin) {
        // Binary format
        size_t needed = static_cast<size_t>(width) * height * src_channels;
        if (pos + needed > raw.size()) {
            NX_ERROR("TextureImporter: PPM/PGM pixel data truncated in '{}'", filename);
            return nullptr;
        }

        for (u32 y = 0; y < height; ++y) {
            u8* dst = data->pixels.data() + static_cast<size_t>(y) * width * 4;
            for (u32 x = 0; x < width; ++x) {
                if (is_grayscale) {
                    u8 g = raw[pos++];
                    dst[x * 4 + 0] = g;
                    dst[x * 4 + 1] = g;
                    dst[x * 4 + 2] = g;
                } else {
                    dst[x * 4 + 0] = raw[pos++]; // R
                    dst[x * 4 + 1] = raw[pos++]; // G
                    dst[x * 4 + 2] = raw[pos++]; // B
                }
                dst[x * 4 + 3] = 255;
            }
        }
    } else {
        // Text format (P2/P3)
        for (u32 y = 0; y < height; ++y) {
            u8* dst = data->pixels.data() + static_cast<size_t>(y) * width * 4;
            for (u32 x = 0; x < width; ++x) {
                if (is_grayscale) {
                    u8 g = static_cast<u8>(read_int());
                    dst[x * 4 + 0] = g;
                    dst[x * 4 + 1] = g;
                    dst[x * 4 + 2] = g;
                } else {
                    dst[x * 4 + 0] = static_cast<u8>(read_int()); // R
                    dst[x * 4 + 1] = static_cast<u8>(read_int()); // G
                    dst[x * 4 + 2] = static_cast<u8>(read_int()); // B
                }
                dst[x * 4 + 3] = 255;
            }
        }
    }

    NX_INFO("TextureImporter: loaded {} '{}' ({}x{}, {} -> RGBA)",
            is_grayscale ? "PGM" : "PPM", filename, width, height,
            (is_pgm_bin || is_ppm_bin) ? "binary" : "text");
    return data;
}

// ── TextureImporter::import ─────────────────────────────────────────────────

std::shared_ptr<AssetData> TextureImporter::import(const std::string& path,
                                                     const AssetMeta& /*meta*/) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        NX_ERROR("TextureImporter: failed to open '{}'", path);
        return nullptr;
    }

    std::vector<u8> raw(std::istreambuf_iterator<char>(file),
                        std::istreambuf_iterator<char>{});

    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string filename = p.filename().string();

    if (ext == ".bmp") {
        return decode_bmp(raw, filename);
    }

    if (ext == ".tga") {
        return decode_tga(raw, filename);
    }

    if (ext == ".ppm" || ext == ".pgm") {
        return decode_ppm_pgm(raw, filename);
    }

    if (ext == ".png") {
        NX_ERROR("TextureImporter: PNG decoding requires stb_image.h. "
                 "Place stb_image.h in your include path and define "
                 "NX_HAS_STB_IMAGE to enable PNG support. File: '{}'", path);
        return nullptr;
    }

    if (ext == ".jpg" || ext == ".jpeg") {
        NX_ERROR("TextureImporter: JPEG decoding requires stb_image.h. "
                 "Place stb_image.h in your include path and define "
                 "NX_HAS_STB_IMAGE to enable JPEG support. File: '{}'", path);
        return nullptr;
    }

    if (ext == ".hdr") {
        NX_ERROR("TextureImporter: HDR decoding requires stb_image.h. "
                 "Place stb_image.h in your include path and define "
                 "NX_HAS_STB_IMAGE to enable HDR support. File: '{}'", path);
        return nullptr;
    }

    NX_ERROR("TextureImporter: unsupported texture format '{}' for file '{}'", ext, path);
    return nullptr;
}

// ── MeshImporter ────────────────────────────────────────────────────────────

std::vector<std::string> MeshImporter::supported_extensions() const {
    return {".gltf", ".glb", ".obj"};
}

std::shared_ptr<AssetData> MeshImporter::import(const std::string& path,
                                                  const AssetMeta& /*meta*/) {
    std::ifstream file(path);
    if (!file.is_open()) {
        NX_ERROR("MeshImporter: failed to open {}", path);
        return nullptr;
    }

    auto data = std::make_shared<MeshData>();
    std::filesystem::path p(path);
    data->name = p.stem().string();
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".obj") {
        // Wavefront OBJ parser
        std::vector<std::array<f32, 3>> positions;
        std::vector<std::array<f32, 3>> normals;
        std::vector<std::array<f32, 2>> texcoords;

        // Map of "v/vt/vn" -> index for deduplication
        std::unordered_map<std::string, u32> vertex_map;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream iss(line);
            std::string token;
            iss >> token;

            if (token == "o" || token == "g") {
                std::string obj_name;
                if (iss >> obj_name) {
                    data->name = obj_name;
                }
            } else if (token == "v") {
                std::array<f32, 3> pos{};
                iss >> pos[0] >> pos[1] >> pos[2];
                positions.push_back(pos);
            } else if (token == "vn") {
                std::array<f32, 3> n{};
                iss >> n[0] >> n[1] >> n[2];
                normals.push_back(n);
            } else if (token == "vt") {
                std::array<f32, 2> uv{};
                iss >> uv[0] >> uv[1];
                texcoords.push_back(uv);
            } else if (token == "f") {
                // Parse face vertices (triangulate quads)
                std::vector<u32> face_indices;
                std::string face_token;
                while (iss >> face_token) {
                    auto it = vertex_map.find(face_token);
                    if (it != vertex_map.end()) {
                        face_indices.push_back(it->second);
                        continue;
                    }

                    MeshData::Vertex vert{};
                    // Parse v, v/vt, v/vt/vn, v//vn
                    int vi = 0, ti = 0, ni = 0;
                    if (std::sscanf(face_token.c_str(), "%d/%d/%d", &vi, &ti, &ni) == 3 ||
                        std::sscanf(face_token.c_str(), "%d//%d", &vi, &ni) == 2 ||
                        std::sscanf(face_token.c_str(), "%d/%d", &vi, &ti) == 2 ||
                        std::sscanf(face_token.c_str(), "%d", &vi) == 1) {

                        if (vi != 0) {
                            size_t idx = (vi > 0) ? size_t(vi - 1) : positions.size() + size_t(vi);
                            if (idx < positions.size()) {
                                vert.position[0] = positions[idx][0];
                                vert.position[1] = positions[idx][1];
                                vert.position[2] = positions[idx][2];
                            }
                        }
                        if (ni != 0) {
                            size_t idx = (ni > 0) ? size_t(ni - 1) : normals.size() + size_t(ni);
                            if (idx < normals.size()) {
                                vert.normal[0] = normals[idx][0];
                                vert.normal[1] = normals[idx][1];
                                vert.normal[2] = normals[idx][2];
                            }
                        }
                        if (ti != 0) {
                            size_t idx = (ti > 0) ? size_t(ti - 1) : texcoords.size() + size_t(ti);
                            if (idx < texcoords.size()) {
                                vert.texcoord[0] = texcoords[idx][0];
                                vert.texcoord[1] = texcoords[idx][1];
                            }
                        }
                    }

                    u32 new_idx = static_cast<u32>(data->vertices.size());
                    data->vertices.push_back(vert);
                    vertex_map[face_token] = new_idx;
                    face_indices.push_back(new_idx);
                }

                // Triangulate (fan triangulation for convex polygons)
                for (size_t i = 2; i < face_indices.size(); ++i) {
                    data->indices.push_back(face_indices[0]);
                    data->indices.push_back(face_indices[i - 1]);
                    data->indices.push_back(face_indices[i]);
                }
            }
        }

        // Generate flat normals if none were provided
        if (normals.empty() && data->indices.size() >= 3) {
            for (size_t i = 0; i + 2 < data->indices.size(); i += 3) {
                auto& v0 = data->vertices[data->indices[i]];
                auto& v1 = data->vertices[data->indices[i + 1]];
                auto& v2 = data->vertices[data->indices[i + 2]];

                f32 e1[3] = {v1.position[0] - v0.position[0],
                             v1.position[1] - v0.position[1],
                             v1.position[2] - v0.position[2]};
                f32 e2[3] = {v2.position[0] - v0.position[0],
                             v2.position[1] - v0.position[1],
                             v2.position[2] - v0.position[2]};
                f32 n[3] = {e1[1]*e2[2] - e1[2]*e2[1],
                            e1[2]*e2[0] - e1[0]*e2[2],
                            e1[0]*e2[1] - e1[1]*e2[0]};
                f32 len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
                if (len > 0.0f) { n[0] /= len; n[1] /= len; n[2] /= len; }

                for (int k = 0; k < 3; ++k) {
                    auto& v = data->vertices[data->indices[i + size_t(k)]];
                    v.normal[0] = n[0]; v.normal[1] = n[1]; v.normal[2] = n[2];
                }
            }
        }

        NX_INFO("MeshImporter: loaded OBJ '{}' ({} verts, {} tris)",
                data->name, data->vertices.size(), data->indices.size() / 3);
    } else if (ext == ".gltf" || ext == ".glb") {
        // Minimal glTF 2.0 JSON parser for mesh data
        // Reads the first mesh/primitive from a .gltf file (JSON-based)
        std::string json_str;
        std::vector<u8> glb_bin;

        if (ext == ".glb") {
            // GLB format: 12-byte header + JSON chunk + BIN chunk
            file.seekg(0, std::ios::end);
            size_t file_size = static_cast<size_t>(file.tellg());
            file.seekg(0);
            if (file_size < 20) {
                NX_ERROR("MeshImporter: GLB file too small");
                return nullptr;
            }
            u32 magic, version, length;
            file.read(reinterpret_cast<char*>(&magic), 4);
            file.read(reinterpret_cast<char*>(&version), 4);
            file.read(reinterpret_cast<char*>(&length), 4);
            if (magic != 0x46546C67) { // 'glTF'
                NX_ERROR("MeshImporter: invalid GLB magic");
                return nullptr;
            }
            // JSON chunk. Chunk lengths come from an untrusted file, so validate
            // them against the bytes actually remaining before allocating.
            u32 json_len, json_type;
            file.read(reinterpret_cast<char*>(&json_len), 4);
            file.read(reinterpret_cast<char*>(&json_type), 4);
            if (json_len > file_size - static_cast<size_t>(file.tellg())) {
                NX_ERROR("MeshImporter: GLB JSON chunk length {} exceeds file size", json_len);
                return nullptr;
            }
            json_str.resize(json_len);
            file.read(json_str.data(), json_len);
            // BIN chunk
            if (static_cast<size_t>(file.tellg()) + 8 <= file_size) {
                u32 bin_len, bin_type;
                file.read(reinterpret_cast<char*>(&bin_len), 4);
                file.read(reinterpret_cast<char*>(&bin_type), 4);
                if (bin_len > file_size - static_cast<size_t>(file.tellg())) {
                    NX_ERROR("MeshImporter: GLB BIN chunk length {} exceeds file size", bin_len);
                    return nullptr;
                }
                glb_bin.resize(bin_len);
                file.read(reinterpret_cast<char*>(glb_bin.data()), bin_len);
            }
        } else {
            // Plain .gltf JSON
            std::ostringstream oss;
            oss << file.rdbuf();
            json_str = oss.str();
        }

        // Minimal JSON value extraction helpers
        auto find_array = [&](const std::string& json, const std::string& key) -> std::string {
            size_t pos = json.find("\"" + key + "\"");
            if (pos == std::string::npos) return "";
            pos = json.find('[', pos);
            if (pos == std::string::npos) return "";
            int depth = 0;
            size_t start = pos;
            for (size_t i = pos; i < json.size(); ++i) {
                if (json[i] == '[') depth++;
                else if (json[i] == ']') { depth--; if (depth == 0) return json.substr(start, i - start + 1); }
            }
            return "";
        };

        auto find_int = [&](const std::string& json, const std::string& key) -> i64 {
            size_t pos = json.find("\"" + key + "\"");
            if (pos == std::string::npos) return -1;
            pos = json.find(':', pos);
            if (pos == std::string::npos) return -1;
            pos++;
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
            return std::stoll(json.substr(pos));
        };

        auto find_object_at = [&](const std::string& arr, size_t start) -> std::pair<std::string, size_t> {
            size_t pos = arr.find('{', start);
            if (pos == std::string::npos) return {"", std::string::npos};
            int depth = 0;
            size_t s = pos;
            for (size_t i = pos; i < arr.size(); ++i) {
                if (arr[i] == '{') depth++;
                else if (arr[i] == '}') { depth--; if (depth == 0) return {arr.substr(s, i - s + 1), i + 1}; }
            }
            return {"", std::string::npos};
        };

        // Parse accessors, bufferViews
        std::string accessors_arr = find_array(json_str, "accessors");
        std::string views_arr = find_array(json_str, "bufferViews");
        std::string meshes_arr = find_array(json_str, "meshes");

        struct AccessorInfo { i64 view; i64 count; i64 comp_type; std::string type; i64 byte_offset; };
        struct BufferViewInfo { i64 buffer; i64 offset; i64 length; i64 stride; };

        std::vector<AccessorInfo> accessors;
        std::vector<BufferViewInfo> buffer_views;

        // Parse buffer views
        {
            size_t pos = 0;
            while (true) {
                auto [obj, next] = find_object_at(views_arr, pos);
                if (next == std::string::npos) break;
                BufferViewInfo bv{};
                bv.buffer = find_int(obj, "buffer");
                bv.offset = find_int(obj, "byteOffset");
                if (bv.offset < 0) bv.offset = 0;
                bv.length = find_int(obj, "byteLength");
                bv.stride = find_int(obj, "byteStride");
                if (bv.stride < 0) bv.stride = 0;
                buffer_views.push_back(bv);
                pos = next;
            }
        }

        // Parse accessors
        {
            size_t pos = 0;
            while (true) {
                auto [obj, next] = find_object_at(accessors_arr, pos);
                if (next == std::string::npos) break;
                AccessorInfo acc{};
                acc.view = find_int(obj, "bufferView");
                acc.count = find_int(obj, "count");
                acc.comp_type = find_int(obj, "componentType");
                acc.byte_offset = find_int(obj, "byteOffset");
                if (acc.byte_offset < 0) acc.byte_offset = 0;
                // Determine type
                size_t tp = obj.find("\"type\"");
                if (tp != std::string::npos) {
                    size_t q1 = obj.find('"', tp + 6);
                    if (q1 != std::string::npos) {
                        size_t q2 = obj.find('"', q1 + 1);
                        if (q2 != std::string::npos) acc.type = obj.substr(q1 + 1, q2 - q1 - 1);
                    }
                }
                accessors.push_back(acc);
                pos = next;
            }
        }

        // Parse first mesh/primitive
        auto [mesh_obj, _m] = find_object_at(meshes_arr, 0);
        std::string prims_arr = find_array(mesh_obj, "primitives");
        auto [prim_obj, _p] = find_object_at(prims_arr, 0);

        i64 idx_accessor = find_int(prim_obj, "indices");
        // Find POSITION, NORMAL, TEXCOORD_0 in attributes
        std::string attrs_str;
        size_t attr_pos = prim_obj.find("\"attributes\"");
        if (attr_pos != std::string::npos) {
            auto [attr_obj, _a] = find_object_at(prim_obj, attr_pos);
            attrs_str = attr_obj;
        }
        i64 pos_accessor = find_int(attrs_str, "POSITION");
        i64 norm_accessor = find_int(attrs_str, "NORMAL");
        i64 uv_accessor = find_int(attrs_str, "TEXCOORD_0");

        // Helper to read float data from buffer
        auto read_floats = [&](i64 acc_idx, u32 components) -> std::vector<f32> {
            std::vector<f32> result;
            if (acc_idx < 0 || static_cast<size_t>(acc_idx) >= accessors.size()) return result;
            const auto& acc = accessors[static_cast<size_t>(acc_idx)];
            if (acc.view < 0 || static_cast<size_t>(acc.view) >= buffer_views.size()) return result;
            const auto& bv = buffer_views[static_cast<size_t>(acc.view)];

            const u8* buf_data = glb_bin.data();
            size_t buf_size = glb_bin.size();
            if (!buf_data || buf_size == 0) return result;

            size_t offset = static_cast<size_t>(bv.offset + acc.byte_offset);
            size_t stride = bv.stride > 0 ? static_cast<size_t>(bv.stride) : (components * sizeof(f32));

            result.reserve(static_cast<size_t>(acc.count) * components);
            for (i64 i = 0; i < acc.count; ++i) {
                size_t base = offset + static_cast<size_t>(i) * stride;
                for (u32 c = 0; c < components; ++c) {
                    f32 val = 0.0f;
                    if (base + (c + 1) * sizeof(f32) <= buf_size) {
                        std::memcpy(&val, buf_data + base + c * sizeof(f32), sizeof(f32));
                    }
                    result.push_back(val);
                }
            }
            return result;
        };

        auto read_indices = [&](i64 acc_idx) -> std::vector<u32> {
            std::vector<u32> result;
            if (acc_idx < 0 || static_cast<size_t>(acc_idx) >= accessors.size()) return result;
            const auto& acc = accessors[static_cast<size_t>(acc_idx)];
            if (acc.view < 0 || static_cast<size_t>(acc.view) >= buffer_views.size()) return result;
            const auto& bv = buffer_views[static_cast<size_t>(acc.view)];

            const u8* buf_data = glb_bin.data();
            size_t buf_size = glb_bin.size();
            if (!buf_data || buf_size == 0) return result;

            size_t offset = static_cast<size_t>(bv.offset + acc.byte_offset);
            result.reserve(static_cast<size_t>(acc.count));

            for (i64 i = 0; i < acc.count; ++i) {
                u32 idx = 0;
                if (acc.comp_type == 5123) { // UNSIGNED_SHORT
                    u16 v = 0;
                    size_t o = offset + static_cast<size_t>(i) * sizeof(u16);
                    if (o + sizeof(u16) <= buf_size) std::memcpy(&v, buf_data + o, sizeof(u16));
                    idx = v;
                } else if (acc.comp_type == 5125) { // UNSIGNED_INT
                    size_t o = offset + static_cast<size_t>(i) * sizeof(u32);
                    if (o + sizeof(u32) <= buf_size) std::memcpy(&idx, buf_data + o, sizeof(u32));
                } else if (acc.comp_type == 5121) { // UNSIGNED_BYTE
                    size_t o = offset + static_cast<size_t>(i);
                    if (o < buf_size) idx = buf_data[o];
                }
                result.push_back(idx);
            }
            return result;
        };

        // Read vertex data
        auto positions = read_floats(pos_accessor, 3);
        auto norms = read_floats(norm_accessor, 3);
        auto uvs = read_floats(uv_accessor, 2);
        auto indices = read_indices(idx_accessor);

        u32 vert_count = static_cast<u32>(positions.size() / 3);
        data->vertices.reserve(vert_count);
        for (u32 i = 0; i < vert_count; ++i) {
            MeshData::Vertex v{};
            v.position[0] = positions[i * 3];
            v.position[1] = positions[i * 3 + 1];
            v.position[2] = positions[i * 3 + 2];
            if (i * 3 + 2 < norms.size()) {
                v.normal[0] = norms[i * 3];
                v.normal[1] = norms[i * 3 + 1];
                v.normal[2] = norms[i * 3 + 2];
            }
            if (i * 2 + 1 < uvs.size()) {
                v.texcoord[0] = uvs[i * 2];
                v.texcoord[1] = uvs[i * 2 + 1];
            }
            data->vertices.push_back(v);
        }

        if (!indices.empty()) {
            data->indices = std::move(indices);
        } else {
            data->indices.resize(vert_count);
            for (u32 i = 0; i < vert_count; ++i) data->indices[i] = i;
        }

        NX_INFO("MeshImporter: loaded glTF '{}' ({} verts, {} tris)",
                data->name, data->vertices.size(), data->indices.size() / 3);
    } else {
        NX_ERROR("MeshImporter: unsupported mesh format '{}' — "
                 "only .obj, .gltf, and .glb are supported", ext);
        return nullptr;
    }

    return data;
}

// ── AudioImporter ───────────────────────────────────────────────────────────

std::vector<std::string> AudioImporter::supported_extensions() const {
    return {".wav", ".ogg", ".mp3", ".flac"};
}

std::shared_ptr<AssetData> AudioImporter::import(const std::string& path,
                                                   const AssetMeta& /*meta*/) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        NX_ERROR("AudioImporter: failed to open '{}'", path);
        return nullptr;
    }

    std::vector<u8> raw(std::istreambuf_iterator<char>(file),
                        std::istreambuf_iterator<char>{});

    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext != ".wav") {
        NX_ERROR("AudioImporter: format '{}' not supported for '{}'. "
                 "Only WAV files are currently supported.", ext, path);
        return nullptr;
    }

    // ── WAV parser ──────────────────────────────────────────────────────
    // RIFF/WAVE format: RIFF header -> "WAVE" -> chunks (fmt, data, etc.)

    auto read_u32_le = [&](size_t off) -> u32 {
        return u32(raw[off]) | (u32(raw[off+1]) << 8) |
               (u32(raw[off+2]) << 16) | (u32(raw[off+3]) << 24);
    };
    auto read_u16_le = [&](size_t off) -> u16 {
        return static_cast<u16>(u16(raw[off]) | (u16(raw[off+1]) << 8));
    };

    // Validate RIFF header
    if (raw.size() < 44) {
        NX_ERROR("AudioImporter: WAV file too small in '{}'", path);
        return nullptr;
    }
    if (raw[0] != 'R' || raw[1] != 'I' || raw[2] != 'F' || raw[3] != 'F') {
        NX_ERROR("AudioImporter: missing RIFF header in '{}'", path);
        return nullptr;
    }
    if (raw[8] != 'W' || raw[9] != 'A' || raw[10] != 'V' || raw[11] != 'E') {
        NX_ERROR("AudioImporter: missing WAVE identifier in '{}'", path);
        return nullptr;
    }

    // Walk chunks to find 'fmt ' and 'data'
    auto data = std::make_shared<AudioData>();
    bool found_fmt = false;
    bool found_data = false;
    size_t pos = 12; // past RIFF header + "WAVE"

    while (pos + 8 <= raw.size()) {
        char chunk_id[5] = {};
        std::memcpy(chunk_id, raw.data() + pos, 4);
        u32 chunk_size = read_u32_le(pos + 4);
        size_t chunk_data_start = pos + 8;

        if (std::strncmp(chunk_id, "fmt ", 4) == 0) {
            if (chunk_data_start + 16 > raw.size()) {
                NX_ERROR("AudioImporter: fmt chunk truncated in '{}'", path);
                return nullptr;
            }
            u16 audio_format = read_u16_le(chunk_data_start);
            if (audio_format != 1) {
                NX_ERROR("AudioImporter: only PCM format supported (got {}), file '{}'",
                         audio_format, path);
                return nullptr;
            }
            data->channels = read_u16_le(chunk_data_start + 2);
            data->sample_rate = read_u32_le(chunk_data_start + 4);
            // bytes 8-11: byte rate, bytes 12-13: block align
            data->bits_per_sample = read_u16_le(chunk_data_start + 14);

            if (data->channels == 0 || data->sample_rate == 0 || data->bits_per_sample == 0) {
                NX_ERROR("AudioImporter: invalid fmt chunk values in '{}'", path);
                return nullptr;
            }
            found_fmt = true;
        } else if (std::strncmp(chunk_id, "data", 4) == 0) {
            if (chunk_data_start + chunk_size > raw.size()) {
                // Allow partial data reads (file may be truncated)
                chunk_size = static_cast<u32>(raw.size() - chunk_data_start);
            }
            data->samples.assign(raw.begin() + static_cast<std::ptrdiff_t>(chunk_data_start),
                                 raw.begin() + static_cast<std::ptrdiff_t>(chunk_data_start + chunk_size));
            found_data = true;
        }

        // Advance to next chunk (chunks are 2-byte aligned)
        pos = chunk_data_start + chunk_size;
        if (pos % 2 != 0) ++pos;

        if (found_fmt && found_data) break;
    }

    if (!found_fmt) {
        NX_ERROR("AudioImporter: no fmt chunk found in '{}'", path);
        return nullptr;
    }
    if (!found_data) {
        NX_ERROR("AudioImporter: no data chunk found in '{}'", path);
        return nullptr;
    }

    // Calculate duration from sample count
    u32 bytes_per_sample = (data->bits_per_sample / 8) * data->channels;
    if (bytes_per_sample > 0 && data->sample_rate > 0) {
        u64 total_frames = data->samples.size() / bytes_per_sample;
        data->duration = static_cast<f32>(total_frames) /
                         static_cast<f32>(data->sample_rate);
    }

    NX_INFO("AudioImporter: loaded WAV '{}' ({}Hz, {}ch, {}bit, {:.2f}s)",
            p.filename().string(), data->sample_rate, data->channels,
            data->bits_per_sample, data->duration);
    return data;
}

// ── ShaderImporter ──────────────────────────────────────────────────────────

std::vector<std::string> ShaderImporter::supported_extensions() const {
    return {".glsl", ".vert", ".frag", ".comp", ".spv"};
}

std::shared_ptr<AssetData> ShaderImporter::import(const std::string& path,
                                                    const AssetMeta& /*meta*/) {
    std::ifstream file(path);
    if (!file.is_open()) {
        NX_ERROR("ShaderImporter: failed to open {}", path);
        return nullptr;
    }

    auto data = std::make_shared<ShaderData>();
    std::stringstream buf;
    buf << file.rdbuf();
    std::string source = buf.str();

    std::filesystem::path p(path);
    auto ext = p.extension().string();

    if (ext == ".vert") {
        data->vertex_source = source;
    } else if (ext == ".frag") {
        data->fragment_source = source;
    } else if (ext == ".comp") {
        data->compute_source = source;
    } else {
        // .glsl — treat as combined vertex+fragment
        data->vertex_source = source;
        data->fragment_source = source;
    }

    return data;
}

// ── ScriptImporter ──────────────────────────────────────────────────────────

std::vector<std::string> ScriptImporter::supported_extensions() const {
    return {".lua", ".nxs"};
}

std::shared_ptr<AssetData> ScriptImporter::import(const std::string& path,
                                                    const AssetMeta& /*meta*/) {
    std::ifstream file(path);
    if (!file.is_open()) {
        NX_ERROR("ScriptImporter: failed to open {}", path);
        return nullptr;
    }

    auto data = std::make_shared<ScriptData>();
    std::stringstream buf;
    buf << file.rdbuf();
    data->source = buf.str();

    return data;
}

// ── MaterialImporter ────────────────────────────────────────────────────────

std::vector<std::string> MaterialImporter::supported_extensions() const {
    return {".mat", ".material"};
}

std::shared_ptr<AssetData> MaterialImporter::import(const std::string& path,
                                                      const AssetMeta& /*meta*/) {
    std::ifstream file(path);
    if (!file.is_open()) {
        NX_ERROR("MaterialImporter: failed to open {}", path);
        return nullptr;
    }

    auto data = std::make_shared<MaterialData>();

    // Simple key-value parsing (real engine would use JSON)
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Trim spaces
        auto trim = [](std::string& s) {
            auto start = s.find_first_not_of(" \t");
            auto end = s.find_last_not_of(" \t");
            s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
        };
        trim(key);
        trim(val);

        if (key == "shader") data->shader_path = val;
        else if (key == "albedo") data->albedo_texture = val;
        else if (key == "normal") data->normal_texture = val;
        else if (key == "metallic_roughness") data->metallic_roughness_texture = val;
        else if (key == "metallic") data->metallic = std::stof(val);
        else if (key == "roughness") data->roughness = std::stof(val);
    }

    return data;
}

// ── AssetLoader ─────────────────────────────────────────────────────────────

AssetLoader::AssetLoader(AssetRegistry& registry) : registry_(registry) {}
AssetLoader::~AssetLoader() = default;

void AssetLoader::register_importer(std::unique_ptr<AssetImporter> importer) {
    importers_.push_back(std::move(importer));
}

void AssetLoader::register_default_importers() {
    register_importer(std::make_unique<TextureImporter>());
    register_importer(std::make_unique<MeshImporter>());
    register_importer(std::make_unique<AudioImporter>());
    register_importer(std::make_unique<ShaderImporter>());
    register_importer(std::make_unique<ScriptImporter>());
    register_importer(std::make_unique<MaterialImporter>());
}

bool AssetLoader::load_sync(AssetId id) {
    return do_load(id);
}

bool AssetLoader::load_sync(const std::string& path) {
    auto* meta = registry_.find_by_path(path);
    if (!meta) return false;
    return do_load(meta->id);
}

void AssetLoader::load_async(AssetId id, u32 priority) {
    load_queue_.push({id, priority});
    progress_.total++;
}

void AssetLoader::load_async(const std::string& path, u32 priority) {
    auto* meta = registry_.find_by_path(path);
    if (!meta) return;
    load_async(meta->id, priority);
}

bool AssetLoader::process_one() {
    if (load_queue_.empty()) return false;

    auto request = load_queue_.top();
    load_queue_.pop();

    progress_.current_asset = request.id;
    bool ok = do_load(request.id);

    if (ok) {
        progress_.completed++;
    } else {
        progress_.failed++;
    }

    if (progress_cb_) {
        progress_cb_(progress_);
    }

    return true;
}

u32 AssetLoader::process_all() {
    u32 processed = 0;
    while (process_one()) {
        ++processed;
    }
    return processed;
}

ProgressInfo AssetLoader::progress() const {
    return progress_;
}

u32 AssetLoader::importer_count() const {
    return static_cast<u32>(importers_.size());
}

AssetImporter* AssetLoader::find_importer(const std::string& extension) const {
    for (auto& imp : importers_) {
        if (imp->supports(extension)) return imp.get();
    }
    return nullptr;
}

AssetImporter* AssetLoader::find_importer_for_type(AssetType type) const {
    for (auto& imp : importers_) {
        if (imp->handled_type() == type) return imp.get();
    }
    return nullptr;
}

bool AssetLoader::do_load(AssetId id) {
    auto* meta = registry_.find(id);
    if (!meta) {
        NX_ERROR("AssetLoader: unknown asset id {}", id.value);
        return false;
    }

    if (meta->status == AssetStatus::Loaded) return true;

    meta->status = AssetStatus::Loading;

    // Find the right importer
    std::filesystem::path p(meta->source_path);
    std::string ext = p.has_extension() ? p.extension().string() : "";

    AssetImporter* importer = nullptr;
    if (!ext.empty()) {
        importer = find_importer(ext);
    }
    if (!importer && meta->type != AssetType::Unknown) {
        importer = find_importer_for_type(meta->type);
    }

    if (!importer) {
        NX_ERROR("AssetLoader: no importer for '{}' ({})", meta->path, ext);
        meta->status = AssetStatus::Failed;
        return false;
    }

    auto data = importer->import(meta->source_path, *meta);
    if (!data) {
        meta->status = AssetStatus::Failed;
        return false;
    }

    registry_.store_data(id, std::move(data));
    return true;
}

} // namespace nexus::assets
