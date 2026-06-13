// ============================================================================
// forward_renderer_3d.cpp - Multi-light forward rendering pipeline
// ============================================================================

#include <nexus/renderer/forward_renderer_3d.h>
#include <nexus/core/log.h>
#include <cmath>

namespace nexus {

// ── Default 3D shaders ──────────────────────────────────────────────────────

static const char* FORWARD_VERTEX_SHADER = R"(
#version 330 core
layout (location = 0) in vec3 a_Position;
layout (location = 1) in vec3 a_Normal;
layout (location = 2) in vec2 a_TexCoord;

out vec3 v_FragPos;
out vec3 v_Normal;
out vec2 v_TexCoord;

uniform mat4 u_ViewProjection;
uniform mat4 u_Model;
uniform mat4 u_NormalMatrix;

void main() {
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    v_FragPos = worldPos.xyz;
    v_Normal = mat3(u_NormalMatrix) * a_Normal;
    v_TexCoord = a_TexCoord;
    gl_Position = u_ViewProjection * worldPos;
}
)";

static const char* FORWARD_FRAGMENT_SHADER = R"(
#version 330 core
in vec3 v_FragPos;
in vec3 v_Normal;
in vec2 v_TexCoord;

out vec4 FragColor;

uniform vec4 u_Color;
uniform sampler2D u_Texture;
uniform vec3 u_CameraPos;

// Directional light
uniform vec3 u_DirLight_Direction;
uniform vec3 u_DirLight_Color;
uniform float u_DirLight_Intensity;

// Point lights
#define MAX_POINT_LIGHTS 8
uniform int u_NumPointLights;
uniform vec3 u_PointLight_Position[MAX_POINT_LIGHTS];
uniform vec3 u_PointLight_Color[MAX_POINT_LIGHTS];
uniform float u_PointLight_Intensity[MAX_POINT_LIGHTS];
uniform float u_PointLight_Radius[MAX_POINT_LIGHTS];

// Spot lights
#define MAX_SPOT_LIGHTS 4
uniform int u_NumSpotLights;
uniform vec3 u_SpotLight_Position[MAX_SPOT_LIGHTS];
uniform vec3 u_SpotLight_Direction[MAX_SPOT_LIGHTS];
uniform vec3 u_SpotLight_Color[MAX_SPOT_LIGHTS];
uniform float u_SpotLight_Intensity[MAX_SPOT_LIGHTS];
uniform float u_SpotLight_Range[MAX_SPOT_LIGHTS];
uniform float u_SpotLight_InnerCos[MAX_SPOT_LIGHTS];
uniform float u_SpotLight_OuterCos[MAX_SPOT_LIGHTS];

vec3 calcDirectionalLight(vec3 normal, vec3 viewDir) {
    vec3 lightDir = normalize(-u_DirLight_Direction);

    // Diffuse
    float diff = max(dot(normal, lightDir), 0.0);

    // Specular (Blinn-Phong)
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), 32.0);

    vec3 ambient  = 0.1 * u_DirLight_Color;
    vec3 diffuse  = diff * u_DirLight_Color;
    vec3 specular = spec * 0.5 * u_DirLight_Color;

    return (ambient + diffuse + specular) * u_DirLight_Intensity;
}

vec3 calcPointLight(int i, vec3 normal, vec3 fragPos, vec3 viewDir) {
    vec3 lightDir = u_PointLight_Position[i] - fragPos;
    float distance = length(lightDir);
    lightDir = normalize(lightDir);

    // Attenuation
    float attenuation = 1.0 / (1.0 + (distance / u_PointLight_Radius[i]) *
                                       (distance / u_PointLight_Radius[i]));

    // Diffuse
    float diff = max(dot(normal, lightDir), 0.0);

    // Specular
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), 32.0);

    vec3 diffuse  = diff * u_PointLight_Color[i];
    vec3 specular = spec * 0.5 * u_PointLight_Color[i];

    return (diffuse + specular) * attenuation * u_PointLight_Intensity[i];
}

