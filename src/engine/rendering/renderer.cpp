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
    if (whiteSmp_.isValid()) device_.destroySampler(whiteSmp_);
    if (whiteTex_.isValid()) device_.destroyTexture(whiteTex_);
    if (sceneUbo_.isValid()) device_.destroyBuffer(sceneUbo_);
    for (auto& kv : meshMap_) {
        destroyGpuMesh(device_, kv.second.gpu);
    }
    materials_.clear(device_);
    graph_.release(device_);
}

auto Renderer::init() -> gfx::GfxError {
    if (inited_) return gfx::GfxError::None;
    {
        gfx::BufferDesc d; d.sizeBytes = sizeof(SceneUbo); d.usage = gfx::BufferUsage::Uniform;
        auto r = device_.createBuffer(d, {});
        if (!r.hasValue()) return r.error();
        sceneUbo_ = r.value();
    }
    {
        gfx::TextureDesc td; td.width = 1; td.height = 1; td.format = gfx::TextureFormat::RGBA8_UNORM; td.mipLevels = 1;
        auto tr = device_.createTexture(td);
        if (!tr.hasValue()) return tr.error();
        whiteTex_ = tr.value();
        const std::uint8_t white[4] = {0xFF,0xFF,0xFF,0xFF};
        auto err = device_.writeTexture(whiteTex_, 0, std::span(reinterpret_cast<const std::byte*>(white), 4));
        if (err != gfx::GfxError::None) return err;
        auto sr = device_.createSampler(gfx::SamplerDesc{});
        if (!sr.hasValue()) return sr.error();
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
        item.depth = world.matrix.col[3].z;
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
        device_.writeBuffer(sceneUbo_, 0, std::span(reinterpret_cast<const std::byte*>(&ubo), sizeof(ubo)));
        cb.bindPipeline(mat->pipeline);
        cb.bindResource(0, gfx::BindingType::UniformBuffer, sceneUbo_.index, sceneUbo_.generation);
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
    graph_.clear();
    gfx::PassDesc pass; pass.name = containers::String("main");
    pass.record = [this, cameraEntity, aspect](gfx::CommandBuffer& cb) { (void)cb; (void)cameraEntity; (void)aspect; };
    (void)graph_.declarePass(std::move(pass));
    gfx::CommandBuffer cb; cb.begin(); cb.beginRenderPass(fb);
    record(cb, cameraEntity, aspect);
    cb.endRenderPass(); cb.end();
    return device_.submit(cb);
}

} // namespace f4::engine
