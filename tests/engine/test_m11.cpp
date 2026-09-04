#include "doctest/doctest.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#ifdef F4_GRAPHICS_HAS_METAL
#include <fstream>
#include <string>
#include <vector>

#include <zlib.h>
#endif

#include "backends/null/null_device.h"
#include "core/ecs/transform.h"
#include "core/math/vec2.h"
#include "core/math/vec3.h"
#include "core/resources/mesh.h"
#include "core/resources/texture.h"
#include "engine/cameras/camera.h"
#include "engine/lighting/light.h"
#include "engine/rendering/material.h"
#include "engine/rendering/mesh_renderer.h"
#include "engine/rendering/pbr.h"
#include "engine/rendering/renderer.h"
#include "engine/rendering/shader_parity.h"
#include "graphics/framebuffer/framebuffer_desc.h"

#ifdef F4_GRAPHICS_HAS_OPENGL
#include <epoxy/gl.h>
#include "backends/opengl/opengl_device.h"
#include "platform/gl_context.h"
#include "platform/window.h"
#endif

#ifdef F4_GRAPHICS_HAS_VULKAN
#include "backends/vulkan/vulkan_device.h"
#endif

#ifdef F4_GRAPHICS_HAS_DX12
#include "backends/dx12/dx12_device.h"
#endif

#ifdef F4_GRAPHICS_HAS_METAL
#include "backends/metal/metal_device.h"
#endif

#if defined(F4_GRAPHICS_HAS_OPENGL) || defined(F4_GRAPHICS_HAS_VULKAN) || \
    defined(F4_GRAPHICS_HAS_DX12) || defined(F4_GRAPHICS_HAS_METAL)
#define F4_M11_GPU_CAPTURE 1
#endif

using namespace f4;

