#include "doctest/doctest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
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
#include "engine/lighting/point_shadows.h"
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

[[nodiscard]] auto wordFloat(std::span<const std::uint32_t> words, std::size_t index)
    -> float {
    float value = 0.0f;
    std::memcpy(&value, &words[index], sizeof(value));
    return value;
}

[[nodiscard]] auto wordFloat(const containers::Array<std::uint32_t>& words, std::size_t index)
    -> float {
    return wordFloat(std::span<const std::uint32_t>(words.data(), words.size()), index);
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
    // Point-light shadows: flag every light castShadows and let the renderer
    // shadow `shadowBudget` of them; pillars (pillarsPerSide^2) are the casters.
    bool castShadows = false;
    std::uint32_t shadowBudget = 8;
    std::uint32_t pillarsPerSide = 0;
    // HDR bloom.
    bool bloom = false;
    float bloomIntensity = 0.6f;
    float bloomThreshold = 1.0f;
};

// Builds the night scene into `registry` and returns the camera entity.
auto buildNightScene(engine::Renderer& renderer, ecs::Registry& registry,
                     const NightSceneOptions& options) -> ecs::Entity {
    renderer.pbrSettings().shadowsEnabled = false;
    renderer.pbrSettings().ambientIntensity = 0.02f;
    renderer.pbrSettings().pointLightsEnabled = options.pointLightsEnabled;
    renderer.pbrSettings().pointShadowBudget = options.shadowBudget;
    renderer.pbrSettings().bloomEnabled = options.bloom;
    renderer.pbrSettings().bloomIntensity = options.bloomIntensity;
    renderer.pbrSettings().bloomThreshold = options.bloomThreshold;

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
            point.castShadows = options.castShadows;
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

    if (options.pillarsPerSide > 0) {
        engine::StandardMaterialDesc pillarDesc;
        pillarDesc.tint = math::Vec4(0.55f, 0.52f, 0.48f, 1.0f);
        pillarDesc.roughnessFactor = 0.7f;
        const auto pillarMaterial = renderer.createMaterial(pillarDesc);
        REQUIRE(pillarMaterial.isValid());
        const std::uint32_t pillars = options.pillarsPerSide;
        for (std::uint32_t row = 0; row < pillars; ++row) {
            for (std::uint32_t column = 0; column < pillars; ++column) {
                const float fx = (static_cast<float>(column) + 0.5f) / static_cast<float>(pillars);
                const float fz = (static_cast<float>(row) + 0.5f) / static_cast<float>(pillars);
                const ecs::Entity pillar = registry.create();
                auto& transform =
                    registry.emplace<ecs::LocalTransform>(pillar, ecs::LocalTransform{});
                transform.position = {(fx * 2.0f - 1.0f) * options.extent, 0.8f,
                                      (fz * 2.0f - 1.0f) * options.extent};
                transform.scale = math::Vec3(0.3f, 1.6f, 0.3f);
                registry.emplace<engine::MeshRenderer>(
                    pillar, engine::MeshRenderer{cubeMesh, pillarMaterial, 0, true, false});
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

namespace {

// What light_binning.comp computes, step for step, on the CPU: copy the
// prefix, count per cluster, prefix-sum with the index cap, fill the lists.
[[nodiscard]] auto emulateGpuBinning(const containers::Array<std::uint32_t>& input,
                                     const engine::ClusteredLightGpuParams& p)
    -> std::vector<std::uint32_t> {
    std::vector<std::uint32_t> out(p.totalWords, 0xdeadbeefu);
    for (std::uint32_t word = 0; word < p.copyWords; ++word) out[word] = input[word];
    const auto covers = [&](std::uint32_t light, std::uint32_t x, std::uint32_t y,
                            std::uint32_t z) {
        const std::uint32_t* box = input.data() + p.rangeOffset + light * 3u;
        return x >= (box[0] & 0xffffu) && x <= (box[0] >> 16) && y >= (box[1] & 0xffffu) &&
               y <= (box[1] >> 16) && z >= (box[2] & 0xffffu) && z <= (box[2] >> 16);
    };
    std::vector<std::uint32_t> counts(p.clusterCount, 0);
    for (std::uint32_t cluster = 0; cluster < p.clusterCount; ++cluster) {
        const std::uint32_t x = cluster % p.tilesX;
        const std::uint32_t y = (cluster / p.tilesX) % p.tilesY;
        const std::uint32_t z = cluster / (p.tilesX * p.tilesY);
        for (std::uint32_t light = 0; light < p.visible; ++light) {
            if (covers(light, x, y, z)) ++counts[cluster];
        }
    }
    std::uint32_t prefix = 0;
    for (std::uint32_t cluster = 0; cluster < p.clusterCount; ++cluster) {
        const std::uint32_t start = std::min(prefix, p.maxIndexCount);
        out[p.gridOffset + cluster * 2u] = p.indexOffset + start;
        out[p.gridOffset + cluster * 2u + 1u] = std::min(counts[cluster], p.maxIndexCount - start);
        prefix += counts[cluster];
    }
    for (std::uint32_t cluster = 0; cluster < p.clusterCount; ++cluster) {
        const std::uint32_t x = cluster % p.tilesX;
        const std::uint32_t y = (cluster / p.tilesX) % p.tilesY;
        const std::uint32_t z = cluster / (p.tilesX * p.tilesY);
        const std::uint32_t first = out[p.gridOffset + cluster * 2u];
        const std::uint32_t allowed = out[p.gridOffset + cluster * 2u + 1u];
        std::uint32_t count = 0;
        for (std::uint32_t light = 0; light < p.visible; ++light) {
            if (!covers(light, x, y, z)) continue;
            if (count < allowed) out[first + count] = light;
            ++count;
        }
    }
    return out;
}

// Compares two clustered-light buffers by meaning: same header values, light
// records, shadow matrices, and per-cluster lists, wherever each buffer
// places its sections. Returns the number of differing items.
[[nodiscard]] auto compareLightBuffers(std::span<const std::uint32_t> a,
                                       std::span<const std::uint32_t> b) -> std::size_t {
    std::size_t differences = 0;
    if (a.size() < engine::kPointLightHeaderWords || b.size() < engine::kPointLightHeaderWords) {
        return 1;
    }
    for (std::size_t word : {0, 1, 2, 3, 7, 8, 9, 11, 12, 13, 14}) {
        if (a[word] != b[word]) ++differences;
    }
    if (differences != 0) return differences;
    const std::uint32_t visible = a[0];
    for (std::uint32_t i = 0; i < visible * engine::kPointLightRecordWords; ++i) {
        if (a[a[4] + i] != b[b[4] + i]) ++differences;
    }
    const std::uint32_t matrixWords = a[11] * 6u * 16u;
    for (std::uint32_t i = 0; i < matrixWords; ++i) {
        if (a[a[10] + i] != b[b[10] + i]) ++differences;
    }
    if (visible == 0) return differences;
    const std::uint32_t clusters = a[1] * a[2] * a[3];
    for (std::uint32_t cluster = 0; cluster < clusters; ++cluster) {
        const std::uint32_t* ra = a.data() + a[5] + cluster * engine::kPointLightClusterWords;
        const std::uint32_t* rb = b.data() + b[5] + cluster * engine::kPointLightClusterWords;
        if (ra[1] != rb[1] || ra[0] - a[6] != rb[0] - b[6]) {
            ++differences;
            continue;
        }
        for (std::uint32_t k = 0; k < ra[1]; ++k) {
            if (a[ra[0] + k] != b[rb[0] + k]) ++differences;
        }
    }
    return differences;
}

} // namespace

TEST_CASE("Point lights: the GPU binning algorithm reproduces the CPU cluster buffer") {
    const math::Mat4 view = math::Mat4::lookAtRH({0.0f, 6.0f, 18.0f}, {0, 0, 0}, {0, 1, 0});
    const math::Mat4 projection =
        math::Mat4::perspectiveRH_ZO(1.04719758f, 16.0f / 9.0f, 0.1f, 80.0f);
    Lcg random;
    std::vector<engine::ClusteredLightInput> lights;
    for (int i = 0; i < 700; ++i) {
        engine::ClusteredLightInput light{{random.range(-24.0f, 24.0f), random.range(0.0f, 7.0f),
                                           random.range(-30.0f, 20.0f)},
                                          {random.range(0.1f, 1.0f), random.range(0.1f, 1.0f),
                                           random.range(0.1f, 1.0f)},
                                          random.range(0.4f, 4.0f)};
        if (i % 37 == 0) light.radiance = {}; // culled for having no energy
        if (i % 11 == 0) light.shadowSlot = static_cast<std::uint32_t>(i % 5);
        lights.push_back(light);
    }
    // A light around the camera crosses the near plane and covers every tile.
    lights.push_back({{0.0f, 6.0f, 17.5f}, {1, 1, 1}, 2.0f});

    for (const std::uint32_t cap : {engine::ClusterGridConfig{}.maxIndexCount, 3000u}) {
        CAPTURE(cap);
        engine::ClusterGridConfig config;
        config.maxIndexCount = cap;
        containers::Array<std::uint32_t> cpu;
        const auto cpuStats =
            engine::buildClusteredLightBuffer(lights, view, projection, 0.1f, 80.0f, config, cpu);
        // No reserved words: the GPU output must be the CPU buffer word for word.
        containers::Array<std::uint32_t> input;
        engine::ClusteredLightGpuParams params;
        const auto gpuStats = engine::buildClusteredLightBinningInput(
            lights, view, projection, 0.1f, 80.0f, config, 0, input, params);
        CHECK(gpuStats.submitted == cpuStats.submitted);
        CHECK(gpuStats.visible == cpuStats.visible);
        CHECK(gpuStats.indexCount == cpuStats.indexCount);
        CHECK(gpuStats.droppedIndices == cpuStats.droppedIndices);
        CHECK(cpuStats.visible > 200);
        if (cap == 3000u) CHECK(cpuStats.droppedIndices > 0);
        REQUIRE(params.totalWords == cpu.size());
        const std::vector<std::uint32_t> emulated = emulateGpuBinning(input, params);
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < cpu.size(); ++i) {
            if (emulated[i] != cpu[i]) ++mismatches;
        }
        CHECK(mismatches == 0);

        // Reserved words (the renderer's shadow matrices) only move the grid.
        containers::Array<std::uint32_t> shifted;
        engine::ClusteredLightGpuParams shiftedParams;
        (void)engine::buildClusteredLightBinningInput(lights, view, projection, 0.1f, 80.0f,
                                                      config, 96, shifted, shiftedParams);
        CHECK(shiftedParams.gridOffset == params.gridOffset + 96);
        CHECK(shiftedParams.reservedOffset == params.gridOffset);
        const std::vector<std::uint32_t> moved = emulateGpuBinning(shifted, shiftedParams);
        CHECK(compareLightBuffers(std::span<const std::uint32_t>(cpu.data(), cpu.size()),
                                  std::span<const std::uint32_t>(moved)) == 0);
    }
}

TEST_CASE("Point lights: the null device always bins on the CPU") {
    gfx::NullDevice device;
    ecs::Registry registry(256);
    engine::Renderer renderer(registry, device);
    REQUIRE(renderer.init() == gfx::GfxError::None);
    NightSceneOptions options;
    options.lightsPerSide = 4;
    const ecs::Entity camera = buildNightScene(renderer, registry, options);
    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {64, 64};
    framebufferDesc.attachments.push(gfx::AttachmentDesc{});
    auto framebuffer = device.createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());
    REQUIRE(renderer.pbrSettings().gpuLightBinning);
    REQUIRE(renderer.render(framebuffer.value(), camera, 1.0f) == gfx::GfxError::None);
    CHECK(renderer.stats().pointLightBinningDispatches == 0);
    CHECK(renderer.stats().pointLightsVisible > 0);
    CHECK(device.stats().dispatchCalls == 0);
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

TEST_CASE("Point-light shadows: face selection, face matrices, and atlas tiles agree") {
    CHECK(engine::pointShadowFaceFor({1.0f, 0.2f, -0.1f}) == 0);
    CHECK(engine::pointShadowFaceFor({-1.0f, 0.2f, 0.3f}) == 1);
    CHECK(engine::pointShadowFaceFor({0.1f, 2.0f, 0.3f}) == 2);
    CHECK(engine::pointShadowFaceFor({0.1f, -2.0f, 0.3f}) == 3);
    CHECK(engine::pointShadowFaceFor({0.1f, 0.2f, 3.0f}) == 4);
    CHECK(engine::pointShadowFaceFor({0.1f, 0.2f, -3.0f}) == 5);

    const math::Vec3 light(1.0f, 2.0f, -3.0f);
    const float range = 5.0f;
    const math::Vec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (std::uint32_t face = 0; face < 6; ++face) {
        const math::Mat4 viewProj = engine::pointShadowFaceViewProj(light, range, face);
        const math::Vec4 clip = viewProj.mul(math::Vec4(light + axes[face] * 2.0f, 1.0f));
        REQUIRE(clip.w > 0.0f);
        CHECK(std::abs(clip.x / clip.w) < 1.0e-4f);
        CHECK(std::abs(clip.y / clip.w) < 1.0e-4f);
        CHECK(clip.z / clip.w > 0.0f);
        CHECK(clip.z / clip.w < 1.0f);
    }
    // Any direction lands inside the [-1, 1] square of the face the shaders pick.
    Lcg random;
    for (int i = 0; i < 2000; ++i) {
        const math::Vec3 d(random.range(-1.0f, 1.0f), random.range(-1.0f, 1.0f),
                           random.range(-1.0f, 1.0f));
        if (math::length(d) < 0.05f) continue;
        const math::Vec3 point = light + d * (range * 0.9f / math::length(d));
        const std::uint32_t face = engine::pointShadowFaceFor(point - light);
        const math::Vec4 clip =
            engine::pointShadowFaceViewProj(light, range, face).mul(math::Vec4(point, 1.0f));
        REQUIRE(clip.w > 0.0f);
        CHECK(std::abs(clip.x / clip.w) <= 1.0001f);
        CHECK(std::abs(clip.y / clip.w) <= 1.0001f);
    }

    const math::Vec4 tile = engine::pointShadowTileTransform(1, 2, 4);
    CHECK(tile.x == doctest::Approx(1.0f / 6.0f));
    CHECK(tile.y == doctest::Approx(0.25f));
    CHECK(tile.z == doctest::Approx(-1.0f + 5.0f / 6.0f));
    CHECK(tile.w == doctest::Approx(-1.0f + 3.0f / 4.0f));
    // Tiles of one row cover [-1, 1] in X without overlap.
    CHECK(engine::pointShadowTileTransform(0, 0, 1).z - 1.0f / 6.0f == doctest::Approx(-1.0f));
    CHECK(engine::pointShadowTileTransform(0, 5, 1).z + 1.0f / 6.0f == doctest::Approx(1.0f));

    CHECK(engine::pointShadowAtlasBinding(gfx::Backend::Vulkan).slot == 8);
    CHECK(engine::pointShadowAtlasBinding(gfx::Backend::Metal).bindPoint == 1);
    CHECK(engine::pointShadowAtlasBinding(gfx::Backend::Metal).slot == 0);
}

TEST_CASE("Point-light shadows: the budget keeps the brightest lights nearest the camera") {
    const std::vector<engine::PointShadowCandidate> candidates = {
        {{10.0f, 0.0f, 0.0f}, {5.0f, 5.0f, 5.0f}, 1.0f},  // bright but far: 5 / 82
        {{2.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 1.0f},   // 1 / 2
        {{0.0f, 0.0f, 3.0f}, {4.0f, 1.0f, 1.0f}, 0.5f},   // 4 / 7.25
        {{0.5f, 0.0f, 0.0f}, {0.2f, 0.2f, 0.2f}, 1.0f},   // camera inside: 0.2
        {{-2.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 1.0f},  // ties with [1]
        {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 3.0f},   // no energy: never picked
        {{0.0f, -1.0f, 0.0f}, {9.0f, 9.0f, 9.0f}, 0.0f},  // no range: never picked
    };
    containers::Array<std::uint32_t> selected;
    engine::selectShadowedPointLights(candidates, {0.0f, 0.0f, 0.0f}, 3, selected);
    REQUIRE(selected.size() == 3);
    CHECK(selected[0] == 2);
    CHECK(selected[1] == 1);
    CHECK(selected[2] == 4);
    engine::selectShadowedPointLights(candidates, {0.0f, 0.0f, 0.0f}, 16, selected);
    CHECK(selected.size() == 5);
    CHECK(selected[3] == 3);
    CHECK(selected[4] == 0);
    engine::selectShadowedPointLights(candidates, {0.0f, 0.0f, 0.0f}, 0, selected);
    CHECK(selected.size() == 0);
}

TEST_CASE("Point-light shadows: the renderer assigns atlas slots and writes face matrices") {
    gfx::NullDevice device;
    ecs::Registry registry(4096);
    engine::Renderer renderer(registry, device);
    REQUIRE(renderer.init() == gfx::GfxError::None);
    NightSceneOptions options;
    options.lightsPerSide = 4;
    options.extent = 6.0f;
    options.range = 4.0f;
    options.castShadows = true;
    options.shadowBudget = 3;
    options.pillarsPerSide = 4;
    const ecs::Entity cameraEntity = buildNightScene(renderer, registry, options);

    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {64, 64};
    framebufferDesc.attachments.push(gfx::AttachmentDesc{});
    auto framebuffer = device.createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());
    REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f) == gfx::GfxError::None);
    CHECK(renderer.stats().pointShadowLights == 3);
    CHECK(renderer.stats().pointShadowDraws > 0);
    {
        const auto words = renderer.pointLightWords();
        REQUIRE(words.size() > engine::kPointLightHeaderWords);
        CHECK(words[10] != 0);
        CHECK(words[11] == 3);
        CHECK(words[12] == 3);
        CHECK(wordFloat(words, 13) == doctest::Approx(renderer.pbrSettings().pointShadowBias));
        CHECK(wordFloat(words, 14) == doctest::Approx(2.0f / 256.0f));
        CHECK(words.size() >= words[10] + 3u * engine::kPointShadowFaces *
                                              engine::kPointShadowMatrixWords);
        std::array<std::uint32_t, 4> slotUse{};
        for (std::uint32_t light = 0; light < words[0]; ++light) {
            const std::uint32_t slot = words[words[4] + light * engine::kPointLightRecordWords + 7];
            REQUIRE(slot <= 3);
            ++slotUse[slot];
            if (slot == 0) continue;
            // The stored +X face matrix maps a point 1 m along +X to the face centre.
            const std::uint32_t record = words[4] + light * engine::kPointLightRecordWords;
            const math::Vec3 position(wordFloat(words, record), wordFloat(words, record + 1),
                                      wordFloat(words, record + 2));
            const std::uint32_t base =
                words[10] + (slot - 1u) * engine::kPointShadowFaces * engine::kPointShadowMatrixWords;
            const math::Vec4 p(position.x + 1.0f, position.y, position.z, 1.0f);
            math::Vec4 clip{};
            for (int row = 0; row < 4; ++row) {
                float value = 0.0f;
                for (int column = 0; column < 4; ++column) {
                    const float component = column == 0 ? p.x : column == 1 ? p.y : column == 2 ? p.z : p.w;
                    value += wordFloat(words, base + static_cast<std::uint32_t>(column * 4 + row)) *
                             component;
                }
                (row == 0 ? clip.x : row == 1 ? clip.y : row == 2 ? clip.z : clip.w) = value;
            }
            REQUIRE(clip.w > 0.0f);
            CHECK(std::abs(clip.x / clip.w) < 1.0e-3f);
            CHECK(std::abs(clip.y / clip.w) < 1.0e-3f);
        }
        CHECK(slotUse[1] == 1);
        CHECK(slotUse[2] == 1);
        CHECK(slotUse[3] == 1);
    }

    // Disabled, or no casters: no slots, and the header says so.
    renderer.pbrSettings().pointShadowsEnabled = false;
    REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f) == gfx::GfxError::None);
    CHECK(renderer.stats().pointShadowLights == 0);
    CHECK(renderer.stats().pointShadowDraws == 0);
    CHECK(renderer.pointLightWords()[10] == 0);
    CHECK(renderer.pointLightWords()[11] == 0);
    renderer.pbrSettings().pointShadowsEnabled = true;
    renderer.pbrSettings().pointShadowBudget = 0;
    REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f) == gfx::GfxError::None);
    CHECK(renderer.stats().pointShadowLights == 0);
    CHECK(renderer.pointLightWords()[10] == 0);
    CHECK(device.destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
}