vec3 calcSpotLight(int i, vec3 normal, vec3 fragPos, vec3 viewDir) {
    vec3 lightDir = u_SpotLight_Position[i] - fragPos;
    float distance = length(lightDir);
    lightDir = normalize(lightDir);

    // Attenuation
    float attenuation = 1.0 / (1.0 + (distance / u_SpotLight_Range[i]) *
                                       (distance / u_SpotLight_Range[i]));

    // Spotlight cone
    float theta = dot(lightDir, normalize(-u_SpotLight_Direction[i]));
    float epsilon = u_SpotLight_InnerCos[i] - u_SpotLight_OuterCos[i];
    float spotIntensity = clamp((theta - u_SpotLight_OuterCos[i]) / max(epsilon, 0.001), 0.0, 1.0);

    // Diffuse
    float diff = max(dot(normal, lightDir), 0.0);

    // Specular
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), 32.0);

    vec3 diffuse  = diff * u_SpotLight_Color[i];
    vec3 specular = spec * 0.5 * u_SpotLight_Color[i];

    return (diffuse + specular) * attenuation * spotIntensity * u_SpotLight_Intensity[i];
}

void main() {
    vec3 normal = normalize(v_Normal);
    vec3 viewDir = normalize(u_CameraPos - v_FragPos);

    vec3 result = calcDirectionalLight(normal, viewDir);

    for (int i = 0; i < u_NumPointLights; ++i) {
        result += calcPointLight(i, normal, v_FragPos, viewDir);
    }

    for (int i = 0; i < u_NumSpotLights; ++i) {
        result += calcSpotLight(i, normal, v_FragPos, viewDir);
    }

    vec4 texColor = texture(u_Texture, v_TexCoord);
    FragColor = vec4(result, 1.0) * texColor * u_Color;
}
)";

// ── Lifecycle ───────────────────────────────────────────────────────────────

void ForwardRenderer3D::init(rhi::RHI* rhi) {
    rhi_ = rhi;

    shader_ = rhi_->create_shader(FORWARD_VERTEX_SHADER, FORWARD_FRAGMENT_SHADER);
    if (shader_ == rhi::INVALID_HANDLE) {
        NX_ERROR("ForwardRenderer3D: Failed to compile shaders");
        return;
    }

    // Create 1x1 white texture
    u32 white_pixel = 0xFFFFFFFF;
    rhi::TextureDesc white_desc;
    white_desc.width  = 1;
    white_desc.height = 1;
    white_desc.format = rhi::TextureFormat::RGBA8;
    white_desc.min_filter = rhi::TextureFilter::Nearest;
    white_desc.mag_filter = rhi::TextureFilter::Nearest;
    white_desc.generate_mipmaps = false;
    white_desc.data = &white_pixel;
    white_texture_ = rhi_->create_texture(white_desc);

    NX_INFO("ForwardRenderer3D initialized");
}

ForwardRenderer3D::ForwardRenderer3D(ForwardRenderer3D&& other) noexcept
    : rhi_(other.rhi_), shader_(other.shader_), white_texture_(other.white_texture_),
      dir_light_(other.dir_light_), point_lights_(std::move(other.point_lights_)),
      spot_lights_(std::move(other.spot_lights_)),
      view_projection_(other.view_projection_), camera_position_(other.camera_position_),
      in_frame_(other.in_frame_), shadow_map_(std::move(other.shadow_map_)) {
    for (int i = 0; i < 6; ++i) frustum_planes_[i] = other.frustum_planes_[i];
    other.rhi_ = nullptr;
    other.shader_ = rhi::INVALID_HANDLE;
    other.white_texture_ = rhi::INVALID_HANDLE;
    other.in_frame_ = false;
}

