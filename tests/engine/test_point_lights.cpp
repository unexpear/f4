#include "doctest/doctest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "backends/null/null_device.h"
#include "core/ecs/transform.h"
#include "core/math/mat4.h"
#include "core/math/vec3.h"
#include "core/math/vec4.h"
#include "core/resources/mesh.h"
#include "core/resources/texture.h"
#include "engine/cameras/camera.h"
#include "engine/lighting/clustered_lights.h"
#include "engine/lighting/light.h"
#include "engine/rendering/material.h"
#include "engine/rendering/mesh_renderer.h"
#include "engine/rendering/renderer.h"
#include "engine/rendering/shader_parity.h"
#include "graphics/framebuffer/framebuffer_desc.h"

#if defined(F4_GRAPHICS_HAS_OPENGL) && defined(F4_GRAPHICS_HAS_VULKAN) && \
    defined(F4_GRAPHICS_HAS_DX12)
#define F4_POINT_LIGHT_GPU 1
#include <epoxy/gl.h>
#include "backends/dx12/dx12_device.h"
#include "backends/opengl/opengl_device.h"
#include "backends/vulkan/vulkan_device.h"
#include "platform/gl_context.h"
#include "platform/window.h"
#include <stb_image_write.h>
#endif

using namespace f4;

namespace {

[[nodiscard]] auto wordFloat(const containers::Array<std::uint32_t>& words, std::size_t index)
    -> float {
    float value = 0.0f;
    std::memcpy(&value, &words[index], sizeof(value));
    return value;
}

// CPU mirror of the shader cluster lookup documented in clustered_lights.h.
[[nodiscard]] auto clusterOfPoint(const containers::Array<std::uint32_t>& words,
                                  const math::Mat4& viewProj, const math::Vec3& point)
    -> std::int64_t {
    const math::Vec4 clip = viewProj.mul(math::Vec4(point, 1.0f));
    if (clip.w <= 0.0f) return -1;
    const std::uint32_t tilesX = words[1];
    const std::uint32_t tilesY = words[2];
    const std::uint32_t slices = words[3];
    const float ndcX = clip.x / clip.w;
    const float ndcY = clip.y / clip.w;
    const auto tile = [](float ndc, std::uint32_t count) {
        const float value = std::floor((ndc * 0.5f + 0.5f) * static_cast<float>(count));
        return static_cast<std::uint32_t>(
            std::clamp(value, 0.0f, static_cast<float>(count - 1u)));
    };
    const float sliceValue = std::floor(std::log2(std::max(clip.w, 1.0e-4f)) *
                                            wordFloat(words, 8) +
                                        wordFloat(words, 9));
    const auto slice = static_cast<std::uint32_t>(
        std::clamp(sliceValue, 0.0f, static_cast<float>(slices - 1u)));
    return static_cast<std::int64_t>((slice * tilesY + tile(ndcY, tilesY)) * tilesX +
                                     tile(ndcX, tilesX));
}

[[nodiscard]] auto clusterContains(const containers::Array<std::uint32_t>& words,
                                   std::uint32_t cluster, std::uint32_t light) -> bool {
    const std::uint32_t record = words[5] + cluster * engine::kPointLightClusterWords;
    const std::uint32_t first = words[record];
    const std::uint32_t count = words[record + 1];
    for (std::uint32_t i = 0; i < count; ++i) {
        if (words[first + i] == light) return true;
    }
    return false;
}

// Small deterministic generator so the binning fixture is identical everywhere.
struct Lcg {
    std::uint32_t state = 0x1234567u;
    auto next() -> float {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    }
    auto range(float lo, float hi) -> float { return lo + (hi - lo) * next(); }
};

[[nodiscard]] auto makeMesh(const std::vector<float>& vertices,
                            const std::vector<std::uint16_t>& indices) -> resources::Mesh {
    // vertices: px py pz nx ny nz u v (8 floats); colors are filled white.
    struct Vertex {
        float px, py, pz, nx, ny, nz, u, v;
        std::uint8_t r, g, b, a;
    };
    std::vector<Vertex> packed;
    for (std::size_t i = 0; i + 7 < vertices.size(); i += 8) {
        packed.push_back({vertices[i], vertices[i + 1], vertices[i + 2], vertices[i + 3],
                          vertices[i + 4], vertices[i + 5], vertices[i + 6], vertices[i + 7],
                          255, 255, 255, 255});
    }
    resources::Mesh mesh;
    resources::VertexStream stream;
    stream.strideBytes = sizeof(Vertex);
    stream.data.resize(packed.size() * sizeof(Vertex));
    std::memcpy(stream.data.data(), packed.data(), packed.size() * sizeof(Vertex));
    mesh.streams.push(static_cast<resources::VertexStream&&>(stream));
    mesh.attributes.push({resources::VertexSemantic::Position,
                          resources::VertexFormat::F32_3, 0, 0});
    mesh.attributes.push({resources::VertexSemantic::Normal,
                          resources::VertexFormat::F32_3, 0, 12});
    mesh.attributes.push({resources::VertexSemantic::TexCoord0,
                          resources::VertexFormat::F32_2, 0, 24});
    mesh.attributes.push({resources::VertexSemantic::Color0,
                          resources::VertexFormat::UNORM8_4, 0, 32});
    mesh.indices.resize(indices.size() * sizeof(std::uint16_t));
    std::memcpy(mesh.indices.data(), indices.data(), indices.size() * sizeof(std::uint16_t));
    mesh.submeshes.push({0, static_cast<std::uint32_t>(indices.size())});
    return mesh;
}

// Ground plane in XZ facing +Y, `half` metres from the origin on each side.
[[nodiscard]] auto makeWhiteTexture() -> resources::Texture {
    resources::Texture texture;
    resources::TextureMip mip;
    mip.width = 1;
    mip.height = 1;
    mip.bytes.resize(4);
    for (std::size_t i = 0; i < 4; ++i) mip.bytes[i] = std::byte{255};
    texture.mips.push(static_cast<resources::TextureMip&&>(mip));
    return texture;
}

[[nodiscard]] auto makeGround(float half) -> resources::Mesh {
    return makeMesh({-half, 0.0f, -half, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
                     -half, 0.0f, half, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                     half, 0.0f, half, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f,
                     half, 0.0f, -half, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f},
                    {0, 1, 2, 0, 2, 3});
}

// Unit cube centred on the origin (24 vertices, flat normals).
[[nodiscard]] auto makeCube() -> resources::Mesh {
    std::vector<float> vertices;
    std::vector<std::uint16_t> indices;
    const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int face = 0; face < 6; ++face) {
        const math::Vec3 normal(n[face][0], n[face][1], n[face][2]);
        const math::Vec3 tangent = std::abs(normal.y) > 0.5f ? math::Vec3(1, 0, 0)
                                                             : math::Vec3(0, 1, 0);
        const math::Vec3 bitangent = math::cross(normal, tangent);
        const auto base = static_cast<std::uint16_t>(vertices.size() / 8);
        const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& corner : corners) {
            const math::Vec3 p = normal * 0.5f + tangent * (0.5f * corner[0]) +
                                 bitangent * (0.5f * corner[1]);
            vertices.insert(vertices.end(), {p.x, p.y, p.z, normal.x, normal.y, normal.z,
                                             corner[0] * 0.5f + 0.5f, corner[1] * 0.5f + 0.5f});
        }
        indices.insert(indices.end(), {base, static_cast<std::uint16_t>(base + 1),
                                       static_cast<std::uint16_t>(base + 2), base,
                                       static_cast<std::uint16_t>(base + 2),
                                       static_cast<std::uint16_t>(base + 3)});
    }
    return makeMesh(vertices, indices);
}