TEST_CASE("Point-light shadows: atlas slots stay with their light across frames") {
    constexpr std::uint64_t kFree = engine::kPointShadowFreeSlot;
    containers::Array<std::uint64_t> owners;
    owners.resize(4);
    std::fill(owners.begin(), owners.end(), kFree);
    containers::Array<std::uint32_t> slots;

    const std::vector<std::uint64_t> first = {10, 20, 30};
    engine::assignStablePointShadowSlots(first, owners, slots);
    REQUIRE(slots.size() == 3);
    CHECK(slots[0] == 0);
    CHECK(slots[1] == 1);
    CHECK(slots[2] == 2);
    CHECK(owners[3] == kFree);

    // 20 drops out, 40 is new and ranked first: 10 and 30 keep their slots,
    // 40 takes the lowest free one.
    const std::vector<std::uint64_t> second = {40, 30, 10};
    engine::assignStablePointShadowSlots(second, owners, slots);
    CHECK(slots[0] == 1);
    CHECK(slots[1] == 2);
    CHECK(slots[2] == 0);
    CHECK(owners[0] == 10);
    CHECK(owners[1] == 40);
    CHECK(owners[2] == 30);
    CHECK(owners[3] == kFree);

    // Everything leaves: every slot is freed.
    engine::assignStablePointShadowSlots({}, owners, slots);
    CHECK(slots.size() == 0);
    for (const std::uint64_t owner : owners) CHECK(owner == kFree);
}