namespace {

constexpr std::uint32_t kCaptureExtent = 64;
using PbrCapture = std::array<std::uint8_t,
                              kCaptureExtent * kCaptureExtent * 4>;

#if defined(F4_M11_GPU_CAPTURE)
[[nodiscard]] auto centerPixel(const PbrCapture& capture)
    -> std::array<std::uint8_t, 4> {
    const std::size_t offset =
        (kCaptureExtent / 2 * kCaptureExtent + kCaptureExtent / 2) * 4u;
    return {capture[offset], capture[offset + 1], capture[offset + 2],
            capture[offset + 3]};
}

void flipCaptureRows(PbrCapture& capture) {
    constexpr std::size_t rowBytes = kCaptureExtent * 4u;
    for (std::size_t y = 0; y < kCaptureExtent / 2; ++y) {
        const std::size_t top = y * rowBytes;
        const std::size_t bottom = (kCaptureExtent - y - 1) * rowBytes;
        for (std::size_t x = 0; x < rowBytes; ++x) {
            const std::uint8_t value = capture[top + x];
            capture[top + x] = capture[bottom + x];
            capture[bottom + x] = value;
        }
    }
}

#ifdef F4_GRAPHICS_HAS_METAL
[[nodiscard]] auto decodeBase64(const std::string& encoded) -> std::vector<std::byte> {
    const auto decode = [](char character) -> int {
        if (character >= 'A' && character <= 'Z') return character - 'A';
        if (character >= 'a' && character <= 'z') return character - 'a' + 26;
        if (character >= '0' && character <= '9') return character - '0' + 52;
        if (character == '+') return 62;
        if (character == '/') return 63;
        return -1;
    };
    std::vector<std::byte> bytes;
    std::uint32_t value = 0;
    int availableBits = -8;
    for (const char character : encoded) {
        if (character == '=') break;
        if (character == ' ' || character == '\t' || character == '\r' ||
            character == '\n') {
            continue;
        }
        const int digit = decode(character);
        if (digit < 0) return {};
        value = (value << 6u) | static_cast<std::uint32_t>(digit);
        availableBits += 6;
        if (availableBits >= 0) {
            bytes.push_back(static_cast<std::byte>((value >> availableBits) & 0xffu));
            availableBits -= 8;
        }
    }
    return bytes;
}

[[nodiscard]] auto loadM11Reference() -> PbrCapture {
    std::ifstream stream(F4_M11_REFERENCE_DEFLATE_BASE64,
                         std::ios::binary | std::ios::ate);
    REQUIRE(stream.good());
    const std::streamsize size = stream.tellg();
    REQUIRE(size > 0);
    stream.seekg(0);
    std::string encoded(static_cast<std::size_t>(size), '\0');
    stream.read(encoded.data(), size);
    REQUIRE(stream.good());
    const std::vector<std::byte> compressed = decodeBase64(encoded);
    REQUIRE_FALSE(compressed.empty());

    PbrCapture reference{};
    uLongf outputSize = reference.size();
    const int result = uncompress(
        reference.data(), &outputSize,
        reinterpret_cast<const Bytef*>(compressed.data()), compressed.size());
    REQUIRE(result == Z_OK);
    REQUIRE(outputSize == reference.size());
    return reference;
}
#endif

[[nodiscard]] auto litPixelCount(const PbrCapture& capture) -> std::size_t {
    std::size_t count = 0;
    for (std::size_t offset = 0; offset < capture.size(); offset += 4) {
        if (capture[offset] > 20 || capture[offset + 1] > 20 ||
            capture[offset + 2] > 20) {
            ++count;
        }
    }
    return count;
}
#endif

[[nodiscard]] auto makeM11Triangle() -> resources::Mesh {
    struct Vertex {
        float px, py, pz;
        float nx, ny, nz;
        float u, v;
        std::uint8_t r, g, b, a;
    };
    const Vertex vertices[3] = {
        {-0.6f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 255, 255, 255, 255},
        { 0.6f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 255, 255, 255, 255},
        { 0.0f,  0.6f, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f, 255, 255, 255, 255},
    };
    const std::uint16_t indices[3] = {0, 1, 2};
    resources::Mesh mesh;
    resources::VertexStream stream;
    stream.strideBytes = sizeof(Vertex);
    stream.data.resize(sizeof(vertices));
    std::memcpy(stream.data.data(), vertices, sizeof(vertices));
    mesh.streams.push(static_cast<resources::VertexStream&&>(stream));
    mesh.attributes.push({resources::VertexSemantic::Position,
                          resources::VertexFormat::F32_3, 0, 0});
    mesh.attributes.push({resources::VertexSemantic::Normal,
                          resources::VertexFormat::F32_3, 0, 12});
    mesh.attributes.push({resources::VertexSemantic::TexCoord0,
                          resources::VertexFormat::F32_2, 0, 24});
    mesh.attributes.push({resources::VertexSemantic::Color0,
                          resources::VertexFormat::UNORM8_4, 0, 32});
    mesh.indices.resize(sizeof(indices));
    std::memcpy(mesh.indices.data(), indices, sizeof(indices));
    mesh.submeshes.push({0, 3});
    return mesh;
}

[[nodiscard]] auto makePositionOnlyTriangle() -> resources::Mesh {
    const float vertices[] = {
        -0.6f, -0.5f, 0.0f,
         0.6f, -0.5f, 0.0f,
         0.0f,  0.6f, 0.0f,
    };
    const std::uint16_t indices[3] = {0, 1, 2};
    resources::Mesh mesh;
    resources::VertexStream stream;
    stream.strideBytes = 12;
    stream.data.resize(sizeof(vertices));
    std::memcpy(stream.data.data(), vertices, sizeof(vertices));
    mesh.streams.push(static_cast<resources::VertexStream&&>(stream));
    mesh.attributes.push({resources::VertexSemantic::Position,
                          resources::VertexFormat::F32_3, 0, 0});
    mesh.indices.resize(sizeof(indices));
    std::memcpy(mesh.indices.data(), indices, sizeof(indices));
    mesh.submeshes.push({0, 3});
    return mesh;
}

[[nodiscard]] auto makeRgbaTexture(std::uint32_t width, std::uint32_t height,
                                   std::uint8_t red, std::uint8_t green,
                                   std::uint8_t blue, std::uint8_t alpha)
    -> resources::Texture {
    resources::Texture texture;
    texture.format = resources::TextureFormat::RGBA8_UNORM;
    resources::TextureMip mip;
    mip.width = width;
    mip.height = height;
    mip.bytes.resize(static_cast<std::size_t>(width) * height * 4u);
    for (std::size_t i = 0; i < mip.bytes.size(); i += 4) {
        mip.bytes[i + 0] = static_cast<std::byte>(red);
        mip.bytes[i + 1] = static_cast<std::byte>(green);
        mip.bytes[i + 2] = static_cast<std::byte>(blue);
        mip.bytes[i + 3] = static_cast<std::byte>(alpha);
    }
    texture.mips.push(static_cast<resources::TextureMip&&>(mip));
    return texture;
}

#if defined(F4_M11_GPU_CAPTURE)
template <typename Reader>
[[nodiscard]] auto capturePbrFrame(gfx::IDevice& device, bool shadows, Reader&& reader,
                                   bool positionOnly = false, bool batchedPair = false)
    -> PbrCapture {
    constexpr std::uint32_t extent = kCaptureExtent;
    gfx::TextureDesc colorDesc;
    colorDesc.width = extent;
    colorDesc.height = extent;
    colorDesc.hasOptimizedClear = true;
    colorDesc.optimizedClear.color[0] = 0.01f;
    colorDesc.optimizedClear.color[1] = 0.015f;
    colorDesc.optimizedClear.color[2] = 0.02f;
    colorDesc.optimizedClear.color[3] = 1.0f;
    auto color = device.createTexture(colorDesc);
    REQUIRE(color.hasValue());
    gfx::TextureDesc depthDesc;
    depthDesc.width = extent;
    depthDesc.height = extent;
    depthDesc.format = gfx::TextureFormat::Depth32F;
    depthDesc.hasOptimizedClear = true;
    depthDesc.optimizedClear.depth = 1.0f;
    auto depth = device.createTexture(depthDesc);
    REQUIRE(depth.hasValue());
    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {extent, extent};
    gfx::AttachmentDesc colorAttachment;
    colorAttachment.texture = color.value();
    colorAttachment.clearOnLoad = true;
    colorAttachment.clear.color[0] = 0.01f;
    colorAttachment.clear.color[1] = 0.015f;
    colorAttachment.clear.color[2] = 0.02f;
    colorAttachment.clear.color[3] = 1.0f;
    framebufferDesc.attachments.push(colorAttachment);
    gfx::AttachmentDesc depthAttachment;
    depthAttachment.type = gfx::AttachmentType::Depth;
    depthAttachment.texture = depth.value();
    depthAttachment.clearOnLoad = true;
    framebufferDesc.attachments.push(depthAttachment);
    auto framebuffer = device.createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());