constexpr std::array<std::array<float, 3>, 8> kNightPalette = {{
    {1.0f, 0.55f, 0.18f}, // sodium orange
    {0.25f, 0.55f, 1.0f}, // cold blue
    {1.0f, 0.18f, 0.32f}, // neon red
    {0.25f, 1.0f, 0.45f}, // green
    {0.95f, 0.3f, 1.0f},  // magenta
    {1.0f, 0.9f, 0.55f},  // warm white
    {0.2f, 0.95f, 1.0f},  // cyan
    {1.0f, 0.8f, 0.1f},   // amber
}};

struct NightSceneOptions {
    std::uint32_t lightsPerSide = 16;
    float extent = 10.0f;     // lights cover [-extent, extent] in X and Z
    float lightHeight = 0.6f; // metres above the ground
    float range = 2.2f;
    float intensity = 6.0f;
    bool pointLightsEnabled = true;
    bool lampCubes = true;
};

// Builds the night scene into `registry` and returns the camera entity.
auto buildNightScene(engine::Renderer& renderer, ecs::Registry& registry,
                     const NightSceneOptions& options) -> ecs::Entity {
    renderer.pbrSettings().shadowsEnabled = false;
    renderer.pbrSettings().ambientIntensity = 0.02f;
    renderer.pbrSettings().pointLightsEnabled = options.pointLightsEnabled;

    engine::StandardMaterialDesc groundDesc;
    groundDesc.tint = math::Vec4(0.16f, 0.16f, 0.18f, 1.0f);
    groundDesc.roughnessFactor = 0.8f;
    groundDesc.metallicFactor = 0.0f;
    groundDesc.doubleSided = true;
    const auto groundMaterial = renderer.createMaterial(groundDesc);
    const auto groundMesh = renderer.createMesh(makeGround(options.extent * 1.4f));
    REQUIRE(groundMaterial.isValid());
    REQUIRE(groundMesh.isValid());
    const ecs::Entity ground = registry.create();
    registry.emplace<ecs::LocalTransform>(ground, ecs::LocalTransform{});
    registry.emplace<engine::MeshRenderer>(
        ground, engine::MeshRenderer{groundMesh, groundMaterial, 0, false, false});

    std::array<resources::MaterialHandle, kNightPalette.size()> lampMaterials{};
    const auto cubeMesh = renderer.createMesh(makeCube());
    REQUIRE(cubeMesh.isValid());
    // Emissive factors only scale the emissive texture (black by default).
    const auto lampEmission = renderer.createTexture(makeWhiteTexture());
    REQUIRE(lampEmission.isValid());
    for (std::size_t i = 0; i < kNightPalette.size(); ++i) {
        engine::StandardMaterialDesc lampDesc;
        lampDesc.emissive = lampEmission;
        lampDesc.tint = math::Vec4(0.02f, 0.02f, 0.02f, 1.0f);
        lampDesc.emissiveFactor = math::Vec3(kNightPalette[i][0], kNightPalette[i][1],
                                             kNightPalette[i][2]) * 2.5f;
        lampMaterials[i] = renderer.createMaterial(lampDesc);
        REQUIRE(lampMaterials[i].isValid());
    }

    const std::uint32_t side = std::max(options.lightsPerSide, 1u);
    for (std::uint32_t row = 0; row < side; ++row) {
        for (std::uint32_t column = 0; column < side; ++column) {
            const float fx = side == 1 ? 0.5f : static_cast<float>(column) / static_cast<float>(side - 1);
            const float fz = side == 1 ? 0.5f : static_cast<float>(row) / static_cast<float>(side - 1);
            const math::Vec3 position((fx * 2.0f - 1.0f) * options.extent, options.lightHeight,
                                      (fz * 2.0f - 1.0f) * options.extent);
            const std::size_t paletteIndex = (row * 3u + column * 5u) % kNightPalette.size();
            const auto& color = kNightPalette[paletteIndex];
            const ecs::Entity light = registry.create();
            engine::PointLight point;
            point.position = position;
            point.color = math::Vec3(color[0], color[1], color[2]);
            point.intensity = options.intensity;
            point.range = options.range;
            registry.emplace<engine::PointLight>(light, point);
            if (options.lampCubes) {
                const ecs::Entity lamp = registry.create();
                auto& transform = registry.emplace<ecs::LocalTransform>(lamp, ecs::LocalTransform{});
                transform.position = position;
                transform.scale = math::Vec3(0.12f);
                registry.emplace<engine::MeshRenderer>(
                    lamp, engine::MeshRenderer{cubeMesh, lampMaterials[paletteIndex], 0, false,
                                               false});
            }
        }
    }

    const ecs::Entity cameraEntity = registry.create();
    engine::Camera camera;
    camera.eye = {0.0f, 7.0f, 11.0f};
    camera.center = {0.0f, 0.0f, 0.0f};
    camera.nearPlane = 0.1f;
    camera.farPlane = 60.0f;
    registry.emplace<engine::Camera>(cameraEntity, camera);
    ecs::updateWorldTransforms(registry);
    return cameraEntity;
}

} // namespace

