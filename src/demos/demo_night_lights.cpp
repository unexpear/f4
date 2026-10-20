#include "demos/demo_night_lights.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/ecs/registry.h"
#include "core/ecs/transform.h"
#include "core/ecs/view.h"
#include "core/resources/mesh.h"
#include "core/resources/texture.h"
#include "engine/cameras/camera.h"
#include "engine/lighting/light.h"
#include "engine/rendering/renderer.h"

namespace f4::demo {

namespace {

constexpr std::array<std::array<float, 3>, 8> kPalette = {{
    {1.0f, 0.55f, 0.18f}, {0.25f, 0.55f, 1.0f}, {1.0f, 0.18f, 0.32f}, {0.25f, 1.0f, 0.45f},
    {0.95f, 0.3f, 1.0f},  {1.0f, 0.9f, 0.55f},  {0.2f, 0.95f, 1.0f},  {1.0f, 0.8f, 0.1f},
}};

struct Vertex {
    float px, py, pz, nx, ny, nz, u, v;
    std::uint8_t r, g, b, a;
};

[[nodiscard]] auto makeMesh(const std::vector<Vertex>& vertices,
                            const std::vector<std::uint16_t>& indices) -> resources::Mesh {
    resources::Mesh mesh;
    resources::VertexStream stream;
    stream.strideBytes = sizeof(Vertex);
    stream.data.resize(vertices.size() * sizeof(Vertex));
    std::memcpy(stream.data.data(), vertices.data(), vertices.size() * sizeof(Vertex));
    mesh.streams.push(static_cast<resources::VertexStream&&>(stream));
    mesh.attributes.push({resources::VertexSemantic::Position, resources::VertexFormat::F32_3, 0, 0});
    mesh.attributes.push({resources::VertexSemantic::Normal, resources::VertexFormat::F32_3, 0, 12});
    mesh.attributes.push({resources::VertexSemantic::TexCoord0, resources::VertexFormat::F32_2, 0, 24});
    mesh.attributes.push({resources::VertexSemantic::Color0, resources::VertexFormat::UNORM8_4, 0, 32});
    mesh.indices.resize(indices.size() * sizeof(std::uint16_t));
    std::memcpy(mesh.indices.data(), indices.data(), indices.size() * sizeof(std::uint16_t));
    mesh.submeshes.push({0, static_cast<std::uint32_t>(indices.size())});
    return mesh;
}

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
    return makeMesh({{-half, 0, -half, 0, 1, 0, 0, 0, 255, 255, 255, 255},
                     {-half, 0, half, 0, 1, 0, 0, 1, 255, 255, 255, 255},
                     {half, 0, half, 0, 1, 0, 1, 1, 255, 255, 255, 255},
                     {half, 0, -half, 0, 1, 0, 1, 0, 255, 255, 255, 255}},
                    {0, 1, 2, 0, 2, 3});
}

[[nodiscard]] auto makeCube() -> resources::Mesh {
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
    const float normals[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto& n : normals) {
        const math::Vec3 normal(n[0], n[1], n[2]);
        const math::Vec3 tangent = std::abs(normal.y) > 0.5f ? math::Vec3(1, 0, 0) : math::Vec3(0, 1, 0);
        const math::Vec3 bitangent = math::cross(normal, tangent);
        const auto base = static_cast<std::uint16_t>(vertices.size());
        const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& c : corners) {
            const math::Vec3 p = normal * 0.5f + tangent * (0.5f * c[0]) + bitangent * (0.5f * c[1]);
            vertices.push_back({p.x, p.y, p.z, normal.x, normal.y, normal.z, c[0] * 0.5f + 0.5f,
                                c[1] * 0.5f + 0.5f, 255, 255, 255, 255});
        }
        for (const std::uint16_t offset : {0, 1, 2, 0, 2, 3}) {
            indices.push_back(static_cast<std::uint16_t>(base + offset));
        }
    }
    return makeMesh(vertices, indices);
}

} // namespace

struct NightLightsScene::Impl {
    explicit Impl(gfx::IDevice& device) : renderer(registry, device) {}