    PbrCapture pixels{};
    {
        ecs::Registry registry(16);
        engine::Renderer renderer(registry, device);
        renderer.pbrSettings().shadowsEnabled = shadows;
        renderer.pbrSettings().ambientIntensity = 0.2f;
        REQUIRE(renderer.init() == gfx::GfxError::None);
        const auto baseTexture = renderer.createTexture(
            makeRgbaTexture(2, 2, 205, 96, 35, 255));
        const auto normalTexture = renderer.createTexture(
            makeRgbaTexture(2, 2, 145, 178, 238, 255));
        const auto mrTexture = renderer.createTexture(
            makeRgbaTexture(2, 2, 0, 140, 76, 255));
        REQUIRE(baseTexture.isValid());
        REQUIRE(normalTexture.isValid());
        REQUIRE(mrTexture.isValid());
        engine::StandardMaterialDesc materialDesc;
        materialDesc.albedo = baseTexture;
        materialDesc.normal = normalTexture;
        materialDesc.metallicRoughness = mrTexture;
        materialDesc.metallicFactor = 0.8f;
        materialDesc.roughnessFactor = 0.85f;
        const auto material = renderer.createMaterial(materialDesc);
        const auto mesh = renderer.createMesh(
            positionOnly ? makePositionOnlyTriangle() : makeM11Triangle());
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
        const std::uint32_t objectCount = batchedPair ? 2u : 1u;
        for (std::uint32_t index = 0; index < objectCount; ++index) {
            const ecs::Entity object = registry.create();
            auto& transform = registry.emplace<ecs::LocalTransform>(
                object, ecs::LocalTransform{});
            if (batchedPair) {
                transform.position.x = index == 0 ? -0.48f : 0.48f;
                transform.scale = {0.68f, 0.68f, 0.68f};
            }
            registry.emplace<engine::MeshRenderer>(
                object, engine::MeshRenderer{mesh, material, 0, shadows, false});
        }
        ecs::updateWorldTransforms(registry);
        REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f) ==
                gfx::GfxError::None);
        if (batchedPair) {
            CHECK(renderer.stats().draws == 1);
            CHECK(renderer.stats().instances == 2);
        }
        device.waitIdle();

        REQUIRE(reader(color.value(), std::as_writable_bytes(std::span(pixels))) ==
                gfx::GfxError::None);
        if (device.backend() == gfx::Backend::OpenGL) flipCaptureRows(pixels);
    }
    CHECK(device.destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
    CHECK(device.destroyTexture(depth.value()) == gfx::GfxError::None);
    CHECK(device.destroyTexture(color.value()) == gfx::GfxError::None);
    return pixels;
}
#endif

#if defined(F4_GRAPHICS_HAS_METAL)
TEST_CASE("Metal hardware: M11 PBR, fallback channels, and instancing are stable") {
    gfx::IDevice* base = gfx::createMetalDevice();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::MetalDevice*>(base);
    const auto first = capturePbrFrame(
        *device, false, [device](gfx::TextureHandle texture, std::span<std::byte> output) {
            return device->readTexture(texture, 0, output);
        });
    const auto repeated = capturePbrFrame(
        *device, false, [device](gfx::TextureHandle texture, std::span<std::byte> output) {
            return device->readTexture(texture, 0, output);
        });
    const auto positionOnly = capturePbrFrame(
        *device, false, [device](gfx::TextureHandle texture, std::span<std::byte> output) {
            return device->readTexture(texture, 0, output);
        }, true);
    const auto batched = capturePbrFrame(
        *device, false, [device](gfx::TextureHandle texture, std::span<std::byte> output) {
            return device->readTexture(texture, 0, output);
        }, false, true);
    const auto center = centerPixel(first);
    const auto compactCenter = centerPixel(positionOnly);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        CHECK(center[channel] > 20);
        CHECK(compactCenter[channel] > 20);
    }
    CHECK(litPixelCount(batched) > 100);
    CHECK(engine::compareShaderImagesRgba8(first, repeated, 0).matches());
    const auto reference = loadM11Reference();
    const auto parity = engine::compareShaderImagesRgba8(reference, first, 2);
    INFO("Metal/native M11 mismatches=" << parity.mismatchPixelCount
                                         << " max delta="
                                         << static_cast<int>(parity.maxChannelDelta));
    CHECK(parity.matches());
    device->waitIdle();
    CHECK(device->validationWarningCount() == 0);
    CHECK(device->validationErrorCount() == 0);
    gfx::deleteDevice(device);
}