TEST_CASE("Point-light shadows: unchanged lights and casters reuse their tiles") {
    gfx::NullDevice device;
    ecs::Registry registry(4096);
    engine::Renderer renderer(registry, device);
    REQUIRE(renderer.init() == gfx::GfxError::None);
    NightSceneOptions options;
    options.lightsPerSide = 4;
    options.extent = 6.0f;
    options.range = 4.0f;
    options.castShadows = true;
    options.shadowBudget = 3;
    options.pillarsPerSide = 4;
    const ecs::Entity cameraEntity = buildNightScene(renderer, registry, options);
    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {64, 64};
    framebufferDesc.attachments.push(gfx::AttachmentDesc{});
    auto framebuffer = device.createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());
    const auto render = [&] {
        REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f) == gfx::GfxError::None);
        return renderer.stats();
    };

    const engine::FrameStats first = render();
    CHECK(first.pointShadowLights == 3);
    CHECK(first.pointShadowLightsRedrawn == 3);
    CHECK(first.pointShadowDraws > 0);
    const std::vector<std::uint32_t> firstWords(renderer.pointLightWords().begin(),
                                                renderer.pointLightWords().end());

    const engine::FrameStats second = render();
    CHECK(second.pointShadowLights == 3);
    CHECK(second.pointShadowLightsRedrawn == 0);
    CHECK(second.pointShadowDraws == 0);
    // Same slots, same matrices: the light buffer is unchanged.
    const std::vector<std::uint32_t> secondWords(renderer.pointLightWords().begin(),
                                                 renderer.pointLightWords().end());
    CHECK(firstWords == secondWords);

    // A caster moves: only lights whose sphere reaches it (before or after)
    // are redrawn.
    ecs::Entity pillar{};
    for (auto [entity, meshRenderer] : ecs::View<engine::MeshRenderer>(registry)) {
        if (meshRenderer.castShadows) pillar = entity;
    }
    REQUIRE(registry.get<ecs::WorldMatrix>(pillar) != nullptr);
    registry.get<ecs::WorldMatrix>(pillar)->matrix.col[3].y += 0.1f;
    const engine::FrameStats moved = render();
    CHECK(moved.pointShadowLightsRedrawn > 0);
    CHECK(moved.pointShadowLightsRedrawn < 3);
    const engine::FrameStats settled = render();
    CHECK(settled.pointShadowLightsRedrawn == 0);

    // An uncached frame leaves nothing valid to reuse.
    renderer.pbrSettings().pointShadowCaching = false;
    const engine::FrameStats uncached = render();
    CHECK(uncached.pointShadowLightsRedrawn == 3);
    renderer.pbrSettings().pointShadowCaching = true;
    const engine::FrameStats recached = render();
    CHECK(recached.pointShadowLightsRedrawn == 3); // nothing valid after an uncached frame
    CHECK(render().pointShadowLightsRedrawn == 0);

    // A new tile size rebuilds the atlas and every slot.
    renderer.pbrSettings().pointShadowTileSize = 128;
    CHECK(render().pointShadowLightsRedrawn == 3);
    CHECK(render().pointShadowLightsRedrawn == 0);
    CHECK(device.destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
}

TEST_CASE("Bloom: the renderer requires an extent and declares the full mip chain") {
    gfx::NullDevice device;
    ecs::Registry registry(4096);
    engine::Renderer renderer(registry, device);
    REQUIRE(renderer.init() == gfx::GfxError::None);
    NightSceneOptions options;
    options.lightsPerSide = 4;
    options.bloom = true;
    const ecs::Entity cameraEntity = buildNightScene(renderer, registry, options);
    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {64, 64};
    framebufferDesc.attachments.push(gfx::AttachmentDesc{});
    auto framebuffer = device.createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());
    CHECK(!engine::PbrRenderSettings{}.bloomEnabled);
    CHECK(renderer.render(framebuffer.value(), cameraEntity, 1.0f) ==
          gfx::GfxError::BadDescriptor);
    REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f, gfx::Extent{64, 64}) ==
            gfx::GfxError::None);
    CHECK(renderer.stats().bloomPasses == 10); // prefilter + 4 down + 4 up + composite
    CHECK(renderer.stats().draws > 0);
    renderer.pbrSettings().bloomLevels = 1;
    REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f, gfx::Extent{64, 64}) ==
            gfx::GfxError::None);
    CHECK(renderer.stats().bloomPasses == 2);
    renderer.pbrSettings().bloomEnabled = false;
    REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f) == gfx::GfxError::None);
    CHECK(renderer.stats().bloomPasses == 0);
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
        REQUIRE(renderer.render(framebuffer.value(), cameraEntity, aspect,
                                gfx::Extent{width, height}) == gfx::GfxError::None);
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

namespace {

// Bloom and point-shadow frames for one backend.
struct EffectCaptures {
    Capture shadowOff;   // pillars, no light flagged
    Capture shadowOn;    // pillars, 8 of 16 lights shadowed
    Capture bloomOff;    // night scene, bloom disabled
    Capture bloomZero;   // night scene, bloom enabled at intensity 0
    Capture bloomOn;     // night scene, bloom enabled
    Capture combined;    // pillars + shadows + bloom
    engine::FrameStats shadowStats{};
    engine::FrameStats bloomStats{};
};

constexpr std::uint32_t kEffectWidth = 160;
constexpr std::uint32_t kEffectHeight = 96;

[[nodiscard]] auto shadowSceneOptions() -> NightSceneOptions {
    NightSceneOptions options;
    options.lightsPerSide = 4;
    options.extent = 6.0f;
    options.lightHeight = 0.7f;
    options.range = 4.5f;
    options.intensity = 8.0f;
    options.pillarsPerSide = 4;
    options.shadowBudget = 8;
    return options;
}

template <typename Reader>
[[nodiscard]] auto captureEffects(gfx::IDevice& device, Reader&& reader) -> EffectCaptures {
    EffectCaptures result;
    NightSceneOptions shadows = shadowSceneOptions();
    result.shadowOff = captureNightFrame(device, shadows, kEffectWidth, kEffectHeight, reader);
    shadows.castShadows = true;
    result.shadowOn = captureNightFrame(device, shadows, kEffectWidth, kEffectHeight, reader,
                                        &result.shadowStats);
    NightSceneOptions bloom;
    result.bloomOff = captureNightFrame(device, bloom, kEffectWidth, kEffectHeight, reader);
    bloom.bloom = true;
    bloom.bloomIntensity = 0.0f;
    result.bloomZero = captureNightFrame(device, bloom, kEffectWidth, kEffectHeight, reader);
    bloom.bloomIntensity = 1.0f;
    bloom.bloomThreshold = 0.6f;
    result.bloomOn = captureNightFrame(device, bloom, kEffectWidth, kEffectHeight, reader,
                                       &result.bloomStats);
    shadows.bloom = true;
    shadows.bloomIntensity = 1.0f;
    shadows.bloomThreshold = 0.6f;
    result.combined = captureNightFrame(device, shadows, kEffectWidth, kEffectHeight, reader);
    return result;
}

void writeEffectCaptures(const char* backend, const EffectCaptures& captures) {
    const std::string suffix = std::string("_") + backend;
    maybeWriteCapture(("night_point_shadows_off" + suffix).c_str(), captures.shadowOff,
                      kEffectWidth, kEffectHeight);
    maybeWriteCapture(("night_point_shadows" + suffix).c_str(), captures.shadowOn, kEffectWidth,
                      kEffectHeight);
    maybeWriteCapture(("night_bloom" + suffix).c_str(), captures.bloomOn, kEffectWidth,
                      kEffectHeight);
    maybeWriteCapture(("night_bloom_zero" + suffix).c_str(), captures.bloomZero, kEffectWidth,
                      kEffectHeight);
    maybeWriteCapture(("night_shadows_bloom" + suffix).c_str(), captures.combined, kEffectWidth,
                      kEffectHeight);
}

void checkEffectParity(const std::string& name, const Capture& reference,
                       const Capture& other) {
    const auto result = engine::compareShaderImagesRgba8(reference, other, 2);
    MESSAGE(name << ": mismatches=" << result.mismatchPixelCount
                 << " max delta=" << static_cast<int>(result.maxChannelDelta));
    CHECK(result.matches());
}

} // namespace