    ecs::Registry registry{16384};
    engine::Renderer renderer;
    ecs::Entity camera{};
    resources::MeshHandle cube{};
    std::array<resources::MaterialHandle, kPalette.size()> lampMaterials{};
    struct Lamp {
        ecs::Entity light{};
        ecs::Entity cube{};
        math::Vec3 base{};
        float phase = 0.0f;
    };
    std::vector<Lamp> lamps;
    std::uint32_t side = 32;
    bool animate = true;
    float time = 0.0f;
    std::array<bool, 12> previousKeys{};
    std::vector<ecs::Entity> pillars;

    void syncCamera(const FreeCamera& freeCamera) {
        if (auto* c = registry.get<engine::Camera>(camera)) {
            const float cy = std::cos(freeCamera.yaw);
            const float sy = std::sin(freeCamera.yaw);
            const float cp = std::cos(freeCamera.pitch);
            const float sp = std::sin(freeCamera.pitch);
            const math::Vec3 forward{-cy * cp, -sp, -sy * cp};
            c->eye = freeCamera.position;
            c->center = freeCamera.position + forward;
            c->up = math::Vec3(0.f, 1.f, 0.f);
        }
    }

    void printSettings() const {
        const auto& s = renderer.pbrSettings();
        std::printf("[night-lights] bloom %s (intensity %.2f, threshold %.2f); point shadows %s "
                    "(budget %u)\n",
                    s.bloomEnabled ? "on" : "off", static_cast<double>(s.bloomIntensity),
                    static_cast<double>(s.bloomThreshold), s.pointShadowsEnabled ? "on" : "off",
                    s.pointShadowBudget);
    }

    void rebuildLamps() {
        for (const Lamp& lamp : lamps) {
            registry.destroy(lamp.light);
            registry.destroy(lamp.cube);
        }
        lamps.clear();
        const float extent = 18.0f;
        for (std::uint32_t row = 0; row < side; ++row) {
            for (std::uint32_t column = 0; column < side; ++column) {
                const float fx = static_cast<float>(column) / static_cast<float>(side - 1);
                const float fz = static_cast<float>(row) / static_cast<float>(side - 1);
                const std::size_t palette = (row * 3u + column * 5u) % kPalette.size();
                Lamp lamp;
                lamp.base = {(fx * 2.0f - 1.0f) * extent, 0.6f, (fz * 2.0f - 1.0f) * extent};
                lamp.phase = static_cast<float>((row * 7u + column * 13u) % 31u) * 0.37f;
                lamp.light = registry.create();
                engine::PointLight light;
                light.position = lamp.base;
                light.color = {kPalette[palette][0], kPalette[palette][1], kPalette[palette][2]};
                light.intensity = 4.0f;
                light.range =
                    std::clamp(2.0f * extent / static_cast<float>(side) * 1.1f, 0.8f, 6.0f);
                // Every lamp may cast shadows; the renderer picks the budget.
                light.castShadows = true;
                registry.emplace<engine::PointLight>(lamp.light, light);
                lamp.cube = registry.create();
                auto& transform = registry.emplace<ecs::LocalTransform>(lamp.cube, ecs::LocalTransform{});
                transform.position = lamp.base;
                transform.scale = math::Vec3(0.14f);
                registry.emplace<engine::MeshRenderer>(
                    lamp.cube, engine::MeshRenderer{cube, lampMaterials[palette], 0, false, false});
                lamps.push_back(lamp);
            }
        }
        std::printf("[night-lights] %zu point lights\n", lamps.size());
    }
};