TEST_CASE("Metal hardware: M11 filtered shadow pass executes cleanly") {
    gfx::IDevice* base = gfx::createMetalDevice();
    REQUIRE(base != nullptr);
    auto* device = static_cast<gfx::MetalDevice*>(base);
    const auto pixels = capturePbrFrame(
        *device, true, [device](gfx::TextureHandle texture, std::span<std::byte> output) {
            return device->readTexture(texture, 0, output);
        });
    const auto center = centerPixel(pixels);
    for (std::size_t channel = 0; channel < 3; ++channel) CHECK(center[channel] > 20);
    device->waitIdle();
    CHECK(device->validationWarningCount() == 0);
    CHECK(device->validationErrorCount() == 0);
    gfx::deleteDevice(device);
}
#endif

} // namespace

TEST_CASE("M11: sRGB transfer functions round-trip representative colors") {
    const std::array<math::Vec3, 4> colors{{
        {0.0f, 0.0f, 0.0f},
        {0.04045f, 0.25f, 0.5f},
        {0.8f, 0.9f, 1.0f},
        {1.0f, 0.125f, 0.003f},
    }};
    for (const math::Vec3 color : colors) {
        const math::Vec3 restored = engine::linearToSrgb(engine::srgbToLinear(color));
        CHECK(restored.x == doctest::Approx(color.x).epsilon(2.0e-5));
        CHECK(restored.y == doctest::Approx(color.y).epsilon(2.0e-5));
        CHECK(restored.z == doctest::Approx(color.z).epsilon(2.0e-5));
    }
}

TEST_CASE("M11: GGX metallic-roughness reference is finite and energy-partitioned") {
    const math::Vec3 f0(0.04f);
    CHECK(engine::fresnelSchlick(1.0f, f0) == f0);
    CHECK(engine::distributionGgx(1.0f, 0.1f) >
          engine::distributionGgx(1.0f, 0.8f));

    engine::PbrDirectInput dielectric;
    dielectric.baseColor = {0.8f, 0.2f, 0.1f};
    dielectric.roughness = 0.5f;
    const auto dielectricResult = engine::evaluatePbrDirect(dielectric);
    CHECK(dielectricResult.diffuse.x > 0.0f);
    CHECK(dielectricResult.specular.x > 0.0f);
    CHECK(std::isfinite(dielectricResult.total.x));

    engine::PbrDirectInput metal = dielectric;
    metal.metallic = 1.0f;
    const auto metalResult = engine::evaluatePbrDirect(metal);
    CHECK(metalResult.diffuse.x == doctest::Approx(0.0f));
    CHECK(metalResult.diffuse.y == doctest::Approx(0.0f));
    CHECK(metalResult.diffuse.z == doctest::Approx(0.0f));

    dielectric.lightDirection = {0.0f, 0.0f, -1.0f};
    const auto backFacing = engine::evaluatePbrDirect(dielectric);
    CHECK(backFacing.total.x == doctest::Approx(0.0f));
    CHECK(backFacing.total.y == doctest::Approx(0.0f));
    CHECK(backFacing.total.z == doctest::Approx(0.0f));
}

TEST_CASE("M11: environment mapping, PCF, and ACES policies are locked") {
    const math::Vec2 forward = engine::directionToEquirectUv({0.0f, 0.0f, 1.0f});
    CHECK(forward.x == doctest::Approx(0.5f));
    CHECK(forward.y == doctest::Approx(0.5f));
    const math::Vec2 right = engine::directionToEquirectUv({1.0f, 0.0f, 0.0f});
    CHECK(right.x == doctest::Approx(0.75f));
    CHECK(engine::directionToEquirectUv({0.0f, 1.0f, 0.0f}).y ==
          doctest::Approx(0.0f));

    const std::array<float, 9> depths{{
        0.2f, 0.8f, 0.2f,
        0.8f, 0.8f, 0.8f,
        0.2f, 0.8f, 0.2f,
    }};
    CHECK(engine::shadowPcf(depths, 3, 3, {0.5f, 0.5f}, 0.5f, 0.0f) ==
          doctest::Approx(5.0f / 9.0f));
    CHECK(engine::shadowPcf(depths, 3, 3, {-0.1f, 0.5f}, 0.5f, 0.0f) == 1.0f);
    CHECK(engine::shadowPcf({}, 0, 0, {0.5f, 0.5f}, 0.5f, 0.0f) == 1.0f);

    const math::Vec3 low = engine::acesToneMap({0.1f, 0.1f, 0.1f});
    const math::Vec3 high = engine::acesToneMap({4.0f, 4.0f, 4.0f});
    CHECK(low.x >= 0.0f);
    CHECK(high.x > low.x);
    CHECK(high.x <= 1.0f);
}