ForwardRenderer3D& ForwardRenderer3D::operator=(ForwardRenderer3D&& other) noexcept {
    if (this != &other) {
        shutdown();
        rhi_ = other.rhi_; shader_ = other.shader_; white_texture_ = other.white_texture_;
        dir_light_ = other.dir_light_; point_lights_ = std::move(other.point_lights_);
        spot_lights_ = std::move(other.spot_lights_);
        view_projection_ = other.view_projection_; camera_position_ = other.camera_position_;
        in_frame_ = other.in_frame_; shadow_map_ = std::move(other.shadow_map_);
        for (int i = 0; i < 6; ++i) frustum_planes_[i] = other.frustum_planes_[i];
        other.rhi_ = nullptr; other.shader_ = rhi::INVALID_HANDLE; other.white_texture_ = rhi::INVALID_HANDLE;
        other.in_frame_ = false;
    }
    return *this;
}

void ForwardRenderer3D::shutdown() {
    if (!rhi_) return;
    if (shader_ != rhi::INVALID_HANDLE) rhi_->destroy_shader(shader_);
    if (white_texture_ != rhi::INVALID_HANDLE) rhi_->destroy_texture(white_texture_);
    shader_ = rhi::INVALID_HANDLE;
    white_texture_ = rhi::INVALID_HANDLE;
    rhi_ = nullptr;
    in_frame_ = false;
}

// ── Frustum culling ─────────────────────────────────────────────────────────

void ForwardRenderer3D::extract_frustum_planes() {
    // Gribb/Hartmann method: extract planes from view-projection matrix
    const Mat4& m = view_projection_;
    // Left
    frustum_planes_[0] = Vec4(m[0][3]+m[0][0], m[1][3]+m[1][0], m[2][3]+m[2][0], m[3][3]+m[3][0]);
    // Right
    frustum_planes_[1] = Vec4(m[0][3]-m[0][0], m[1][3]-m[1][0], m[2][3]-m[2][0], m[3][3]-m[3][0]);
    // Bottom
    frustum_planes_[2] = Vec4(m[0][3]+m[0][1], m[1][3]+m[1][1], m[2][3]+m[2][1], m[3][3]+m[3][1]);
    // Top
    frustum_planes_[3] = Vec4(m[0][3]-m[0][1], m[1][3]-m[1][1], m[2][3]-m[2][1], m[3][3]-m[3][1]);
    // Near
    frustum_planes_[4] = Vec4(m[0][3]+m[0][2], m[1][3]+m[1][2], m[2][3]+m[2][2], m[3][3]+m[3][2]);
    // Far
    frustum_planes_[5] = Vec4(m[0][3]-m[0][2], m[1][3]-m[1][2], m[2][3]-m[2][2], m[3][3]-m[3][2]);

    // Normalize planes
    for (auto& plane : frustum_planes_) {
        float len = glm::length(Vec3(plane));
        if (len > 0.0f) plane /= len;
    }
}

bool ForwardRenderer3D::is_visible(Vec3 center, float radius) const {
    for (const auto& plane : frustum_planes_) {
        float dist = glm::dot(Vec3(plane), center) + plane.w;
        if (dist < -radius) return false;
    }
    return true;
}

// ── Frame scope ─────────────────────────────────────────────────────────────

void ForwardRenderer3D::begin_frame(const Camera3D& camera) {
    if (!rhi_ || shader_ == rhi::INVALID_HANDLE) return;

    current_camera_ = camera;
    view_projection_ = camera.get_view_projection();
    camera_position_ = camera.position;
    point_lights_.clear();
    spot_lights_.clear();
    in_frame_ = true;

    extract_frustum_planes();

    rhi_->set_depth_test(true);
    rhi_->bind_shader(shader_);
    rhi_->set_uniform_mat4(shader_, "u_ViewProjection", view_projection_);
    rhi_->set_uniform_vec3(shader_, "u_CameraPos", camera_position_);
}

void ForwardRenderer3D::end_frame() {
    in_frame_ = false;
}

void ForwardRenderer3D::set_directional_light(const DirectionalLight& light) {
    dir_light_ = light;
    rhi_->set_uniform_vec3(shader_, "u_DirLight_Direction", light.direction);
    rhi_->set_uniform_vec3(shader_, "u_DirLight_Color", light.color);
    rhi_->set_uniform_float(shader_, "u_DirLight_Intensity", light.intensity);
}