bool NightLightsScene::init(gfx::IDevice* device, platform::Window*) {
    impl_ = new Impl(*device);
    auto& renderer = impl_->renderer;
    if (renderer.init() != gfx::GfxError::None) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    renderer.pbrSettings().shadowsEnabled = false;
    renderer.pbrSettings().ambientIntensity = 0.02f;

    engine::StandardMaterialDesc groundDesc;
    groundDesc.tint = math::Vec4(0.16f, 0.16f, 0.18f, 1.0f);
    groundDesc.roughnessFactor = 0.8f;
    groundDesc.doubleSided = true;
    const auto groundMaterial = renderer.createMaterial(groundDesc);
    const auto groundMesh = renderer.createMesh(makeGround(26.0f));
    impl_->cube = renderer.createMesh(makeCube());
    if (!groundMaterial.isValid() || !groundMesh.isValid() || !impl_->cube.isValid()) return false;
    // Emissive factors only scale the emissive texture (black by default).
    const auto lampEmission = renderer.createTexture(makeWhiteTexture());
    if (!lampEmission.isValid()) return false;
    for (std::size_t i = 0; i < kPalette.size(); ++i) {
        engine::StandardMaterialDesc lampDesc;
        lampDesc.emissive = lampEmission;
        lampDesc.tint = math::Vec4(0.02f, 0.02f, 0.02f, 1.0f);
        lampDesc.emissiveFactor =
            math::Vec3(kPalette[i][0], kPalette[i][1], kPalette[i][2]) * 3.0f;
        impl_->lampMaterials[i] = renderer.createMaterial(lampDesc);
    }
    const ecs::Entity ground = impl_->registry.create();
    impl_->registry.emplace<ecs::LocalTransform>(ground, ecs::LocalTransform{});
    impl_->registry.emplace<engine::MeshRenderer>(
        ground, engine::MeshRenderer{groundMesh, groundMaterial, 0, false, false});

    // Pillars between the lamps: the shadow casters for point-light shadows.
    engine::StandardMaterialDesc pillarDesc;
    pillarDesc.tint = math::Vec4(0.55f, 0.52f, 0.48f, 1.0f);
    pillarDesc.roughnessFactor = 0.7f;
    const auto pillarMaterial = renderer.createMaterial(pillarDesc);
    if (!pillarMaterial.isValid()) return false;
    constexpr int kPillarsPerSide = 12;
    for (int row = 0; row < kPillarsPerSide; ++row) {
        for (int column = 0; column < kPillarsPerSide; ++column) {
            const float fx = (static_cast<float>(column) + 0.5f) / kPillarsPerSide;
            const float fz = (static_cast<float>(row) + 0.5f) / kPillarsPerSide;
            const ecs::Entity pillar = impl_->registry.create();
            auto& transform =
                impl_->registry.emplace<ecs::LocalTransform>(pillar, ecs::LocalTransform{});
            transform.position = {(fx * 2.0f - 1.0f) * 18.0f + 0.4f, 0.9f,
                                  (fz * 2.0f - 1.0f) * 18.0f - 0.3f};
            transform.scale = math::Vec3(0.35f, 1.8f, 0.35f);
            impl_->registry.emplace<engine::MeshRenderer>(
                pillar, engine::MeshRenderer{impl_->cube, pillarMaterial, 0, true, false});
            impl_->pillars.push_back(pillar);
        }
    }
    renderer.pbrSettings().bloomEnabled = true;
    renderer.pbrSettings().bloomIntensity = 0.8f;
    renderer.pbrSettings().pointShadowsEnabled = true;
    renderer.pbrSettings().pointShadowBudget = 8;

    impl_->camera = impl_->registry.create();
    engine::Camera camera;
    camera.nearPlane = 0.1f;
    camera.farPlane = 100.0f;
    impl_->registry.emplace<engine::Camera>(impl_->camera, camera);
    impl_->rebuildLamps();
    ecs::updateWorldTransforms(impl_->registry);
    impl_->printSettings();
    return true;
}

void NightLightsScene::shutdown(gfx::IDevice* device) {
    if (impl_ == nullptr) return;
    device->waitIdle();
    delete impl_;
    impl_ = nullptr;
}

void NightLightsScene::onResize(std::uint32_t, std::uint32_t) {}