TEST_CASE("M11: material identity covers every PBR factor and texture") {
    engine::MaterialCache cache;
    engine::StandardMaterialDesc base;
    const auto first = cache.getOrCreate(base);
    CHECK(cache.getOrCreate(base) == first);

    engine::StandardMaterialDesc changed = base;
    changed.roughnessFactor = 0.25f;
    CHECK(cache.getOrCreate(changed) != first);
    changed = base;
    changed.normalYSign = -1.0f;
    CHECK(cache.getOrCreate(changed) != first);
    changed = base;
    changed.metallicRoughness = resources::TextureHandle{7, 3};
    CHECK(cache.getOrCreate(changed) != first);
    changed = base;
    changed.alphaMode = engine::MaterialAlphaMode::Mask;
    CHECK(cache.getOrCreate(changed) != first);
}

TEST_CASE("M11: glTF materials map explicitly into renderer materials") {
    resources::GltfMaterial source;
    source.baseColorFactor = {0.8f, 0.4f, 0.2f, 0.75f};
    source.emissiveFactor = {0.1f, 0.2f, 0.3f};
    source.metallicFactor = 0.65f;
    source.roughnessFactor = 0.35f;
    source.normalTexture = {1, 0, 0.6f};
    source.metallicRoughnessTexture.textureIndex = 2;
    source.occlusionTexture = {3, 0, 0.45f};
    source.emissiveTexture.textureIndex = 4;
    source.baseColorTexture.textureIndex = 0;
    source.alphaMode = resources::GltfAlphaMode::Blend;
    source.alphaCutoff = 0.25f;
    source.doubleSided = true;
    source.unlit = true;
    const std::array<resources::TextureHandle, 5> textures{{
        {4, 2}, {7, 3}, {8, 1}, {9, 2}, {10, 4},
    }};

    const auto converted = engine::materialFromGltf(source, textures);
    REQUIRE(converted.hasValue());
    CHECK(converted.value().tint == source.baseColorFactor);
    CHECK(converted.value().emissiveFactor == source.emissiveFactor);
    CHECK(converted.value().metallicFactor == source.metallicFactor);
    CHECK(converted.value().roughnessFactor == source.roughnessFactor);
    CHECK(converted.value().normalScale == source.normalTexture.scale);
    CHECK(converted.value().occlusionStrength == source.occlusionTexture.scale);
    CHECK(converted.value().albedo == textures[0]);
    CHECK(converted.value().normal == textures[1]);
    CHECK(converted.value().metallicRoughness == textures[2]);
    CHECK(converted.value().occlusion == textures[3]);
    CHECK(converted.value().emissive == textures[4]);
    CHECK(converted.value().alphaMode == engine::MaterialAlphaMode::Blend);
    CHECK(converted.value().alphaCutoff == source.alphaCutoff);
    CHECK(converted.value().doubleSided);
    CHECK(converted.value().unlit);

    gfx::NullDevice device;
    ecs::Registry registry(4);
    engine::Renderer renderer(registry, device);
    const auto material = renderer.createMaterialFromGltf(source, textures);
    REQUIRE(material.hasValue());
    CHECK(renderer.materialCache().get(material.value())->desc == converted.value());

    source.baseColorTexture.texCoord = 1;
    const auto unsupportedUv = engine::materialFromGltf(source, textures);
    REQUIRE(unsupportedUv.hasError());
    CHECK(unsupportedUv.error() == engine::MaterialImportError::UnsupportedTexCoord);
    source.baseColorTexture.texCoord = 0;
    source.baseColorTexture.textureIndex = 5;
    const auto missingTexture = engine::materialFromGltf(source, textures);
    REQUIRE(missingTexture.hasError());
    CHECK(missingTexture.error() == engine::MaterialImportError::TextureIndexOutOfRange);

    source.baseColorTexture.textureIndex = 0;
    auto invalidTextures = textures;
    invalidTextures[0] = {};
    const auto invalidTexture = engine::materialFromGltf(source, invalidTextures);
    REQUIRE(invalidTexture.hasError());
    CHECK(invalidTexture.error() == engine::MaterialImportError::InvalidTextureHandle);
}