void ForwardRenderer3D::add_point_light(const PointLight& light) {
    if (point_lights_.size() >= MAX_POINT_LIGHTS) return;
    u32 idx = static_cast<u32>(point_lights_.size());
    point_lights_.push_back(light);

    std::string prefix = "u_PointLight_Position[" + std::to_string(idx) + "]";
    rhi_->set_uniform_vec3(shader_, prefix, light.position);

    prefix = "u_PointLight_Color[" + std::to_string(idx) + "]";
    rhi_->set_uniform_vec3(shader_, prefix, light.color);

    prefix = "u_PointLight_Intensity[" + std::to_string(idx) + "]";
    rhi_->set_uniform_float(shader_, prefix, light.intensity);

    prefix = "u_PointLight_Radius[" + std::to_string(idx) + "]";
    rhi_->set_uniform_float(shader_, prefix, light.radius);

    rhi_->set_uniform_int(shader_, "u_NumPointLights",
                          static_cast<i32>(point_lights_.size()));
}

void ForwardRenderer3D::add_spot_light(const SpotLight& light) {
    if (spot_lights_.size() >= MAX_SPOT_LIGHTS) return;
    u32 idx = static_cast<u32>(spot_lights_.size());
    spot_lights_.push_back(light);

    std::string si = std::to_string(idx);
    rhi_->set_uniform_vec3(shader_, "u_SpotLight_Position[" + si + "]", light.position);
    rhi_->set_uniform_vec3(shader_, "u_SpotLight_Direction[" + si + "]", light.direction);
    rhi_->set_uniform_vec3(shader_, "u_SpotLight_Color[" + si + "]", light.color);
    rhi_->set_uniform_float(shader_, "u_SpotLight_Intensity[" + si + "]", light.intensity);
    rhi_->set_uniform_float(shader_, "u_SpotLight_Range[" + si + "]", light.range);
    rhi_->set_uniform_float(shader_, "u_SpotLight_InnerCos[" + si + "]", light.inner_cos);
    rhi_->set_uniform_float(shader_, "u_SpotLight_OuterCos[" + si + "]", light.outer_cos);
    rhi_->set_uniform_int(shader_, "u_NumSpotLights", static_cast<i32>(spot_lights_.size()));
}

// ── Mesh management ─────────────────────────────────────────────────────────

void ForwardRenderer3D::upload_mesh(Mesh& mesh) {
    // VBO
    rhi::BufferDesc vbo_desc;
    vbo_desc.type  = rhi::BufferType::Vertex;
    vbo_desc.usage = rhi::BufferUsage::Static;
    vbo_desc.size  = mesh.vertices.size() * sizeof(MeshVertex);
    vbo_desc.data  = mesh.vertices.data();
    mesh.vbo = rhi_->create_buffer(vbo_desc);

    // IBO
    rhi::BufferDesc ibo_desc;
    ibo_desc.type  = rhi::BufferType::Index;
    ibo_desc.usage = rhi::BufferUsage::Static;
    ibo_desc.size  = mesh.indices.size() * sizeof(u32);
    ibo_desc.data  = mesh.indices.data();
    mesh.ibo = rhi_->create_buffer(ibo_desc);

    // Pipeline / VAO
    rhi::PipelineDesc pipe_desc;
    pipe_desc.shader     = shader_;
    pipe_desc.blend      = rhi::BlendMode::None;
    pipe_desc.depth_test = true;
    pipe_desc.cull       = rhi::CullMode::Back;
    pipe_desc.primitive  = rhi::PrimitiveType::Triangles;

    pipe_desc.vertex_layout.stride = sizeof(MeshVertex);
    pipe_desc.vertex_layout.attributes = {
        {0, 3, offsetof(MeshVertex, position), false},
        {1, 3, offsetof(MeshVertex, normal),   false},
        {2, 2, offsetof(MeshVertex, texcoord), false},
    };

    mesh.pipeline = rhi_->create_pipeline(pipe_desc);
}