TEST_CASE("Point lights: an empty frame still uploads a valid cluster header") {
    containers::Array<std::uint32_t> words;
    const math::Mat4 view = math::Mat4::lookAtRH({0, 2, 5}, {0, 0, 0}, {0, 1, 0});
    const math::Mat4 projection = math::Mat4::perspectiveRH_ZO(1.0f, 1.5f, 0.1f, 100.0f);
    const auto stats = engine::buildClusteredLightBuffer({}, view, projection, 0.1f, 100.0f,
                                                         engine::ClusterGridConfig{}, words);
    REQUIRE(words.size() == engine::kPointLightHeaderWords);
    CHECK(words[0] == 0);
    CHECK(words[1] == 16);
    CHECK(words[2] == 9);
    CHECK(words[3] == 24);
    CHECK(words[4] == engine::kPointLightHeaderWords);
    CHECK(stats.submitted == 0);
    CHECK(stats.visible == 0);
    CHECK(stats.indexCount == 0);
    const auto params = engine::clusterSliceParams(0.1f, 100.0f, 24);
    CHECK(engine::clusterSliceForDepth(0.1f, params, 24) == 0);
    CHECK(engine::clusterSliceForDepth(99.0f, params, 24) == 23);
    CHECK(engine::clusterSliceForDepth(1000.0f, params, 24) == 23);
    CHECK(engine::clusterSliceForDepth(0.0f, params, 24) == 0);
}