TEST_CASE("M11: material pipelines are specialized for the real mesh layout") {
    gfx::NullDevice device;
    engine::MaterialCache cache;
    const auto material = cache.getOrCreate({});
    gfx::VertexInputLayout compact;
    compact.bindings.push({0, 12, 0});
    compact.attributes.push({0, 0, gfx::VertexFormat::Float32x3, 0});
    gfx::VertexInputLayout full;
    full.bindings.push({0, 36, 0});
    full.attributes.push({0, 0, gfx::VertexFormat::Float32x3, 0});
    full.attributes.push({0, 1, gfx::VertexFormat::Float32x3, 12});
    full.attributes.push({0, 2, gfx::VertexFormat::Float32x2, 24});
    full.attributes.push({0, 3, gfx::VertexFormat::UNorm8x4, 32});

    REQUIRE(cache.ensurePipeline(device, material, compact) == gfx::GfxError::None);
    REQUIRE(cache.ensurePipeline(device, material, full) == gfx::GfxError::None);
    const auto compactPipeline = cache.pipelineFor(material, compact);
    const auto fullPipeline = cache.pipelineFor(material, full);
    CHECK(compactPipeline.isValid());
    CHECK(fullPipeline.isValid());
    CHECK(compactPipeline != fullPipeline);
    CHECK(device.stats().pipelineCount == 2);
    cache.clear(device);
    CHECK(device.stats().pipelineCount == 0);
    CHECK(device.stats().shaderCount == 0);
}

TEST_CASE("M11: renderer owns PBR textures and rejects malformed mip chains") {
    gfx::NullDevice device;
    ecs::Registry registry(8);
    engine::Renderer renderer(registry, device);
    REQUIRE(renderer.init() == gfx::GfxError::None);

    const resources::Texture valid = makeRgbaTexture(2, 2, 220, 40, 20, 255);
    const auto handle = renderer.createTexture(valid);
    CHECK(handle.isValid());

    resources::Texture malformed = valid;
    malformed.mips[0].bytes.pop();
    CHECK_FALSE(renderer.createTexture(malformed).isValid());
    resources::Texture compressed = valid;
    compressed.format = resources::TextureFormat::BC7_UNORM;
    CHECK_FALSE(renderer.createTexture(compressed).isValid());
    renderer.destroyTexture(handle);
}

TEST_CASE("M11: renderer supplies missing PBR vertex channels") {
    gfx::NullDevice device;
    ecs::Registry registry(4);
    engine::Renderer renderer(registry, device);
    REQUIRE(renderer.init() == gfx::GfxError::None);
    const std::uint64_t buffersBeforeMesh = device.stats().bufferCount;
    const auto mesh = renderer.createMesh(makePositionOnlyTriangle());
    REQUIRE(mesh.isValid());
    // Position, index, and the renderer-generated normal/UV/color stream.
    CHECK(device.stats().bufferCount == buffersBeforeMesh + 3);
    renderer.destroyMesh(mesh);
    CHECK(device.stats().bufferCount == buffersBeforeMesh);

    resources::Mesh invalid = makePositionOnlyTriangle();
    invalid.attributes[0].offsetBytes = std::numeric_limits<std::uint32_t>::max();
    CHECK_FALSE(renderer.createMesh(invalid).isValid());
}

TEST_CASE("M11: shadow casters produce a graph-routed shadow and main draw") {
    gfx::NullDevice device;
    ecs::Registry registry(16);
    engine::Renderer renderer(registry, device);
    REQUIRE(renderer.init() == gfx::GfxError::None);
    const auto mesh = renderer.createMesh(makeM11Triangle());
    REQUIRE(mesh.isValid());
    const auto material = renderer.createMaterial({});
    REQUIRE(material.isValid());

    const ecs::Entity cameraEntity = registry.create();
    engine::Camera camera;
    camera.eye = {0.0f, 0.0f, 3.0f};
    registry.emplace<engine::Camera>(cameraEntity, camera);
    const ecs::Entity lightEntity = registry.create();
    engine::DirectionalLight light;
    light.direction = {-0.4f, -1.0f, -0.3f};
    light.intensity = 3.0f;
    registry.emplace<engine::DirectionalLight>(lightEntity, light);
    const ecs::Entity object = registry.create();
    registry.emplace<ecs::LocalTransform>(object, ecs::LocalTransform{});
    registry.emplace<engine::MeshRenderer>(
        object, engine::MeshRenderer{mesh, material, 0, true, false});
    ecs::updateWorldTransforms(registry);

    gfx::FramebufferDesc framebufferDesc;
    framebufferDesc.extent = {64, 64};
    framebufferDesc.attachments.push(gfx::AttachmentDesc{});
    auto framebuffer = device.createFramebuffer(framebufferDesc);
    REQUIRE(framebuffer.hasValue());
    REQUIRE(renderer.render(framebuffer.value(), cameraEntity, 1.0f) ==
            gfx::GfxError::None);
    CHECK(device.stats().drawIndexedCalls == 2);
    CHECK(renderer.stats().draws == 1);
    CHECK(renderer.graph().resourceCount() == 3);
    CHECK(renderer.graph().passCount() == 2);
    CHECK(renderer.graph().pass(0).accesses.size() == 1);
    CHECK(renderer.graph().pass(0).accesses[0].state.layout ==
          gfx::TextureLayout::ColorAttachment);
    CHECK(renderer.graph().pass(1).accesses[0].state.layout ==
          gfx::TextureLayout::ShaderRead);
    CHECK(device.destroyFramebuffer(framebuffer.value()) == gfx::GfxError::None);
}