void ForwardRenderer3D::destroy_mesh(Mesh& mesh) {
    if (!rhi_) return;
    if (mesh.pipeline != rhi::INVALID_HANDLE) rhi_->destroy_pipeline(mesh.pipeline);
    if (mesh.ibo != rhi::INVALID_HANDLE) rhi_->destroy_buffer(mesh.ibo);
    if (mesh.vbo != rhi::INVALID_HANDLE) rhi_->destroy_buffer(mesh.vbo);
    mesh.pipeline = rhi::INVALID_HANDLE;
    mesh.ibo = rhi::INVALID_HANDLE;
    mesh.vbo = rhi::INVALID_HANDLE;
}

void ForwardRenderer3D::draw_mesh(const Mesh& mesh, const Mat4& transform,
                                  Vec4 color, rhi::TextureHandle texture) {
    if (!in_frame_) return;

    // Frustum culling — compute bounding sphere from mesh transform
    Vec3 center = Vec3(transform[3]); // translation column
    float scale_max = std::max({glm::length(Vec3(transform[0])),
                                glm::length(Vec3(transform[1])),
                                glm::length(Vec3(transform[2]))});
    // Conservative radius estimate (unit cube diagonal ~0.866)
    float radius = scale_max * 0.866f;
    if (!is_visible(center, radius)) return;

    rhi_->bind_shader(shader_);
    rhi_->set_uniform_mat4(shader_, "u_Model", transform);

    // Normal matrix = transpose(inverse(model))
    Mat4 normal_matrix = glm::transpose(glm::inverse(transform));
    rhi_->set_uniform_mat4(shader_, "u_NormalMatrix", normal_matrix);

    rhi_->set_uniform_vec4(shader_, "u_Color", color);

    rhi::TextureHandle tex = (texture != rhi::INVALID_HANDLE) ? texture : white_texture_;
    rhi_->bind_texture(tex, 0);
    rhi_->set_uniform_int(shader_, "u_Texture", 0);

    rhi_->bind_pipeline(mesh.pipeline);
    rhi_->bind_vertex_buffer(mesh.vbo);
    rhi_->bind_index_buffer(mesh.ibo);
    rhi_->draw_indexed(static_cast<u32>(mesh.indices.size()));
}

// ── Shadow mapping ──────────────────────────────────────────────────────

void ForwardRenderer3D::enable_shadows(const CascadedShadowMap::Config& config) {
    if (!rhi_) return;
    shadow_map_ = std::make_unique<CascadedShadowMap>();
    shadow_map_->init(rhi_, config);
    NX_INFO("ForwardRenderer3D: Cascaded shadow mapping enabled ({} cascades, {}px)",
            config.num_cascades, config.resolution);
}

void ForwardRenderer3D::disable_shadows() {
    if (shadow_map_) {
        shadow_map_->shutdown();
        shadow_map_.reset();
    }
}

void ForwardRenderer3D::render_shadow_pass(Vec3 light_direction,
                                            ShadowGeometryCallback submit_geometry) {
    if (!shadow_map_ || !in_frame_) return;

    // Update cascade splits based on current camera
    shadow_map_->update(current_camera_, light_direction);

    // Render each cascade
    for (u32 c = 0; c < shadow_map_->num_cascades(); ++c) {
        shadow_map_->begin_pass(c);
        submit_geometry(c);
        shadow_map_->end_pass();
    }

    // Re-bind the main shader after shadow pass
    rhi_->bind_shader(shader_);
    rhi_->set_uniform_mat4(shader_, "u_ViewProjection", view_projection_);
    rhi_->set_uniform_vec3(shader_, "u_CameraPos", camera_position_);
}

// ── Primitive mesh generators ───────────────────────────────────────────────