TEST_CASE("Bloom and point-light shadows match across OpenGL, Vulkan, and DX12") {
    auto windowResult =
        platform::Window::create({"Bloom and point shadows", kEffectWidth, kEffectHeight, false});
    REQUIRE(windowResult.hasValue());
    platform::Window* window = windowResult.value();
    REQUIRE(window->makeCurrent());
    REQUIRE(platform::initGl(4, 3).hasValue());
    gfx::IDevice* openGlBase = gfx::createOpenGLDevice();
    REQUIRE(openGlBase != nullptr);
    const auto gl = captureEffects(*openGlBase, [](gfx::TextureHandle, std::span<std::byte> out) {
        glReadPixels(0, 0, static_cast<GLsizei>(kEffectWidth), static_cast<GLsizei>(kEffectHeight),
                     GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        return glGetError() == GL_NO_ERROR ? gfx::GfxError::None : gfx::GfxError::DeviceLost;
    });
    gfx::deleteDevice(openGlBase);
    delete window;

    gfx::IDevice* vulkanBase = gfx::createVulkanDevice();
    REQUIRE(vulkanBase != nullptr);
    auto* vulkan = static_cast<gfx::VulkanDevice*>(vulkanBase);
    const auto vk = captureEffects(*vulkan, [vulkan](gfx::TextureHandle texture,
                                                     std::span<std::byte> out) {
        return vulkan->readTexture(texture, 0, out);
    });
    CHECK(vulkan->validationWarningCount() == 0);
    CHECK(vulkan->validationErrorCount() == 0);
    gfx::deleteDevice(vulkanBase);

    gfx::IDevice* dx12Base = gfx::createDx12Device();
    REQUIRE(dx12Base != nullptr);
    auto* dx12 = static_cast<gfx::Dx12Device*>(dx12Base);
    const auto dx = captureEffects(*dx12, [dx12](gfx::TextureHandle texture,
                                                 std::span<std::byte> out) {
        return dx12->readTexture(texture, 0, out);
    });
    CHECK(dx12->validationWarningCount() == 0);
    CHECK(dx12->validationErrorCount() == 0);
    gfx::deleteDevice(dx12Base);

    writeEffectCaptures("opengl", gl);
    writeEffectCaptures("vulkan", vk);
    writeEffectCaptures("dx12", dx);

    // Point shadows: 8 slots used, shadows only ever darken, and they darken
    // a real area of the ground behind the pillars.
    CHECK(gl.shadowStats.pointShadowLights == 8);
    CHECK(gl.shadowStats.pointShadowDraws > 0);
    std::size_t darkened = 0;
    std::size_t brightened = 0;
    for (std::size_t i = 0; i + 3 < gl.shadowOn.size(); i += 4) {
        int loss = 0;
        for (std::size_t c = 0; c < 3; ++c) {
            const int delta = static_cast<int>(gl.shadowOff[i + c]) - gl.shadowOn[i + c];
            loss = std::max(loss, delta);
            if (delta < -1) ++brightened;
        }
        if (loss > 8) ++darkened;
    }
    MESSAGE("point shadows: darkened pixels=" << darkened << " brightened channels="
            << brightened << " luma off=" << meanLuma(gl.shadowOff)
            << " on=" << meanLuma(gl.shadowOn) << " draws=" << gl.shadowStats.pointShadowDraws);
    CHECK(darkened > 150);
    CHECK(brightened == 0);

    // Bloom: intensity 0 reproduces the non-bloom frame (fp16 HDR rounding
    // only); a real intensity brightens the frame around the emissive lamps.
    CHECK(gl.bloomStats.bloomPasses == 10);
    const auto zeroGl = engine::compareShaderImagesRgba8(gl.bloomOff, gl.bloomZero, 2);
    const auto zeroVk = engine::compareShaderImagesRgba8(vk.bloomOff, vk.bloomZero, 2);
    const auto zeroDx = engine::compareShaderImagesRgba8(dx.bloomOff, dx.bloomZero, 2);
    MESSAGE("bloom intensity 0 vs off: GL max delta=" << static_cast<int>(zeroGl.maxChannelDelta)
            << " VK=" << static_cast<int>(zeroVk.maxChannelDelta)
            << " DX=" << static_cast<int>(zeroDx.maxChannelDelta));
    CHECK(zeroGl.matches());
    CHECK(zeroVk.matches());
    CHECK(zeroDx.matches());
    std::size_t glowing = 0;
    std::size_t dimmed = 0;
    for (std::size_t i = 0; i + 3 < gl.bloomOn.size(); i += 4) {
        bool glow = false;
        for (std::size_t c = 0; c < 3; ++c) {
            if (gl.bloomOn[i + c] > gl.bloomOff[i + c] + 3) glow = true;
            if (gl.bloomOn[i + c] + 2 < gl.bloomOff[i + c]) ++dimmed;
        }
        if (glow) ++glowing;
    }
    MESSAGE("bloom: glowing pixels=" << glowing << " luma off=" << meanLuma(gl.bloomOff)
            << " on=" << meanLuma(gl.bloomOn));
    CHECK(glowing > 500);
    CHECK(dimmed == 0);
    CHECK(meanLuma(gl.bloomOn) > meanLuma(gl.bloomOff) + 1.0);

    checkEffectParity("shadows GL/VK", gl.shadowOn, vk.shadowOn);
    checkEffectParity("shadows GL/DX", gl.shadowOn, dx.shadowOn);
    checkEffectParity("bloom GL/VK", gl.bloomOn, vk.bloomOn);
    checkEffectParity("bloom GL/DX", gl.bloomOn, dx.bloomOn);
    checkEffectParity("shadows+bloom GL/VK", gl.combined, vk.combined);
    checkEffectParity("shadows+bloom GL/DX", gl.combined, dx.combined);
}

namespace {

// Mirror of validateRendererFrame() in src/app/f4_webgpu_app.cpp: the same
// 64x64 lit triangle with a red point light, a small shadow-casting triangle
// between them, and then bloom. The browser smoke compares its WebGPU frames
// with the reference files this test locks, which is how WebGPU joins the
// OpenGL / Vulkan / DX12 parity check.
constexpr std::uint32_t kWebFrameExtent = 64;

struct WebValidationFrames {
    Capture pointShadow;
    Capture bloom;
};

[[nodiscard]] auto webValidationTexture(std::uint8_t red, std::uint8_t green, std::uint8_t blue)
    -> resources::Texture {
    resources::Texture texture;
    texture.format = resources::TextureFormat::RGBA8_UNORM;
    resources::TextureMip mip;
    mip.width = 2;
    mip.height = 2;
    mip.bytes.resize(16);
    for (std::size_t offset = 0; offset < mip.bytes.size(); offset += 4) {
        mip.bytes[offset + 0] = static_cast<std::byte>(red);
        mip.bytes[offset + 1] = static_cast<std::byte>(green);
        mip.bytes[offset + 2] = static_cast<std::byte>(blue);
        mip.bytes[offset + 3] = std::byte{255};
    }
    texture.mips.push(static_cast<resources::TextureMip&&>(mip));
    return texture;
}

[[nodiscard]] auto webValidationTriangle() -> resources::Mesh {
    resources::Mesh mesh = makeMesh({-0.6f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                     0.6f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
                                     0.0f, 0.6f, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f},
                                    {0, 1, 2});
    return mesh;
}

template <typename Reader>
[[nodiscard]] auto captureWebValidationFrames(gfx::IDevice& device, Reader&& reader)
    -> WebValidationFrames {
    constexpr std::uint32_t size = kWebFrameExtent;
    gfx::TextureDesc colorDesc;
    colorDesc.width = size;
    colorDesc.height = size;
    auto color = device.createTexture(colorDesc);
    REQUIRE(color.hasValue());
    gfx::TextureDesc depthDesc;
    depthDesc.width = size;
    depthDesc.height = size;
    depthDesc.format = gfx::TextureFormat::Depth32F;
    auto depth = device.createTexture(depthDesc);
    REQUIRE(depth.hasValue());
    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {size, size};
    gfx::AttachmentDesc colorAttachment;
    colorAttachment.texture = color.value();
    colorAttachment.clearOnLoad = true;
    colorAttachment.clear.color[0] = 0.01f;
    colorAttachment.clear.color[1] = 0.015f;
    colorAttachment.clear.color[2] = 0.02f;
    framebufferDesc.attachments.push(colorAttachment);
    gfx::AttachmentDesc depthAttachment;
    depthAttachment.type = gfx::AttachmentType::Depth;
    depthAttachment.texture = depth.value();
    depthAttachment.clearOnLoad = true;
    framebufferDesc.attachments.push(depthAttachment);
    auto framebuffer = device.createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());

    const auto read = [&](Capture& pixels) {
        pixels.assign(static_cast<std::size_t>(size) * size * 4u, 0);
        device.waitIdle();
        REQUIRE(reader(color.value(), std::as_writable_bytes(std::span(pixels))) ==
                gfx::GfxError::None);
        if (device.backend() == gfx::Backend::OpenGL) {
            const std::size_t row = static_cast<std::size_t>(size) * 4u;
            for (std::uint32_t y = 0; y < size / 2; ++y) {
                std::swap_ranges(pixels.begin() + static_cast<std::ptrdiff_t>(y * row),
                                 pixels.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                                 pixels.begin() +
                                     static_cast<std::ptrdiff_t>((size - 1 - y) * row));
            }
        }
    };

    WebValidationFrames frames;
    {
        ecs::Registry registry(16);
        engine::Renderer renderer(registry, device);
        renderer.pbrSettings().shadowsEnabled = false;
        renderer.pbrSettings().ambientIntensity = 0.2f;
        REQUIRE(renderer.init() == gfx::GfxError::None);
        const auto albedo = renderer.createTexture(webValidationTexture(205, 96, 35));
        const auto normal = renderer.createTexture(webValidationTexture(145, 178, 238));
        const auto metallicRoughness = renderer.createTexture(webValidationTexture(0, 140, 76));
        engine::StandardMaterialDesc materialDesc;
        materialDesc.albedo = albedo;
        materialDesc.normal = normal;
        materialDesc.metallicRoughness = metallicRoughness;
        materialDesc.metallicFactor = 0.8f;
        materialDesc.roughnessFactor = 0.85f;
        const auto material = renderer.createMaterial(materialDesc);
        const auto mesh = renderer.createMesh(webValidationTriangle());
        REQUIRE(material.isValid());
        REQUIRE(mesh.isValid());
        const ecs::Entity cameraEntity = registry.create();
        engine::Camera camera;
        camera.eye = {0.0f, 0.0f, 2.0f};
        camera.center = {0.0f, 0.0f, 0.0f};
        registry.emplace<engine::Camera>(cameraEntity, camera);
        const ecs::Entity lightEntity = registry.create();
        engine::DirectionalLight light;
        light.direction = {-0.25f, -0.15f, -1.0f};
        light.color = {1.0f, 0.92f, 0.8f};
        light.intensity = 2.5f;
        registry.emplace<engine::DirectionalLight>(lightEntity, light);
        const ecs::Entity object = registry.create();
        registry.emplace<ecs::LocalTransform>(object, ecs::LocalTransform{});
        registry.emplace<engine::MeshRenderer>(object,
                                               engine::MeshRenderer{mesh, material, 0, false, false});
        const ecs::Entity pointEntity = registry.create();
        engine::PointLight point;
        point.position = {0.0f, 0.0f, 0.6f};
        point.color = {1.0f, 0.05f, 0.05f};
        point.intensity = 4.0f;
        point.range = 2.0f;
        point.castShadows = true;
        registry.emplace<engine::PointLight>(pointEntity, point);
        const ecs::Entity occluder = registry.create();
        auto& occluderTransform =
            registry.emplace<ecs::LocalTransform>(occluder, ecs::LocalTransform{});
        occluderTransform.position = {0.15f, -0.2f, 0.3f};
        occluderTransform.scale = math::Vec3(0.2f);
        registry.emplace<engine::MeshRenderer>(occluder,
                                               engine::MeshRenderer{mesh, material, 0, true, false});
        ecs::updateWorldTransforms(registry);

        const gfx::Extent extent{size, size};
        REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f, extent) ==
                gfx::GfxError::None);
        CHECK(renderer.stats().pointShadowLights == 1);
        read(frames.pointShadow);
        renderer.pbrSettings().bloomEnabled = true;
        renderer.pbrSettings().bloomIntensity = 1.5f;
        renderer.pbrSettings().bloomThreshold = 0.3f;
        REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f, extent) ==
                gfx::GfxError::None);
        CHECK(renderer.stats().bloomPasses > 2);
        read(frames.bloom);
    }
    CHECK(device.destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
    CHECK(device.destroyTexture(depth.value()) == gfx::GfxError::None);
    CHECK(device.destroyTexture(color.value()) == gfx::GfxError::None);
    return frames;
}