#if defined(F4_GRAPHICS_HAS_OPENGL) && defined(F4_GRAPHICS_HAS_VULKAN) && \
    defined(F4_GRAPHICS_HAS_DX12)
TEST_CASE("M11: native PBR paths preserve front faces and fill missing vertex channels") {
    auto windowResult = platform::Window::create({"M11 parity", 64, 64, false});
    REQUIRE(windowResult.hasValue());
    platform::Window* window = windowResult.value();
    REQUIRE(window->makeCurrent());
    REQUIRE(platform::initGl(4, 3).hasValue());
    gfx::IDevice* openGlBase = gfx::createOpenGLDevice();
    REQUIRE(openGlBase != nullptr);
    const auto openGl = capturePbrFrame(
        *openGlBase, false, [](gfx::TextureHandle, std::span<std::byte> output) {
            glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, output.data());
            return glGetError() == GL_NO_ERROR ? gfx::GfxError::None
                                               : gfx::GfxError::DeviceLost;
        });
    const auto openGlPositionOnly = capturePbrFrame(
        *openGlBase, false, [](gfx::TextureHandle, std::span<std::byte> output) {
            glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, output.data());
            return glGetError() == GL_NO_ERROR ? gfx::GfxError::None
                                               : gfx::GfxError::DeviceLost;
        }, true);
    const auto openGlBatched = capturePbrFrame(
        *openGlBase, false, [](gfx::TextureHandle, std::span<std::byte> output) {
            glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, output.data());
            return glGetError() == GL_NO_ERROR ? gfx::GfxError::None
                                               : gfx::GfxError::DeviceLost;
        }, false, true);
    gfx::deleteDevice(openGlBase);
    delete window;

    gfx::IDevice* vulkanBase = gfx::createVulkanDevice();
    REQUIRE(vulkanBase != nullptr);
    auto* vulkan = static_cast<gfx::VulkanDevice*>(vulkanBase);
    const auto vk = capturePbrFrame(
        *vulkan, false, [vulkan](gfx::TextureHandle texture, std::span<std::byte> output) {
            return vulkan->readTexture(texture, 0, output);
        });
    const auto vkPositionOnly = capturePbrFrame(
        *vulkan, false, [vulkan](gfx::TextureHandle texture,
                                std::span<std::byte> output) {
            return vulkan->readTexture(texture, 0, output);
        }, true);
    const auto vkBatched = capturePbrFrame(
        *vulkan, false, [vulkan](gfx::TextureHandle texture,
                                std::span<std::byte> output) {
            return vulkan->readTexture(texture, 0, output);
        }, false, true);
    CHECK(vulkan->validationWarningCount() == 0);
    CHECK(vulkan->validationErrorCount() == 0);
    gfx::deleteDevice(vulkanBase);

    gfx::IDevice* dx12Base = gfx::createDx12Device();
    REQUIRE(dx12Base != nullptr);
    auto* dx12 = static_cast<gfx::Dx12Device*>(dx12Base);
    const auto dx = capturePbrFrame(
        *dx12, false, [dx12](gfx::TextureHandle texture, std::span<std::byte> output) {
            return dx12->readTexture(texture, 0, output);
        });
    const auto dxPositionOnly = capturePbrFrame(
        *dx12, false, [dx12](gfx::TextureHandle texture,
                             std::span<std::byte> output) {
            return dx12->readTexture(texture, 0, output);
        }, true);
    const auto dxBatched = capturePbrFrame(
        *dx12, false, [dx12](gfx::TextureHandle texture,
                             std::span<std::byte> output) {
            return dx12->readTexture(texture, 0, output);
        }, false, true);
    CHECK(dx12->validationWarningCount() == 0);
    CHECK(dx12->validationErrorCount() == 0);
    gfx::deleteDevice(dx12Base);

    const auto openGlCenter = centerPixel(openGl);
    const auto vkCenter = centerPixel(vk);
    const auto dxCenter = centerPixel(dx);
    const auto openGlPositionOnlyCenter = centerPixel(openGlPositionOnly);

    INFO("OpenGL center=" << static_cast<int>(openGlCenter[0]) << ","
                           << static_cast<int>(openGlCenter[1]) << ","
                           << static_cast<int>(openGlCenter[2]));
    INFO("Vulkan center=" << static_cast<int>(vkCenter[0]) << ","
                           << static_cast<int>(vkCenter[1]) << ","
                           << static_cast<int>(vkCenter[2]));
    INFO("DX12 center=" << static_cast<int>(dxCenter[0]) << ","
                         << static_cast<int>(dxCenter[1]) << ","
                         << static_cast<int>(dxCenter[2]));
    for (std::size_t channel = 0; channel < 3; ++channel) {
        CHECK(openGlCenter[channel] > 20);
        CHECK(openGlPositionOnlyCenter[channel] > 20);
    }
    const auto glVk = engine::compareShaderImagesRgba8(openGl, vk, 2);
    const auto glDx = engine::compareShaderImagesRgba8(openGl, dx, 2);
    const auto compactGlVk = engine::compareShaderImagesRgba8(
        openGlPositionOnly, vkPositionOnly, 2);
    const auto compactGlDx = engine::compareShaderImagesRgba8(
        openGlPositionOnly, dxPositionOnly, 2);
    const auto batchedGlVk = engine::compareShaderImagesRgba8(openGlBatched, vkBatched, 2);
    const auto batchedGlDx = engine::compareShaderImagesRgba8(openGlBatched, dxBatched, 2);
    INFO("full PBR GL/VK mismatches=" << glVk.mismatchPixelCount
                                       << " max delta="
                                       << static_cast<int>(glVk.maxChannelDelta)
                                       << " GL/DX mismatches=" << glDx.mismatchPixelCount
                                       << " max delta="
                                       << static_cast<int>(glDx.maxChannelDelta));
    CHECK(glVk.matches());
    CHECK(glDx.matches());
    CHECK(compactGlVk.matches());
    CHECK(compactGlDx.matches());
    CHECK(litPixelCount(openGlBatched) > 100);
    CHECK(batchedGlVk.matches());
    CHECK(batchedGlDx.matches());
}