Mesh create_cube_mesh() {
    Mesh mesh;

    // 24 vertices (4 per face, for correct normals)
    mesh.vertices = {
        // Front face (+Z)
        {{-0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f}},
        {{ 0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 0.0f}},
        {{ 0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 1.0f}},
        {{-0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 1.0f}},
        // Back face (-Z)
        {{ 0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 0.0f}},
        {{-0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 1.0f}},
        {{ 0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 1.0f}},
        // Top face (+Y)
        {{-0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f}},
        {{ 0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 0.0f}},
        {{ 0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 1.0f}},
        {{-0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 1.0f}},
        // Bottom face (-Y)
        {{-0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f}},
        {{ 0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 0.0f}},
        {{ 0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 1.0f}},
        {{-0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 1.0f}},
        // Right face (+X)
        {{ 0.5f, -0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f}},
        {{ 0.5f, -0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 0.0f}},
        {{ 0.5f,  0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 1.0f}},
        {{ 0.5f,  0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 1.0f}},
        // Left face (-X)
        {{-0.5f, -0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 0.0f}},
        {{-0.5f,  0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 1.0f}},
        {{-0.5f,  0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 1.0f}},
    };

    mesh.indices = {
         0,  1,  2,   2,  3,  0,  // front
         4,  5,  6,   6,  7,  4,  // back
         8,  9, 10,  10, 11,  8,  // top
        12, 13, 14,  14, 15, 12,  // bottom
        16, 17, 18,  18, 19, 16,  // right
        20, 21, 22,  22, 23, 20,  // left
    };

    return mesh;
}

Mesh create_plane_mesh(float size, u32 subdivisions) {
    Mesh mesh;

    float half = size * 0.5f;
    float step = size / static_cast<float>(subdivisions);

    for (u32 z = 0; z <= subdivisions; ++z) {
        for (u32 x = 0; x <= subdivisions; ++x) {
            float px = -half + static_cast<float>(x) * step;
            float pz = -half + static_cast<float>(z) * step;
            float u = static_cast<float>(x) / static_cast<float>(subdivisions);
            float v = static_cast<float>(z) / static_cast<float>(subdivisions);

            mesh.vertices.push_back({
                {px, 0.0f, pz},
                {0.0f, 1.0f, 0.0f},
                {u, v}
            });
        }
    }

    for (u32 z = 0; z < subdivisions; ++z) {
        for (u32 x = 0; x < subdivisions; ++x) {
            u32 tl = z * (subdivisions + 1) + x;
            u32 tr = tl + 1;
            u32 bl = (z + 1) * (subdivisions + 1) + x;
            u32 br = bl + 1;

            mesh.indices.push_back(tl);
            mesh.indices.push_back(bl);
            mesh.indices.push_back(tr);
            mesh.indices.push_back(tr);
            mesh.indices.push_back(bl);
            mesh.indices.push_back(br);
        }
    }

    return mesh;
}

Mesh create_sphere_mesh(float radius, u32 rings, u32 sectors) {
    Mesh mesh;

    for (u32 r = 0; r <= rings; ++r) {
        float phi = math::PI * static_cast<float>(r) / static_cast<float>(rings);
        for (u32 s = 0; s <= sectors; ++s) {
            float theta = math::TWO_PI * static_cast<float>(s) / static_cast<float>(sectors);

            Vec3 pos;
            pos.x = radius * std::sin(phi) * std::cos(theta);
            pos.y = radius * std::cos(phi);
            pos.z = radius * std::sin(phi) * std::sin(theta);

            Vec3 normal = glm::normalize(pos);
            Vec2 uv;
            uv.x = static_cast<float>(s) / static_cast<float>(sectors);
            uv.y = static_cast<float>(r) / static_cast<float>(rings);

            mesh.vertices.push_back({pos, normal, uv});
        }
    }

    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < sectors; ++s) {
            u32 cur = r * (sectors + 1) + s;
            u32 next = cur + sectors + 1;

            mesh.indices.push_back(cur);
            mesh.indices.push_back(next);
            mesh.indices.push_back(cur + 1);

            mesh.indices.push_back(cur + 1);
            mesh.indices.push_back(next);
            mesh.indices.push_back(next + 1);
        }
    }

    return mesh;
}

} // namespace nexus