[[nodiscard]] auto readFixture(const std::string& path) -> Capture {
    Capture bytes;
    if (std::FILE* file = std::fopen(path.c_str(), "rb")) {
        bytes.resize(static_cast<std::size_t>(kWebFrameExtent) * kWebFrameExtent * 4u);
        const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
        if (read != bytes.size()) bytes.clear();
    }
    return bytes;
}

void writeFixture(const std::string& path, const Capture& bytes) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    REQUIRE(file != nullptr);
    CHECK(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    std::fclose(file);
}

} // namespace

namespace {

// Offscreen colour + depth target shared by the multi-frame tests below.
struct NightTarget {
    gfx::IDevice& device;
    std::uint32_t width;
    std::uint32_t height;
    gfx::TextureHandle color{};
    gfx::TextureHandle depth{};
    gfx::FramebufferHandle framebuffer{};

    NightTarget(gfx::IDevice& owner, std::uint32_t w, std::uint32_t h)
        : device(owner), width(w), height(h) {
        gfx::TextureDesc colorDesc;
        colorDesc.width = width;
        colorDesc.height = height;
        colorDesc.hasOptimizedClear = true;
        colorDesc.optimizedClear.color[3] = 1.0f;
        auto colorResult = device.createTexture(colorDesc);
        REQUIRE(colorResult.hasValue());
        color = colorResult.value();
        gfx::TextureDesc depthDesc;
        depthDesc.width = width;
        depthDesc.height = height;
        depthDesc.format = gfx::TextureFormat::Depth32F;
        depthDesc.hasOptimizedClear = true;
        depthDesc.optimizedClear.depth = 1.0f;
        auto depthResult = device.createTexture(depthDesc);
        REQUIRE(depthResult.hasValue());
        depth = depthResult.value();
        gfx::FramebufferDesc desc;
        desc.extent = {width, height};
        gfx::AttachmentDesc colorAttachment;
        colorAttachment.texture = color;
        colorAttachment.clearOnLoad = true;
        colorAttachment.clear.color[3] = 1.0f;
        desc.attachments.push(colorAttachment);
        gfx::AttachmentDesc depthAttachment;
        depthAttachment.type = gfx::AttachmentType::Depth;
        depthAttachment.texture = depth;
        depthAttachment.clearOnLoad = true;
        desc.attachments.push(depthAttachment);
        auto framebufferResult = device.createFramebuffer(desc);
        REQUIRE(framebufferResult.hasValue());
        framebuffer = framebufferResult.value();
    }
    ~NightTarget() {
        device.waitIdle();
        (void)device.destroyFramebuffer(framebuffer);
        (void)device.destroyTexture(depth);
        (void)device.destroyTexture(color);
    }
    NightTarget(const NightTarget&) = delete;
    NightTarget& operator=(const NightTarget&) = delete;

    template <typename Reader>
    [[nodiscard]] auto render(engine::Renderer& renderer, ecs::Entity camera, Reader& reader)
        -> Capture {
        REQUIRE(renderer.render(framebuffer, camera,
                                static_cast<float>(width) / static_cast<float>(height),
                                gfx::Extent{width, height}) == gfx::GfxError::None);
        device.waitIdle();
        Capture pixels(static_cast<std::size_t>(width) * height * 4u, 0);
        REQUIRE(reader(color, std::as_writable_bytes(std::span(pixels))) == gfx::GfxError::None);
        if (device.backend() == gfx::Backend::OpenGL) {
            const std::size_t row = static_cast<std::size_t>(width) * 4u;
            for (std::uint32_t y = 0; y < height / 2; ++y) {
                std::swap_ranges(pixels.begin() + static_cast<std::ptrdiff_t>(y * row),
                                 pixels.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                                 pixels.begin() +
                                     static_cast<std::ptrdiff_t>((height - 1 - y) * row));
            }
        }
        return pixels;
    }
};

// One night scene rendered by its own renderer; the cache test keeps two of
// them in lock step, one with the shadow cache and one without.
struct NightWorld {
    ecs::Registry registry{4096};
    engine::Renderer renderer;
    ecs::Entity camera{};
    std::vector<ecs::Entity> lights;
    std::vector<ecs::Entity> casters;