TEST_CASE("M11: filtered shadow pass executes cleanly on all three backends") {
    auto windowResult = platform::Window::create({"M11 shadows", 64, 64, false});
    REQUIRE(windowResult.hasValue());
    platform::Window* window = windowResult.value();
    REQUIRE(window->makeCurrent());
    REQUIRE(platform::initGl(4, 3).hasValue());
    gfx::IDevice* openGlBase = gfx::createOpenGLDevice();
    REQUIRE(openGlBase != nullptr);
    const auto openGl = capturePbrFrame(
        *openGlBase, true, [](gfx::TextureHandle, std::span<std::byte> output) {
            glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, output.data());
            return glGetError() == GL_NO_ERROR ? gfx::GfxError::None
                                               : gfx::GfxError::DeviceLost;
        });
    gfx::deleteDevice(openGlBase);
    delete window;

    gfx::IDevice* vulkanBase = gfx::createVulkanDevice();
    REQUIRE(vulkanBase != nullptr);
    auto* vulkan = static_cast<gfx::VulkanDevice*>(vulkanBase);
    const auto vk = capturePbrFrame(
        *vulkan, true, [vulkan](gfx::TextureHandle texture, std::span<std::byte> output) {
            return vulkan->readTexture(texture, 0, output);
        });
    CHECK(vulkan->validationWarningCount() == 0);
    CHECK(vulkan->validationErrorCount() == 0);
    gfx::deleteDevice(vulkanBase);

    gfx::IDevice* dx12Base = gfx::createDx12Device();
    REQUIRE(dx12Base != nullptr);
    auto* dx12 = static_cast<gfx::Dx12Device*>(dx12Base);
    const auto dx = capturePbrFrame(
        *dx12, true, [dx12](gfx::TextureHandle texture, std::span<std::byte> output) {
            return dx12->readTexture(texture, 0, output);
        });
    CHECK(dx12->validationWarningCount() == 0);
    CHECK(dx12->validationErrorCount() == 0);
    gfx::deleteDevice(dx12Base);

    const auto openGlCenter = centerPixel(openGl);
    const auto vkCenter = centerPixel(vk);
    const auto dxCenter = centerPixel(dx);

    INFO("shadow OpenGL center=" << static_cast<int>(openGlCenter[0]) << ","
                                  << static_cast<int>(openGlCenter[1]) << ","
                                  << static_cast<int>(openGlCenter[2]));
    INFO("shadow Vulkan center=" << static_cast<int>(vkCenter[0]) << ","
                                  << static_cast<int>(vkCenter[1]) << ","
                                  << static_cast<int>(vkCenter[2]));
    INFO("shadow DX12 center=" << static_cast<int>(dxCenter[0]) << ","
                                << static_cast<int>(dxCenter[1]) << ","
                                << static_cast<int>(dxCenter[2]));
    for (std::size_t channel = 0; channel < 3; ++channel) {
        CHECK(openGlCenter[channel] > 20);
    }
    const auto glVk = engine::compareShaderImagesRgba8(openGl, vk, 8);
    const auto glDx = engine::compareShaderImagesRgba8(openGl, dx, 2);
    INFO("shadow GL/VK mismatches=" << glVk.mismatchPixelCount
                                     << " max delta="
                                     << static_cast<int>(glVk.maxChannelDelta)
                                     << " GL/DX mismatches=" << glDx.mismatchPixelCount
                                     << " max delta="
                                     << static_cast<int>(glDx.maxChannelDelta));
    CHECK(glVk.mismatchPixelCount <= (glVk.pixelCount * 2u) / 100u);
    CHECK(glVk.maxChannelDelta <= 48);
    CHECK(glDx.matches());
}
#endif