TEST_CASE("Point lights: lights outside the camera volume or without energy are culled") {
    const math::Vec3 eye(0.0f, 2.0f, 10.0f);
    const math::Mat4 view = math::Mat4::lookAtRH(eye, {0, 0, 0}, {0, 1, 0});
    const math::Mat4 projection = math::Mat4::perspectiveRH_ZO(1.0f, 1.0f, 0.1f, 100.0f);
    std::vector<engine::ClusteredLightInput> lights = {
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 0.25f}, 1.0f},    // visible
        {{0.0f, 2.0f, 20.0f}, {1.0f, 1.0f, 1.0f}, 1.0f},    // behind the camera
        {{-200.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 1.0f},  // far outside the frustum
        {{0.0f, 0.0f, -200.0f}, {1.0f, 1.0f, 1.0f}, 1.0f},  // beyond the far plane
        {{1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 0.0f},     // zero range
        {{-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 2.0f},    // no radiance
        {{0.0f, 2.0f, 10.05f}, {1.0f, 1.0f, 1.0f}, 0.5f},   // crosses the near plane
    };
    containers::Array<std::uint32_t> words;
    const auto stats = engine::buildClusteredLightBuffer(lights, view, projection, 0.1f, 100.0f,
                                                         engine::ClusterGridConfig{}, words);
    CHECK(stats.submitted == 7);
    REQUIRE(stats.visible == 2);
    REQUIRE(words[0] == 2);
    CHECK(wordFloat(words, words[4] + 0) == 0.0f);
    CHECK(wordFloat(words, words[4] + 3) == 1.0f);
    CHECK(wordFloat(words, words[4] + 4) == 1.0f);
    CHECK(wordFloat(words, words[4] + 5) == 0.5f);
    // The near-crossing light covers every tile of its first slice.
    const std::uint32_t tiles = words[1] * words[2];
    std::uint32_t nearCoverage = 0;
    for (std::uint32_t cluster = 0; cluster < tiles; ++cluster) {
        if (clusterContains(words, cluster, 1)) ++nearCoverage;
    }
    CHECK(nearCoverage == tiles);
    // The centre light is listed in the cluster that contains its centre.
    const math::Mat4 viewProj = projection.mul(view);
    const auto centreCluster = clusterOfPoint(words, viewProj, {0.0f, 0.0f, 0.0f});
    REQUIRE(centreCluster >= 0);
    CHECK(clusterContains(words, static_cast<std::uint32_t>(centreCluster), 0));
}

TEST_CASE("Point lights: clusters list every light that reaches any sampled point") {
    const math::Mat4 view = math::Mat4::lookAtRH({0.0f, 6.0f, 18.0f}, {0, 0, 0}, {0, 1, 0});
    const math::Mat4 projection =
        math::Mat4::perspectiveRH_ZO(1.04719758f, 16.0f / 9.0f, 0.1f, 80.0f);
    const math::Mat4 viewProj = projection.mul(view);
    Lcg random;
    std::vector<engine::ClusteredLightInput> lights;
    for (int i = 0; i < 600; ++i) {
        lights.push_back({{random.range(-24.0f, 24.0f), random.range(0.0f, 4.0f),
                           random.range(-30.0f, 16.0f)},
                          {random.range(0.1f, 1.0f), random.range(0.1f, 1.0f),
                           random.range(0.1f, 1.0f)},
                          random.range(0.4f, 4.0f)});
    }
    containers::Array<std::uint32_t> words;
    const auto stats = engine::buildClusteredLightBuffer(lights, view, projection, 0.1f, 80.0f,
                                                         engine::ClusterGridConfig{}, words);
    CHECK(stats.submitted == 600);
    CHECK(stats.visible > 200);
    CHECK(stats.visible < 600);
    CHECK(stats.droppedIndices == 0);
    REQUIRE(words[0] == stats.visible);

    // Map source lights to their packed index through their exact positions.
    std::vector<std::int64_t> packedIndex(lights.size(), -1);
    for (std::uint32_t packed = 0; packed < words[0]; ++packed) {
        const std::uint32_t record = words[4] + packed * engine::kPointLightRecordWords;
        for (std::size_t source = 0; source < lights.size(); ++source) {
            if (wordFloat(words, record) == lights[source].position.x &&
                wordFloat(words, record + 1) == lights[source].position.y &&
                wordFloat(words, record + 2) == lights[source].position.z) {
                packedIndex[source] = packed;
            }
        }
    }

    std::size_t checkedPairs = 0;
    std::size_t missing = 0;
    for (int sample = 0; sample < 40000; ++sample) {
        const math::Vec3 point(random.range(-26.0f, 26.0f), random.range(-0.5f, 5.0f),
                               random.range(-32.0f, 18.0f));
        const math::Vec4 clip = viewProj.mul(math::Vec4(point, 1.0f));
        if (clip.w <= 0.1f || clip.w >= 80.0f || std::abs(clip.x) > clip.w ||
            std::abs(clip.y) > clip.w) {
            continue;
        }
        const auto cluster = clusterOfPoint(words, viewProj, point);
        REQUIRE(cluster >= 0);
        for (std::size_t source = 0; source < lights.size(); ++source) {
            const float rangeSq = lights[source].range * lights[source].range;
            if (math::lengthSq(point - lights[source].position) >= rangeSq) continue;
            ++checkedPairs;
            if (packedIndex[source] < 0 ||
                !clusterContains(words, static_cast<std::uint32_t>(cluster),
                                 static_cast<std::uint32_t>(packedIndex[source]))) {
                ++missing;
            }
        }
    }
    INFO("checked light/point pairs=" << checkedPairs);
    CHECK(checkedPairs > 1000);
    CHECK(missing == 0);
}

TEST_CASE("Point lights: the light-reference cap drops and counts overflow") {
    const math::Mat4 view = math::Mat4::lookAtRH({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    const math::Mat4 projection = math::Mat4::perspectiveRH_ZO(1.0f, 1.0f, 0.1f, 50.0f);
    std::vector<engine::ClusteredLightInput> lights;
    for (int i = 0; i < 50; ++i) {
        lights.push_back({{static_cast<float>(i % 5) - 2.0f, 0.0f, 0.0f}, {1, 1, 1}, 3.0f});
    }
    engine::ClusterGridConfig config;
    config.maxIndexCount = 10;
    containers::Array<std::uint32_t> words;
    const auto stats =
        engine::buildClusteredLightBuffer(lights, view, projection, 0.1f, 50.0f, config, words);
    CHECK(stats.visible == 50);
    CHECK(stats.indexCount == 10);
    CHECK(stats.droppedIndices > 0);
    std::uint32_t listed = 0;
    const std::uint32_t clusters = words[1] * words[2] * words[3];
    for (std::uint32_t cluster = 0; cluster < clusters; ++cluster) {
        listed += words[words[5] + cluster * engine::kPointLightClusterWords + 1];
    }
    CHECK(listed == 10);
    CHECK(words.size() == words[6] + 10);
}

TEST_CASE("Point lights: the renderer uploads and binds clustered lights for every draw") {
    gfx::NullDevice device;
    ecs::Registry registry(4096);
    engine::Renderer renderer(registry, device);
    REQUIRE(renderer.init() == gfx::GfxError::None);
    NightSceneOptions options;
    options.lightsPerSide = 18; // 324 lights
    options.lampCubes = false;
    const ecs::Entity cameraEntity = buildNightScene(renderer, registry, options);

    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {64, 64};
    framebufferDesc.attachments.push(gfx::AttachmentDesc{});
    auto framebuffer = device.createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());
    REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f) == gfx::GfxError::None);
    const engine::FrameStats stats = renderer.stats();
    CHECK(stats.draws == 1);
    CHECK(stats.pointLights == 324);
    CHECK(stats.pointLightsVisible > 100);
    CHECK(stats.pointLightsVisible <= 324);
    CHECK(stats.pointLightReferences > stats.pointLightsVisible);
    CHECK(stats.pointLightReferencesDropped == 0);
    const auto words = renderer.pointLightWords();
    REQUIRE(words.size() > engine::kPointLightHeaderWords);
    CHECK(words[0] == stats.pointLightsVisible);
    CHECK(words[7] == stats.pointLightReferences);

    renderer.pbrSettings().pointLightsEnabled = false;
    REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f) == gfx::GfxError::None);
    CHECK(renderer.stats().pointLights == 0);
    CHECK(renderer.pointLightWords().size() == engine::kPointLightHeaderWords);
    CHECK(renderer.pointLightWords()[0] == 0);
    CHECK(device.destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
}