    NightWorld(gfx::IDevice& device, const NightSceneOptions& options) : renderer(registry, device) {
        REQUIRE(renderer.init() == gfx::GfxError::None);
        camera = buildNightScene(renderer, registry, options);
        for (auto [entity, light] : ecs::View<engine::PointLight>(registry)) {
            (void)light;
            lights.push_back(entity);
        }
        for (auto [entity, meshRenderer] : ecs::View<engine::MeshRenderer>(registry)) {
            if (meshRenderer.castShadows) casters.push_back(entity);
        }
    }
};

struct CacheFrame {
    const char* step;
    Capture cached;
    Capture uncached;
    engine::FrameStats cachedStats{};
    engine::FrameStats uncachedStats{};
};

[[nodiscard]] auto shadowCacheOptions() -> NightSceneOptions {
    NightSceneOptions options = shadowSceneOptions();
    options.castShadows = true;
    options.shadowBudget = 4; // 16 flagged lights compete for 4 slots
    return options;
}

struct SequenceFrame {
    const char* step;
    Capture pixels;
    engine::FrameStats stats{};
};

// Renders the same scripted sequence (static frames, a caster moving, a
// light moving, the camera moving so the shadowed selection changes, every
// caster moving) with the shadow cache on or off. Runs one renderer at a
// time: each renderer's constant rings take ~600 of a device's 1024 buffer
// slots, so two cannot coexist on one device.
template <typename Reader>
[[nodiscard]] auto captureShadowSequence(gfx::IDevice& device, Reader& reader, bool caching)
    -> std::vector<SequenceFrame> {
    const NightSceneOptions options = shadowCacheOptions();
    NightTarget target(device, kEffectWidth, kEffectHeight);
    auto worldPointer = std::make_unique<NightWorld>(device, options);
    NightWorld& world = *worldPointer;
    world.renderer.pbrSettings().pointShadowCaching = caching;

    // The light closest to the camera's view of the scene centre is always
    // among the shadowed ones (the selection prefers bright, near lights);
    // then a caster near that light.
    std::size_t nearLight = 0;
    float nearest = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < world.lights.size(); ++i) {
        const math::Vec3 p = world.registry.get<engine::PointLight>(world.lights[i])->position;
        const float d = math::length(p - math::Vec3(0.0f, 7.0f, 11.0f));
        if (d < nearest) {
            nearest = d;
            nearLight = i;
        }
    }
    const math::Vec3 nearPosition =
        world.registry.get<engine::PointLight>(world.lights[nearLight])->position;
    std::size_t nearCaster = 0;
    nearest = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < world.casters.size(); ++i) {
        const math::Vec3 p =
            world.registry.get<ecs::WorldMatrix>(world.casters[i])->matrix.col[3].xyz();
        const float d = math::length(p - nearPosition);
        if (d < nearest) {
            nearest = d;
            nearCaster = i;
        }
    }

    std::vector<SequenceFrame> frames;
    const auto frame = [&](const char* step) {
        SequenceFrame result;
        result.step = step;
        result.pixels = target.render(world.renderer, world.camera, reader);
        result.stats = world.renderer.stats();
        frames.push_back(static_cast<SequenceFrame&&>(result));
    };
    frame("first frame");
    frame("static");
    world.registry.get<ecs::WorldMatrix>(world.casters[nearCaster])->matrix.col[3].x += 0.35f;
    frame("one caster moved");
    frame("static after caster move");
    world.registry.get<engine::PointLight>(world.lights[nearLight])->position.x += 0.25f;
    frame("one light moved");
    world.registry.get<engine::Camera>(world.camera)->eye.x += 4.0f;
    frame("camera moved (selection changes)");
    frame("static after camera move");
    for (const ecs::Entity caster : world.casters) {
        world.registry.get<ecs::WorldMatrix>(caster)->matrix.col[3].z -= 0.2f;
    }
    frame("every caster moved");
    worldPointer.reset();
    return frames;
}

template <typename Reader>
[[nodiscard]] auto captureShadowCacheSequence(gfx::IDevice& device, Reader&& reader)
    -> std::vector<CacheFrame> {
    const auto cached = captureShadowSequence(device, reader, true);
    const auto uncached = captureShadowSequence(device, reader, false);
    REQUIRE(cached.size() == uncached.size());
    std::vector<CacheFrame> frames;
    for (std::size_t i = 0; i < cached.size(); ++i) {
        CacheFrame frame;
        frame.step = cached[i].step;
        frame.cached = cached[i].pixels;
        frame.uncached = uncached[i].pixels;
        frame.cachedStats = cached[i].stats;
        frame.uncachedStats = uncached[i].stats;
        frames.push_back(static_cast<CacheFrame&&>(frame));
    }
    return frames;
}

void checkShadowCacheFrames(const char* backend, const std::vector<CacheFrame>& frames) {
    REQUIRE(frames.size() == 8);
    for (const CacheFrame& frame : frames) {
        const auto diff = engine::compareShaderImagesRgba8(frame.cached, frame.uncached, 0);
        MESSAGE(std::string(backend) << " " << std::string(frame.step) << ": shadowed="
                << frame.cachedStats.pointShadowLights
                << " redrawn cached/uncached=" << frame.cachedStats.pointShadowLightsRedrawn
                << "/" << frame.uncachedStats.pointShadowLightsRedrawn
                << " draws cached/uncached=" << frame.cachedStats.pointShadowDraws << "/"
                << frame.uncachedStats.pointShadowDraws
                << " mismatches=" << diff.mismatchPixelCount);
        CHECK(frame.cached == frame.uncached);
        CHECK(frame.cachedStats.pointShadowLights == 4);
        CHECK(frame.uncachedStats.pointShadowLightsRedrawn ==
              frame.uncachedStats.pointShadowLights);
    }
    const auto redrawn = [&](std::size_t index) {
        return frames[index].cachedStats.pointShadowLightsRedrawn;
    };
    CHECK(redrawn(0) == 4);
    CHECK(redrawn(1) == 0);
    CHECK(frames[1].cachedStats.pointShadowDraws == 0);
    CHECK(redrawn(2) > 0); // the moved caster's lights
    CHECK(redrawn(2) < 4); // ... and not the others: partial (tile reset) path
    CHECK(redrawn(3) == 0);
    CHECK(redrawn(4) > 0);
    CHECK(redrawn(4) < 4);
    CHECK(redrawn(5) > 0);
    CHECK(redrawn(6) == 0);
    CHECK(redrawn(7) == 4);
}

} // namespace

TEST_CASE("Point-light shadow cache: cached frames equal uncached frames on OpenGL, Vulkan, "
          "and DX12") {
    auto windowResult =
        platform::Window::create({"Point shadow cache", kEffectWidth, kEffectHeight, false});
    REQUIRE(windowResult.hasValue());
    platform::Window* window = windowResult.value();
    REQUIRE(window->makeCurrent());
    REQUIRE(platform::initGl(4, 3).hasValue());
    gfx::IDevice* openGlBase = gfx::createOpenGLDevice();
    REQUIRE(openGlBase != nullptr);
    const auto gl = captureShadowCacheSequence(
        *openGlBase, [](gfx::TextureHandle, std::span<std::byte> out) {
            glReadPixels(0, 0, static_cast<GLsizei>(kEffectWidth),
                         static_cast<GLsizei>(kEffectHeight), GL_RGBA, GL_UNSIGNED_BYTE,
                         out.data());
            return glGetError() == GL_NO_ERROR ? gfx::GfxError::None : gfx::GfxError::DeviceLost;
        });
    gfx::deleteDevice(openGlBase);
    delete window;
    checkShadowCacheFrames("OpenGL", gl);

    gfx::IDevice* vulkanBase = gfx::createVulkanDevice();
    REQUIRE(vulkanBase != nullptr);
    auto* vulkan = static_cast<gfx::VulkanDevice*>(vulkanBase);
    const auto vk = captureShadowCacheSequence(
        *vulkan, [vulkan](gfx::TextureHandle texture, std::span<std::byte> out) {
            return vulkan->readTexture(texture, 0, out);
        });
    CHECK(vulkan->validationWarningCount() == 0);
    CHECK(vulkan->validationErrorCount() == 0);
    gfx::deleteDevice(vulkanBase);
    checkShadowCacheFrames("Vulkan", vk);

    gfx::IDevice* dx12Base = gfx::createDx12Device();
    REQUIRE(dx12Base != nullptr);
    auto* dx12 = static_cast<gfx::Dx12Device*>(dx12Base);
    const auto dx = captureShadowCacheSequence(
        *dx12, [dx12](gfx::TextureHandle texture, std::span<std::byte> out) {
            return dx12->readTexture(texture, 0, out);
        });
    CHECK(dx12->validationWarningCount() == 0);
    CHECK(dx12->validationErrorCount() == 0);
    gfx::deleteDevice(dx12Base);
    checkShadowCacheFrames("DX12", dx);

    for (std::size_t i = 0; i < gl.size(); ++i) {
        checkEffectParity(std::string("shadow cache ") + gl[i].step + " GL/VK", gl[i].cached,
                          vk[i].cached);
        checkEffectParity(std::string("shadow cache ") + gl[i].step + " GL/DX", gl[i].cached,
                          dx[i].cached);
    }
    maybeWriteCapture("shadow_cache_caster_moved_opengl", gl[2].cached, kEffectWidth,
                      kEffectHeight);
    maybeWriteCapture("shadow_cache_camera_moved_opengl", gl[5].cached, kEffectWidth,
                      kEffectHeight);
}