void NightLightsScene::update(float dt, const Uint8* keys, const SDL_MouseMotionEvent*) {
    if (impl_ == nullptr) return;
    const std::array<bool, 12> pressed = {
        keys[SDL_SCANCODE_LEFTBRACKET] != 0, keys[SDL_SCANCODE_RIGHTBRACKET] != 0,
        keys[SDL_SCANCODE_P] != 0,           keys[SDL_SCANCODE_M] != 0,
        keys[SDL_SCANCODE_B] != 0,           keys[SDL_SCANCODE_MINUS] != 0,
        keys[SDL_SCANCODE_EQUALS] != 0,      keys[SDL_SCANCODE_SEMICOLON] != 0,
        keys[SDL_SCANCODE_APOSTROPHE] != 0,  keys[SDL_SCANCODE_K] != 0,
        keys[SDL_SCANCODE_COMMA] != 0,       keys[SDL_SCANCODE_PERIOD] != 0};
    const auto edge = [&](std::size_t i) { return pressed[i] && !impl_->previousKeys[i]; };
    if (edge(0) && impl_->side > 8) {
        impl_->side /= 2;
        impl_->rebuildLamps();
    }
    if (edge(1) && impl_->side < 64) {
        impl_->side *= 2;
        impl_->rebuildLamps();
    }
    if (edge(2)) {
        auto& enabled = impl_->renderer.pbrSettings().pointLightsEnabled;
        enabled = !enabled;
    }
    if (edge(3)) impl_->animate = !impl_->animate;
    auto& settings = impl_->renderer.pbrSettings();
    bool changed = false;
    if (edge(4)) { settings.bloomEnabled = !settings.bloomEnabled; changed = true; }
    if (edge(5)) { settings.bloomIntensity = std::max(0.0f, settings.bloomIntensity - 0.2f); changed = true; }
    if (edge(6)) { settings.bloomIntensity = std::min(3.0f, settings.bloomIntensity + 0.2f); changed = true; }
    if (edge(7)) { settings.bloomThreshold = std::max(0.0f, settings.bloomThreshold - 0.25f); changed = true; }
    if (edge(8)) { settings.bloomThreshold = std::min(8.0f, settings.bloomThreshold + 0.25f); changed = true; }
    if (edge(9)) { settings.pointShadowsEnabled = !settings.pointShadowsEnabled; changed = true; }
    if (edge(10) && settings.pointShadowBudget > 1) { settings.pointShadowBudget /= 2; changed = true; }
    if (edge(11) && settings.pointShadowBudget < 16) { settings.pointShadowBudget *= 2; changed = true; }
    if (changed) impl_->printSettings();
    impl_->previousKeys = pressed;

    if (impl_->animate) impl_->time += dt;
    for (const auto& lamp : impl_->lamps) {
        const float t = impl_->time * 0.8f + lamp.phase;
        const math::Vec3 position = lamp.base + math::Vec3(std::sin(t) * 0.35f,
                                                           std::sin(t * 1.7f) * 0.15f,
                                                           std::cos(t) * 0.35f);
        if (auto* light = impl_->registry.get<engine::PointLight>(lamp.light)) {
            light->position = position;
        }
        if (auto* transform = impl_->registry.get<ecs::LocalTransform>(lamp.cube)) {
            transform->position = position;
            ecs::tagTransformDirty(impl_->registry, lamp.cube);
        }
    }
    ecs::updateWorldTransforms(impl_->registry);
}

void NightLightsScene::render(gfx::CommandBuffer& cb, const FreeCamera& camera,
                              std::uint32_t width, std::uint32_t height) {
    // Fallback single-pass path (no bloom, no point shadows): the bench uses
    // renderPasses() because recordsOwnPasses() is true.
    if (impl_ == nullptr) return;
    impl_->syncCamera(camera);
    impl_->renderer.record(
        cb, impl_->camera,
        height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.f);
}

void NightLightsScene::renderPasses(gfx::CommandBuffer&, const FreeCamera& camera,
                                    std::uint32_t width, std::uint32_t height,
                                    gfx::FramebufferHandle target) {
    if (impl_ == nullptr || width == 0 || height == 0) return;
    impl_->syncCamera(camera);
    // Renderer::render() records and submits its own render graph (sun
    // shadow, point-shadow atlas, main pass, bloom chain); the bench then
    // draws the HUD on top through its load-only overlay framebuffer.
    const gfx::GfxError error = impl_->renderer.render(
        target, impl_->camera, static_cast<float>(width) / static_cast<float>(height),
        gfx::Extent{width, height});
    if (error != gfx::GfxError::None) {
        std::fprintf(stderr, "[night-lights] render failed: %d\n", static_cast<int>(error));
    }
}

} // namespace f4::demo