#if defined(F4_POINT_LIGHT_GPU)
namespace {

using Capture = std::vector<std::uint8_t>;

template <typename Reader>
[[nodiscard]] auto captureNightFrame(gfx::IDevice& device, const NightSceneOptions& options,
                                     std::uint32_t width, std::uint32_t height,
                                     Reader&& reader, engine::FrameStats* statsOut = nullptr)
    -> Capture {
    gfx::TextureDesc colorDesc;
    colorDesc.width = width;
    colorDesc.height = height;
    colorDesc.hasOptimizedClear = true;
    colorDesc.optimizedClear.color[0] = 0.0f;
    colorDesc.optimizedClear.color[1] = 0.0f;
    colorDesc.optimizedClear.color[2] = 0.0f;
    colorDesc.optimizedClear.color[3] = 1.0f;
    auto color = device.createTexture(colorDesc);
    REQUIRE(color.hasValue());
    gfx::TextureDesc depthDesc;
    depthDesc.width = width;
    depthDesc.height = height;
    depthDesc.format = gfx::TextureFormat::Depth32F;
    depthDesc.hasOptimizedClear = true;
    depthDesc.optimizedClear.depth = 1.0f;
    auto depth = device.createTexture(depthDesc);
    REQUIRE(depth.hasValue());
    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {width, height};
    gfx::AttachmentDesc colorAttachment;
    colorAttachment.texture = color.value();
    colorAttachment.clearOnLoad = true;
    colorAttachment.clear.color[0] = 0.0f;
    colorAttachment.clear.color[1] = 0.0f;
    colorAttachment.clear.color[2] = 0.0f;
    colorAttachment.clear.color[3] = 1.0f;
    framebufferDesc.attachments.push(colorAttachment);
    gfx::AttachmentDesc depthAttachment;
    depthAttachment.type = gfx::AttachmentType::Depth;
    depthAttachment.texture = depth.value();
    depthAttachment.clearOnLoad = true;
    framebufferDesc.attachments.push(depthAttachment);
    auto framebuffer = device.createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());