namespace {

// CPU vs GPU light binning on one renderer: the same static frame rendered
// with gpuLightBinning off and on.
struct BinningFrame {
    const char* scene;
    Capture cpu;
    Capture gpu;
    std::vector<std::uint32_t> cpuWords;
    std::vector<std::uint32_t> gpuWords;
    engine::FrameStats cpuStats{};
    engine::FrameStats gpuStats{};
};

[[nodiscard]] auto readLightBuffer(gfx::IDevice& device, gfx::BufferHandle buffer)
    -> std::vector<std::uint32_t> {
    std::vector<std::uint32_t> header(engine::kPointLightHeaderWords);
    REQUIRE(device.readBuffer(buffer, 0, std::as_writable_bytes(std::span(header))) ==
            gfx::GfxError::None);
    std::size_t size = static_cast<std::size_t>(header[6]) + header[7];
    if (header[11] != 0) size = std::max<std::size_t>(size, header[10] + header[11] * 96u);
    std::vector<std::uint32_t> words(size);
    REQUIRE(device.readBuffer(buffer, 0, std::as_writable_bytes(std::span(words))) ==
            gfx::GfxError::None);
    return words;
}

template <typename Reader>
[[nodiscard]] auto captureBinningScenes(gfx::IDevice& device, Reader&& reader)
    -> std::vector<BinningFrame> {
    struct Scene {
        const char* name;
        NightSceneOptions options;
        std::uint32_t maxIndexCount;
    };
    NightSceneOptions dense;
    dense.lightsPerSide = 32; // 1024 lights
    dense.extent = 14.0f;
    dense.range = 2.5f;
    const Scene scenes[] = {
        {"1024 lights", dense, engine::ClusterGridConfig{}.maxIndexCount},
        {"1024 lights, index cap 20000", dense, 20000u},
        {"shadowed lights", shadowCacheOptions(), engine::ClusterGridConfig{}.maxIndexCount},
    };
    std::vector<BinningFrame> frames;
    for (const Scene& scene : scenes) {
        NightTarget target(device, kEffectWidth, kEffectHeight);
        auto world = std::make_unique<NightWorld>(device, scene.options);
        world->renderer.pbrSettings().pointLightClusters.maxIndexCount = scene.maxIndexCount;
        BinningFrame frame;
        frame.scene = scene.name;
        world->renderer.pbrSettings().gpuLightBinning = false;
        frame.cpu = target.render(world->renderer, world->camera, reader);
        frame.cpuStats = world->renderer.stats();
        const auto cpuWords = world->renderer.pointLightWords();
        frame.cpuWords.assign(cpuWords.begin(), cpuWords.end());
        world->renderer.pbrSettings().gpuLightBinning = true;
        frame.gpu = target.render(world->renderer, world->camera, reader);
        frame.gpuStats = world->renderer.stats();
        frame.gpuWords = readLightBuffer(device, world->renderer.pointLightBuffer());
        frames.push_back(static_cast<BinningFrame&&>(frame));
    }
    return frames;
}

void checkBinningFrames(const char* backend, const std::vector<BinningFrame>& frames) {
    for (const BinningFrame& frame : frames) {
        const auto diff = engine::compareShaderImagesRgba8(frame.cpu, frame.gpu, 0);
        const std::size_t bufferDifferences =
            compareLightBuffers(std::span<const std::uint32_t>(frame.cpuWords),
                                std::span<const std::uint32_t>(frame.gpuWords));
        MESSAGE(std::string(backend) << " " << std::string(frame.scene)
                << ": visible=" << frame.cpuStats.pointLightsVisible
                << " refs=" << frame.cpuStats.pointLightReferences
                << " dropped=" << frame.cpuStats.pointLightReferencesDropped
                << " dispatches cpu/gpu=" << frame.cpuStats.pointLightBinningDispatches << "/"
                << frame.gpuStats.pointLightBinningDispatches
                << " pixel mismatches=" << diff.mismatchPixelCount
                << " buffer differences=" << bufferDifferences);
        CHECK(frame.cpuStats.pointLightBinningDispatches == 0);
        CHECK(frame.gpuStats.pointLightBinningDispatches == 3);
        CHECK(frame.cpuStats.pointLightsVisible >= 16);
        CHECK(frame.gpuStats.pointLightsVisible == frame.cpuStats.pointLightsVisible);
        CHECK(frame.gpuStats.pointLightReferences == frame.cpuStats.pointLightReferences);
        CHECK(frame.gpuStats.pointLightReferencesDropped ==
              frame.cpuStats.pointLightReferencesDropped);
        CHECK(diff.mismatchPixelCount == 0);
        CHECK(bufferDifferences == 0);
    }
    CHECK(frames[1].cpuStats.pointLightReferencesDropped > 0);
    CHECK(frames[2].cpuStats.pointShadowLights > 0);
}

} // namespace

TEST_CASE("Point lights: GPU light binning matches CPU binning on OpenGL, Vulkan, and DX12") {
    auto windowResult =
        platform::Window::create({"GPU light binning", kEffectWidth, kEffectHeight, false});
    REQUIRE(windowResult.hasValue());
    platform::Window* window = windowResult.value();
    REQUIRE(window->makeCurrent());
    REQUIRE(platform::initGl(4, 3).hasValue());
    gfx::IDevice* openGlBase = gfx::createOpenGLDevice();
    REQUIRE(openGlBase != nullptr);
    const auto gl = captureBinningScenes(
        *openGlBase, [](gfx::TextureHandle, std::span<std::byte> out) {
            glReadPixels(0, 0, static_cast<GLsizei>(kEffectWidth),
                         static_cast<GLsizei>(kEffectHeight), GL_RGBA, GL_UNSIGNED_BYTE,
                         out.data());
            return glGetError() == GL_NO_ERROR ? gfx::GfxError::None : gfx::GfxError::DeviceLost;
        });
    gfx::deleteDevice(openGlBase);
    delete window;
    checkBinningFrames("OpenGL", gl);

    gfx::IDevice* vulkanBase = gfx::createVulkanDevice();
    REQUIRE(vulkanBase != nullptr);
    auto* vulkan = static_cast<gfx::VulkanDevice*>(vulkanBase);
    const auto vk = captureBinningScenes(
        *vulkan, [vulkan](gfx::TextureHandle texture, std::span<std::byte> out) {
            return vulkan->readTexture(texture, 0, out);
        });
    CHECK(vulkan->validationWarningCount() == 0);
    CHECK(vulkan->validationErrorCount() == 0);
    gfx::deleteDevice(vulkanBase);
    checkBinningFrames("Vulkan", vk);

    gfx::IDevice* dx12Base = gfx::createDx12Device();
    REQUIRE(dx12Base != nullptr);
    auto* dx12 = static_cast<gfx::Dx12Device*>(dx12Base);
    const auto dx = captureBinningScenes(
        *dx12, [dx12](gfx::TextureHandle texture, std::span<std::byte> out) {
            return dx12->readTexture(texture, 0, out);
        });
    CHECK(dx12->validationWarningCount() == 0);
    CHECK(dx12->validationErrorCount() == 0);
    gfx::deleteDevice(dx12Base);
    checkBinningFrames("DX12", dx);

    for (std::size_t i = 0; i < gl.size(); ++i) {
        checkEffectParity(std::string("GPU binning ") + gl[i].scene + " GL/VK", gl[i].gpu,
                          vk[i].gpu);
        checkEffectParity(std::string("GPU binning ") + gl[i].scene + " GL/DX", gl[i].gpu,
                          dx[i].gpu);
    }
}

TEST_CASE("Bloom and point-light shadows: WebGPU validation frames match the locked references") {
    auto windowResult = platform::Window::create(
        {"Effect references", kWebFrameExtent, kWebFrameExtent, false});
    REQUIRE(windowResult.hasValue());
    platform::Window* window = windowResult.value();
    REQUIRE(window->makeCurrent());
    REQUIRE(platform::initGl(4, 3).hasValue());
    gfx::IDevice* openGlBase = gfx::createOpenGLDevice();
    REQUIRE(openGlBase != nullptr);
    const auto gl = captureWebValidationFrames(
        *openGlBase, [](gfx::TextureHandle, std::span<std::byte> out) {
            glReadPixels(0, 0, static_cast<GLsizei>(kWebFrameExtent),
                         static_cast<GLsizei>(kWebFrameExtent), GL_RGBA, GL_UNSIGNED_BYTE,
                         out.data());
            return glGetError() == GL_NO_ERROR ? gfx::GfxError::None : gfx::GfxError::DeviceLost;
        });
    gfx::deleteDevice(openGlBase);
    delete window;

    gfx::IDevice* vulkanBase = gfx::createVulkanDevice();
    REQUIRE(vulkanBase != nullptr);
    auto* vulkan = static_cast<gfx::VulkanDevice*>(vulkanBase);
    const auto vk = captureWebValidationFrames(
        *vulkan, [vulkan](gfx::TextureHandle texture, std::span<std::byte> out) {
            return vulkan->readTexture(texture, 0, out);
        });
    CHECK(vulkan->validationErrorCount() == 0);
    gfx::deleteDevice(vulkanBase);

    gfx::IDevice* dx12Base = gfx::createDx12Device();
    REQUIRE(dx12Base != nullptr);
    auto* dx12 = static_cast<gfx::Dx12Device*>(dx12Base);
    const auto dx = captureWebValidationFrames(
        *dx12, [dx12](gfx::TextureHandle texture, std::span<std::byte> out) {
            return dx12->readTexture(texture, 0, out);
        });
    CHECK(dx12->validationErrorCount() == 0);
    gfx::deleteDevice(dx12Base);

    checkEffectParity("web frame point shadow GL/VK", gl.pointShadow, vk.pointShadow);
    checkEffectParity("web frame point shadow GL/DX", gl.pointShadow, dx.pointShadow);
    checkEffectParity("web frame bloom GL/VK", gl.bloom, vk.bloom);
    checkEffectParity("web frame bloom GL/DX", gl.bloom, dx.bloom);

    const std::string shadowPath = std::string(F4_EFFECT_FIXTURE_DIR) +
                                   "/renderer_point_shadow_reference.rgba";
    const std::string bloomPath = std::string(F4_EFFECT_FIXTURE_DIR) +
                                  "/renderer_bloom_reference.rgba";
    if (const char* update = std::getenv("F4_UPDATE_EFFECT_FIXTURES");
        update != nullptr && update[0] == '1') {
        // Vulkan is the reference writer; the other backends are held to it.
        writeFixture(shadowPath, vk.pointShadow);
        writeFixture(bloomPath, vk.bloom);
        MESSAGE("wrote " << shadowPath << " and " << bloomPath);
        return;
    }
    const Capture shadowReference = readFixture(shadowPath);
    const Capture bloomReference = readFixture(bloomPath);
    REQUIRE(!shadowReference.empty());
    REQUIRE(!bloomReference.empty());
    checkEffectParity("web frame point shadow reference/GL", shadowReference, gl.pointShadow);
    checkEffectParity("web frame point shadow reference/VK", shadowReference, vk.pointShadow);
    checkEffectParity("web frame point shadow reference/DX", shadowReference, dx.pointShadow);
    checkEffectParity("web frame bloom reference/GL", bloomReference, gl.bloom);
    checkEffectParity("web frame bloom reference/VK", bloomReference, vk.bloom);
    checkEffectParity("web frame bloom reference/DX", bloomReference, dx.bloom);
}

