#include "engine/rendering/renderer.h"

#include <cstring>
#include <span>

#include "core/ecs/view.h"
#include "core/ecs/transform.h"
#include "core/math/mat4.h"
#include "engine/cameras/camera.h"
#include "graphics/texture/texture_desc.h"
#include "graphics/sampler/sampler_desc.h"
#include "graphics/framebuffer/framebuffer_desc.h"

namespace f4::engine {

namespace {

struct SceneUbo {
    math::Mat4 viewProj;
    math::Mat4 model;
    math::Vec4 tint;
};
static_assert(sizeof(SceneUbo) == 144, "SceneUbo std140");

} // namespace

Renderer::Renderer(ecs::Registry& registry, gfx::IDevice& device)
    : registry_(registry), device_(device), meshArena_(64) {}

Renderer::~Renderer() {
    device_.waitIdle();
    if (whiteSmp_.isValid()) device_.destroySampler(whiteSmp_);
    if (whiteTex_.isValid()) device_.destroyTexture(whiteTex_);
    sceneConstants_.destroy(device_);
    for (auto& kv : meshMap_) {
        destroyGpuMesh(device_, kv.second.gpu);
    }
    materials_.clear(device_);
    (void)graph_.release(device_);
}

auto Renderer::init() -> gfx::GfxError {
    if (inited_) return gfx::GfxError::None;
    {
        const gfx::GfxError error = sceneConstants_.create(device_, sizeof(SceneUbo), 64);
        if (error != gfx::GfxError::None) return error;
    }
    {
        gfx::TextureDesc td; td.width = 1; td.height = 1; td.format = gfx::TextureFormat::RGBA8_UNORM; td.mipLevels = 1;
        auto tr = device_.createTexture(td);
        if (!tr.hasValue()) return tr.error();
        const gfx::TextureHandle texture = tr.value();
        const std::uint8_t white[4] = {0xFF,0xFF,0xFF,0xFF};
        const auto pixels = std::span(reinterpret_cast<const std::byte*>(white), 4);
        if (const auto error = device_.writeTexture(texture, 0, pixels);
            error != gfx::GfxError::None) {
            (void)device_.destroyTexture(texture);
            return error;
        }
        auto sr = device_.createSampler(gfx::SamplerDesc{});
        if (!sr.hasValue()) {
            (void)device_.destroyTexture(texture);
            return sr.error();
        }
        whiteTex_ = texture;
        whiteSmp_ = sr.value();
    }
    inited_ = true;
    return gfx::GfxError::None;
}

auto Renderer::createMesh(const resources::Mesh& mesh) -> resources::MeshHandle {
    auto h = meshArena_.acquire(mesh);
    auto gpuRes = createGpuMesh(device_, mesh);
    if (!gpuRes.hasValue()) { meshArena_.release(h); return resources::MeshHandle{}; }
    MeshEntry e; e.mesh = mesh; e.gpu = static_cast<GpuMesh&&>(gpuRes.value());
    meshMap_.set(h, static_cast<MeshEntry&&>(e));
    return h;
}

void Renderer::destroyMesh(resources::MeshHandle h) {
    auto* e = meshMap_.find(h);
    if (e != nullptr) { destroyGpuMesh(device_, e->gpu); meshMap_.erase(h); }
    if (meshArena_.contains(h)) meshArena_.release(h);
}

auto Renderer::createMaterial(const StandardMaterialDesc& desc) -> resources::MaterialHandle {
    return materials_.getOrCreate(desc);
}

auto Renderer::ensureMaterialPipeline(resources::MaterialHandle h) -> gfx::GfxError {
    return materials_.ensurePipeline(device_, h);
}

void Renderer::record(gfx::CommandBuffer& cb, ecs::Entity cameraEntity, float aspect) {
    stats_ = {};
    updateCameras(registry_, aspect);
    const Camera* cam = registry_.get<Camera>(cameraEntity);
    if (cam == nullptr) return;
    const math::Mat4 view = cameraView(*cam);
    sceneConstants_.beginFrame();
    queue_.clear();
    for (auto [entity, renderer, world] : ecs::View<MeshRenderer, ecs::WorldMatrix>(registry_)) {
        (void)entity;
        auto* entry = meshMap_.find(renderer.mesh);
        if (entry == nullptr) continue;
        const StandardMaterial* mat = materials_.get(renderer.material);
        if (mat == nullptr) continue;
        if (!mat->pipeline.isValid()) {
            if (auto err = materials_.ensurePipeline(device_, renderer.material); err != gfx::GfxError::None) continue;
        }
        const auto* mat2 = materials_.get(renderer.material);
        RenderItem item;
        item.entity = entity;
        item.meshRenderer = renderer;
        item.material = renderer.material;
        item.depth = -view.transformPoint(world.matrix.col[3].xyz()).z;
        item.transparent = renderer.transparent || mat2->desc.transparent;
        item.pipelineSortKey = mat2->pipeline.index;
        queue_.push(item);
    }
    queue_.sort();
    for (const RenderItem& item : queue_.items()) {
        auto* entry = meshMap_.find(item.meshRenderer.mesh);
        if (entry == nullptr) continue;
        const StandardMaterial* mat = materials_.get(item.material);
        if (mat == nullptr || !mat->pipeline.isValid()) continue;
        const ecs::WorldMatrix* wm = registry_.get<ecs::WorldMatrix>(item.entity);
        if (wm == nullptr) continue;
        SceneUbo ubo; ubo.viewProj = cam->viewProj; ubo.model = wm->matrix; ubo.tint = mat->desc.tint;
        const ConstantRing::Slot constants = sceneConstants_.alloc(
            device_, std::span(reinterpret_cast<const std::byte*>(&ubo), sizeof(ubo)));
        if (!constants.valid) continue;
        cb.bindPipeline(mat->pipeline);
        sceneConstants_.bind(cb, constants);
        gfx::TextureHandle tex = whiteTex_;
        gfx::SamplerHandle smp = whiteSmp_;
        cb.bindResource(0, gfx::BindingType::Texture, tex.index, tex.generation);
        cb.bindResource(0, gfx::BindingType::Sampler, smp.index, smp.generation);
        for (std::size_t bi = 0; bi < entry->gpu.vertexBuffers.size(); ++bi) {
            const auto& vb = entry->gpu.vertexBuffers[bi];
            const auto& binding = entry->gpu.layout.bindings[bi];
            cb.bindVertexBuffer(static_cast<std::uint32_t>(bi), vb, binding.strideBytes);
        }
        if (entry->gpu.indexBuffer.isValid()) {
            cb.bindIndexBuffer(entry->gpu.indexBuffer, entry->gpu.indexFormat);
            const std::uint32_t sub = item.meshRenderer.subMeshIndex;
            const auto& mesh = entry->mesh;
            if (sub < mesh.submeshes.size()) {
                const auto& sm = mesh.submeshes[sub];
                cb.drawIndexed(static_cast<std::uint32_t>(sm.indexCount), 1);
            } else {
                cb.drawIndexed(entry->gpu.indexCount, 1);
            }
        } else {
            cb.draw(entry->gpu.vertexCount, 1);
        }
        stats_.draws++; stats_.instances++;
    }
}

auto Renderer::render(gfx::FramebufferHandle fb, ecs::Entity cameraEntity, float aspect) -> gfx::GfxError {
    if (!inited_) return gfx::GfxError::BadDescriptor;
    if (!fb.isValid()) return gfx::GfxError::InvalidHandle;
    if (const gfx::GfxError error = graph_.clear(device_); error != gfx::GfxError::None) return error;

    const gfx::RenderGraph::ResourceId target =
        graph_.importFramebuffer(containers::String("main-target"), fb);
    gfx::PassDesc pass; pass.name = containers::String("main");
    pass.writes.push(target);
    pass.record = [this, cameraEntity, aspect](gfx::CommandBuffer& cb) {
        record(cb, cameraEntity, aspect);
    };
    (void)graph_.declarePass(std::move(pass));
    gfx::CommandBuffer cb;
    return graph_.execute(device_, cb);
}

} // namespace f4::engine