    Capture pixels(static_cast<std::size_t>(width) * height * 4u, 0);
    {
        ecs::Registry registry(4096);
        engine::Renderer renderer(registry, device);
        REQUIRE(renderer.init() == gfx::GfxError::None);
        const ecs::Entity cameraEntity = buildNightScene(renderer, registry, options);
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        REQUIRE(renderer.render(framebuffer.value(), cameraEntity, aspect) ==
                gfx::GfxError::None);
        if (statsOut != nullptr) *statsOut = renderer.stats();
        device.waitIdle();
        REQUIRE(reader(color.value(), std::as_writable_bytes(std::span(pixels))) ==
                gfx::GfxError::None);
        if (device.backend() == gfx::Backend::OpenGL) {
            const std::size_t row = static_cast<std::size_t>(width) * 4u;
            for (std::uint32_t y = 0; y < height / 2; ++y) {
                std::swap_ranges(pixels.begin() + static_cast<std::ptrdiff_t>(y * row),
                                 pixels.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                                 pixels.begin() +
                                     static_cast<std::ptrdiff_t>((height - 1 - y) * row));
            }
        }
    }
    CHECK(device.destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
    CHECK(device.destroyTexture(depth.value()) == gfx::GfxError::None);
    CHECK(device.destroyTexture(color.value()) == gfx::GfxError::None);
    return pixels;
}

[[nodiscard]] auto meanLuma(const Capture& pixels) -> double {
    double sum = 0.0;
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        sum += 0.2126 * pixels[i] + 0.7152 * pixels[i + 1] + 0.0722 * pixels[i + 2];
    }
    return sum / static_cast<double>(pixels.size() / 4);
}

[[nodiscard]] auto dominantChannelCounts(const Capture& pixels) -> std::array<std::size_t, 3> {
    std::array<std::size_t, 3> counts{};
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const int r = pixels[i];
        const int g = pixels[i + 1];
        const int b = pixels[i + 2];
        if (r > 40 && r > g + 20 && r > b + 20) ++counts[0];
        if (g > 40 && g > r + 20 && g > b + 20) ++counts[1];
        if (b > 40 && b > r + 20 && b > g + 20) ++counts[2];
    }
    return counts;
}

void maybeWriteCapture(const char* name, const Capture& pixels, std::uint32_t width,
                       std::uint32_t height) {
    const char* directory = std::getenv("F4_POINT_LIGHT_CAPTURE_DIR");
    if (directory == nullptr || directory[0] == '\0') return;
    const std::string path = std::string(directory) + "/" + name + ".png";
    CHECK(stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                         pixels.data(), static_cast<int>(width * 4u)) != 0);
}

struct BackendCaptures {
    Capture lit;
    Capture dark;
    Capture outOfReach;
    engine::FrameStats litStats{};
};

template <typename Reader>
[[nodiscard]] auto captureBackend(gfx::IDevice& device, Reader&& reader) -> BackendCaptures {
    constexpr std::uint32_t width = 160;
    constexpr std::uint32_t height = 96;
    BackendCaptures result;
    NightSceneOptions lit;
    result.lit = captureNightFrame(device, lit, width, height, reader, &result.litStats);
    NightSceneOptions dark = lit;
    dark.pointLightsEnabled = false;
    result.dark = captureNightFrame(device, dark, width, height, reader);
    // Lights hover 3 m up with 1 m reach: they are clustered and iterated but
    // reach no surface, so the frame must equal the lights-disabled frame.
    NightSceneOptions outOfReach = lit;
    outOfReach.lightHeight = 3.0f;
    outOfReach.range = 1.0f;
    outOfReach.lampCubes = false;
    NightSceneOptions darkNoLamps = dark;
    darkNoLamps.lampCubes = false;
    result.outOfReach = captureNightFrame(device, outOfReach, width, height, reader);
    const Capture darkReference = captureNightFrame(device, darkNoLamps, width, height, reader);
    CHECK(result.outOfReach == darkReference);
    return result;
}

} // namespace