TEST_CASE("Point lights: night scene frame-time benchmark (set F4_POINT_LIGHT_BENCH=1)") {
    const char* enabled = std::getenv("F4_POINT_LIGHT_BENCH");
    if (enabled == nullptr) return;
    // "1": every row once. "cache": shadow-cache rows only, cached and
    // uncached interleaved over several rounds, reporting the median.
    const std::string mode = enabled;
    // "binning": CPU vs GPU light binning at 1024/4096 lights, interleaved.
    if (mode != "1" && mode != "cache" && mode != "binning") return;
    bool verbose = true;
    bool benchGpuBinning = true;
    double lastCpuMs = 0.0; // CPU time inside render() per frame, last bench() call
    constexpr std::uint32_t width = 1280;
    constexpr std::uint32_t height = 720;
    constexpr int warmup = 30;
    constexpr int frames = 240;

    // "stress": fixed 2.5 m range, so denser grids pile hundreds of lights into
    // each cluster. "scaled": range shrinks with the grid spacing (2x spacing),
    // the way a level designer would place many small lights.
    // `bloom` enables the HDR bloom chain; `shadows` < 0 means the plain night
    // scene, 0 adds 32x32 shadow-casting pillars (one beside each lamp of the
    // 32x32 grid), N > 0 also shadows the N best-ranked lights.
    const auto bench = [&](gfx::IDevice& device, const char* backend,
                           std::uint32_t lightsPerSide, bool scaledRange, bool bloom = false,
                           int shadows = -1, bool shadowCache = true) -> double {
        double ms = 0.0;
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
            options.bloom = bloom;
            options.bloomIntensity = 0.8f;
            if (shadows >= 0) options.pillarsPerSide = 32;
            if (shadows > 0) {
                options.castShadows = true;
                options.shadowBudget = static_cast<std::uint32_t>(shadows);
            }
            const ecs::Entity cameraEntity = buildNightScene(renderer, registry, options);
            renderer.pbrSettings().pointShadowCaching = shadowCache;
            renderer.pbrSettings().gpuLightBinning = benchGpuBinning;
            const float aspect = static_cast<float>(width) / static_cast<float>(height);
            const gfx::Extent extent{width, height};
            for (int i = 0; i < warmup; ++i) {
                REQUIRE(renderer.render(framebuffer.value(), cameraEntity, aspect, extent) ==
                        gfx::GfxError::None);
            }
            device.waitIdle();
            const auto start = std::chrono::steady_clock::now();
            double cpuMs = 0.0;
            for (int i = 0; i < frames; ++i) {
                const auto frameStart = std::chrono::steady_clock::now();
                const gfx::GfxError error =
                    renderer.render(framebuffer.value(), cameraEntity, aspect, extent);
                cpuMs += std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - frameStart)
                             .count();
                REQUIRE(error == gfx::GfxError::None);
            }
            device.waitIdle();
            const auto end = std::chrono::steady_clock::now();
            ms = std::chrono::duration<double, std::milli>(end - start).count() / frames;
            lastCpuMs = cpuMs / frames;
            const engine::FrameStats stats = renderer.stats();
            char variant[64];
            std::snprintf(variant, sizeof(variant), "%s%s%s%s",
                          scaledRange ? "scaled" : "stress", bloom ? "+bloom" : "",
                          shadows < 0 ? "" : shadows == 0 ? "+pillars" : "+pillars+shadows",
                          shadows > 0 && !shadowCache ? "(uncached)" : "");
            if (verbose) std::printf("[point-light bench] %s %s %ux%u lights=%u range=%.2f visible=%u "
                        "refs=%u max/cluster=%u draws=%u shadowed=%u shadowDraws=%u "
                        "redrawn=%u bloomPasses=%u binning=%s: %.3f ms/frame "
                        "(render() CPU %.3f ms)\n",
                        backend, variant, width, height,
                        stats.pointLights, static_cast<double>(options.range),
                        stats.pointLightsVisible,
                        stats.pointLightReferences, stats.maxPointLightsPerCluster,
                        stats.draws, stats.pointShadowLights, stats.pointShadowDraws,
                        stats.pointShadowLightsRedrawn, stats.bloomPasses,
                        stats.pointLightBinningDispatches != 0 ? "gpu" : "cpu", ms, lastCpuMs);
        }
        CHECK(device.destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
        CHECK(device.destroyTexture(depth.value()) == gfx::GfxError::None);
        CHECK(device.destroyTexture(color.value()) == gfx::GfxError::None);
        return ms;
    };
    // CPU vs GPU binning on the plain night scene, 1024 and 4096 lights, both
    // range models; interleaved rounds, median frame and render() CPU time.
    const auto binningRows = [&](gfx::IDevice& device, const char* backend) {
        constexpr int rounds = 5;
        verbose = false;
        for (const bool scaled : {false, true}) {
            for (const std::uint32_t side : {32u, 64u}) {
                std::vector<double> frame[2], cpu[2];
                for (int round = 0; round < rounds; ++round) {
                    for (int order = 0; order < 2; ++order) {
                        const int gpu = (round + order) % 2;
                        benchGpuBinning = gpu == 1;
                        frame[gpu].push_back(bench(device, backend, side, scaled));
                        cpu[gpu].push_back(lastCpuMs);
                    }
                }
                for (auto* values : {&frame[0], &frame[1], &cpu[0], &cpu[1]}) {
                    std::sort(values->begin(), values->end());
                }
                std::printf("[point-light bench] %s binning %s lights=%u: CPU binning %.3f "
                            "ms/frame (render() CPU %.3f), GPU binning %.3f ms/frame "
                            "(render() CPU %.3f) (median of %d)\n",
                            backend, scaled ? "scaled" : "stress", side * side,
                            frame[0][rounds / 2], cpu[0][rounds / 2], frame[1][rounds / 2],
                            cpu[1][rounds / 2], rounds);
            }
        }
        benchGpuBinning = true;
        verbose = true;
    };
    // Static night scene with 32x32 pillars: cached vs uncached point shadows,
    // interleaved so clock drift hits both alike; median of the rounds.
    const auto cacheRows = [&](gfx::IDevice& device, const char* backend) {
        constexpr int rounds = 7;
        verbose = false;
        for (int shadows : {4, 8, 16}) {
            std::vector<double> cached, uncached;
            for (int round = 0; round < rounds; ++round) {
                // Alternate which variant runs first so ordering bias cancels.
                if (round % 2 == 0) {
                    cached.push_back(bench(device, backend, 32u, true, false, shadows, true));
                    uncached.push_back(bench(device, backend, 32u, true, false, shadows, false));
                } else {
                    uncached.push_back(bench(device, backend, 32u, true, false, shadows, false));
                    cached.push_back(bench(device, backend, 32u, true, false, shadows, true));
                }
            }
            std::sort(cached.begin(), cached.end());
            std::sort(uncached.begin(), uncached.end());
            std::printf("[point-light bench] %s shadow cache lights=1024 pillars=1024 "
                        "shadowed=%d: cached %.3f ms/frame, uncached %.3f ms/frame "
                        "(median of %d; min %.3f / %.3f)\n",
                        backend, shadows, cached[rounds / 2], uncached[rounds / 2], rounds,
                        cached.front(), uncached.front());
        }
        verbose = true;
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
        if (mode == "cache") {
            cacheRows(*device, "OpenGL");
        } else if (mode == "binning") {
            binningRows(*device, "OpenGL");
        } else {
            for (std::uint32_t side : {0u, 16u, 32u, 64u}) bench(*device, "OpenGL", side, false);
            for (std::uint32_t side : {32u, 64u}) bench(*device, "OpenGL", side, true);
            bench(*device, "OpenGL", 32u, true, true);
            for (int shadows : {0, 4, 8, 16}) bench(*device, "OpenGL", 32u, true, false, shadows);
            for (int shadows : {4, 8, 16})
                bench(*device, "OpenGL", 32u, true, false, shadows, false);
            bench(*device, "OpenGL", 32u, true, true, 8);
        }
        gfx::deleteDevice(device);
        delete window;
    }
    {
        gfx::IDevice* device = gfx::createVulkanDevice();
        REQUIRE(device != nullptr);
        if (mode == "cache") {
            cacheRows(*device, "Vulkan");
        } else if (mode == "binning") {
            binningRows(*device, "Vulkan");
        } else {
            for (std::uint32_t side : {0u, 16u, 32u, 64u}) bench(*device, "Vulkan", side, false);
            for (std::uint32_t side : {32u, 64u}) bench(*device, "Vulkan", side, true);
            bench(*device, "Vulkan", 32u, true, true);
            for (int shadows : {0, 4, 8, 16}) bench(*device, "Vulkan", 32u, true, false, shadows);
            for (int shadows : {4, 8, 16})
                bench(*device, "Vulkan", 32u, true, false, shadows, false);
            bench(*device, "Vulkan", 32u, true, true, 8);
        }
        gfx::deleteDevice(device);
    }
    {
        gfx::IDevice* device = gfx::createDx12Device();
        REQUIRE(device != nullptr);
        std::printf("[point-light bench] DX12 adapter: %s\n",
                    static_cast<gfx::Dx12Device*>(device)->deviceName());
        if (mode == "cache") {
            cacheRows(*device, "DX12");
        } else if (mode == "binning") {
            binningRows(*device, "DX12");
        } else {
            for (std::uint32_t side : {0u, 16u, 32u, 64u}) bench(*device, "DX12", side, false);
            for (std::uint32_t side : {32u, 64u}) bench(*device, "DX12", side, true);
            bench(*device, "DX12", 32u, true, true);
            for (int shadows : {0, 4, 8, 16}) bench(*device, "DX12", 32u, true, false, shadows);
            for (int shadows : {4, 8, 16})
                bench(*device, "DX12", 32u, true, false, shadows, false);
            bench(*device, "DX12", 32u, true, true, 8);
        }
        gfx::deleteDevice(device);
    }
}
#endif