TEST_CASE("Point lights: clustered night scene matches across OpenGL, Vulkan, and DX12") {
    constexpr std::uint32_t width = 160;
    constexpr std::uint32_t height = 96;
    auto windowResult = platform::Window::create({"Point lights", width, height, false});
    REQUIRE(windowResult.hasValue());
    platform::Window* window = windowResult.value();
    REQUIRE(window->makeCurrent());
    REQUIRE(platform::initGl(4, 3).hasValue());
    gfx::IDevice* openGlBase = gfx::createOpenGLDevice();
    REQUIRE(openGlBase != nullptr);
    const auto gl = captureBackend(*openGlBase, [](gfx::TextureHandle, std::span<std::byte> out) {
        glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA,
                     GL_UNSIGNED_BYTE, out.data());
        return glGetError() == GL_NO_ERROR ? gfx::GfxError::None : gfx::GfxError::DeviceLost;
    });
    gfx::deleteDevice(openGlBase);
    delete window;

    gfx::IDevice* vulkanBase = gfx::createVulkanDevice();
    REQUIRE(vulkanBase != nullptr);
    auto* vulkan = static_cast<gfx::VulkanDevice*>(vulkanBase);
    const auto vk = captureBackend(*vulkan, [vulkan](gfx::TextureHandle texture,
                                                     std::span<std::byte> out) {
        return vulkan->readTexture(texture, 0, out);
    });
    CHECK(vulkan->validationWarningCount() == 0);
    CHECK(vulkan->validationErrorCount() == 0);
    gfx::deleteDevice(vulkanBase);

    gfx::IDevice* dx12Base = gfx::createDx12Device();
    REQUIRE(dx12Base != nullptr);
    auto* dx12 = static_cast<gfx::Dx12Device*>(dx12Base);
    const auto dx = captureBackend(*dx12, [dx12](gfx::TextureHandle texture,
                                                 std::span<std::byte> out) {
        return dx12->readTexture(texture, 0, out);
    });
    CHECK(dx12->validationWarningCount() == 0);
    CHECK(dx12->validationErrorCount() == 0);
    gfx::deleteDevice(dx12Base);

    maybeWriteCapture("night_lights_opengl", gl.lit, width, height);
    maybeWriteCapture("night_lights_vulkan", vk.lit, width, height);
    maybeWriteCapture("night_lights_dx12", dx.lit, width, height);
    maybeWriteCapture("night_lights_off_opengl", gl.dark, width, height);

    CHECK(gl.litStats.pointLights == 256);
    CHECK(gl.litStats.pointLightsVisible > 150);
    const double litLuma = meanLuma(gl.lit);
    const double darkLuma = meanLuma(gl.dark);
    const auto hues = dominantChannelCounts(gl.lit);
    MESSAGE("night scene: visible lights=" << gl.litStats.pointLightsVisible
            << " refs=" << gl.litStats.pointLightReferences
            << " max/cluster=" << gl.litStats.maxPointLightsPerCluster
            << " luma lit=" << litLuma << " dark=" << darkLuma
            << " red/green/blue-dominant pixels=" << hues[0] << "/" << hues[1] << "/"
            << hues[2]);
    CHECK(darkLuma < 12.0);
    CHECK(litLuma > darkLuma * 3.0);
    CHECK(hues[0] > 50);
    CHECK(hues[1] > 50);
    CHECK(hues[2] > 50);

    const auto glVk = engine::compareShaderImagesRgba8(gl.lit, vk.lit, 2);
    const auto glDx = engine::compareShaderImagesRgba8(gl.lit, dx.lit, 2);
    const auto glVkDark = engine::compareShaderImagesRgba8(gl.dark, vk.dark, 2);
    const auto glDxDark = engine::compareShaderImagesRgba8(gl.dark, dx.dark, 2);
    MESSAGE("lit GL/VK mismatches=" << glVk.mismatchPixelCount << " max delta="
            << static_cast<int>(glVk.maxChannelDelta) << "; GL/DX mismatches="
            << glDx.mismatchPixelCount << " max delta="
            << static_cast<int>(glDx.maxChannelDelta));
    CHECK(glVk.matches());
    CHECK(glDx.matches());
    CHECK(glVkDark.matches());
    CHECK(glDxDark.matches());
}

TEST_CASE("Point lights: night scene frame-time benchmark (set F4_POINT_LIGHT_BENCH=1)") {
    const char* enabled = std::getenv("F4_POINT_LIGHT_BENCH");
    if (enabled == nullptr || enabled[0] != '1') return;
    constexpr std::uint32_t width = 1280;
    constexpr std::uint32_t height = 720;
    constexpr int warmup = 30;
    constexpr int frames = 240;

    // "stress": fixed 2.5 m range, so denser grids pile hundreds of lights into
    // each cluster. "scaled": range shrinks with the grid spacing (2x spacing),
    // the way a level designer would place many small lights.
    const auto bench = [&](gfx::IDevice& device, const char* backend,
                           std::uint32_t lightsPerSide, bool scaledRange) {
        gfx::TextureDesc colorDesc;
        colorDesc.width = width;
        colorDesc.height = height;
        auto color = device.createTexture(colorDesc);
        REQUIRE(color.hasValue());
        gfx::TextureDesc depthDesc;
        depthDesc.width = width;
        depthDesc.height = height;
        depthDesc.format = gfx::TextureFormat::Depth32F;
        depthDesc.hasOptimizedClear = true;
        depthDesc.optimizedClear.depth = 1.0f;
        auto depth = device.createTexture(depthDesc);
        REQUIRE(depth.hasValue());
        gfx::FramebufferDesc framebufferDesc;
        framebufferDesc.extent = {width, height};
        gfx::AttachmentDesc colorAttachment;
        colorAttachment.texture = color.value();
        colorAttachment.clearOnLoad = true;
        framebufferDesc.attachments.push(colorAttachment);
        gfx::AttachmentDesc depthAttachment;
        depthAttachment.type = gfx::AttachmentType::Depth;
        depthAttachment.texture = depth.value();
        depthAttachment.clearOnLoad = true;
        framebufferDesc.attachments.push(depthAttachment);
        auto framebuffer = device.createFramebuffer(framebufferDesc);
        REQUIRE(framebuffer.hasValue());
        {
            ecs::Registry registry(16384);
            engine::Renderer renderer(registry, device);
            REQUIRE(renderer.init() == gfx::GfxError::None);
            NightSceneOptions options;
            options.lightsPerSide = lightsPerSide;
            options.extent = 14.0f;
            options.range = 2.5f;
            if (scaledRange && lightsPerSide > 1) {
                const float spacing =
                    2.0f * options.extent / static_cast<float>(lightsPerSide - 1u);
                options.range = std::max(0.75f, 2.0f * spacing);
            }
            options.pointLightsEnabled = lightsPerSide > 0;
            const ecs::Entity cameraEntity = buildNightScene(renderer, registry, options);
            const float aspect = static_cast<float>(width) / static_cast<float>(height);
            for (int i = 0; i < warmup; ++i) {
                REQUIRE(renderer.render(framebuffer.value(), cameraEntity, aspect) ==
                        gfx::GfxError::None);
            }
            device.waitIdle();
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < frames; ++i) {
                REQUIRE(renderer.render(framebuffer.value(), cameraEntity, aspect) ==
                        gfx::GfxError::None);
            }
            device.waitIdle();
            const auto end = std::chrono::steady_clock::now();
            const double ms =
                std::chrono::duration<double, std::milli>(end - start).count() / frames;
            const engine::FrameStats stats = renderer.stats();
            std::printf("[point-light bench] %s %s %ux%u lights=%u range=%.2f visible=%u "
                        "refs=%u max/cluster=%u draws=%u: %.3f ms/frame\n",
                        backend, scaledRange ? "scaled" : "stress", width, height,
                        stats.pointLights, static_cast<double>(options.range),
                        stats.pointLightsVisible,
                        stats.pointLightReferences, stats.maxPointLightsPerCluster,
                        stats.draws, ms);
        }
        CHECK(device.destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
        CHECK(device.destroyTexture(depth.value()) == gfx::GfxError::None);
        CHECK(device.destroyTexture(color.value()) == gfx::GfxError::None);
    };

    {
        auto windowResult = platform::Window::create({"Point light bench", 64, 64, false});
        REQUIRE(windowResult.hasValue());
        platform::Window* window = windowResult.value();
        REQUIRE(window->makeCurrent());
        REQUIRE(platform::initGl(4, 3).hasValue());
        gfx::IDevice* device = gfx::createOpenGLDevice();
        REQUIRE(device != nullptr);
        std::printf("[point-light bench] OpenGL renderer: %s\n",
                    reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
        for (std::uint32_t side : {0u, 16u, 32u, 64u}) bench(*device, "OpenGL", side, false);
        for (std::uint32_t side : {32u, 64u}) bench(*device, "OpenGL", side, true);
        gfx::deleteDevice(device);
        delete window;
    }
    {
        gfx::IDevice* device = gfx::createVulkanDevice();
        REQUIRE(device != nullptr);
        for (std::uint32_t side : {0u, 16u, 32u, 64u}) bench(*device, "Vulkan", side, false);
        for (std::uint32_t side : {32u, 64u}) bench(*device, "Vulkan", side, true);
        gfx::deleteDevice(device);
    }
    {
        gfx::IDevice* device = gfx::createDx12Device();
        REQUIRE(device != nullptr);
        std::printf("[point-light bench] DX12 adapter: %s\n",
                    static_cast<gfx::Dx12Device*>(device)->deviceName());
        for (std::uint32_t side : {0u, 16u, 32u, 64u}) bench(*device, "DX12", side, false);
        for (std::uint32_t side : {32u, 64u}) bench(*device, "DX12", side, true);
        gfx::deleteDevice(device);
    }
}
#endif
