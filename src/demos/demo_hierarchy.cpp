#include "demos/demo_hierarchy.h"
#include <cstring>
#include <cmath>
#include <vector>
#include <cstdint>
#include "core/ecs/registry.h"
#include "core/ecs/transform.h"
#include "core/math/mat4.h"
#include "core/math/vec3.h"
#include "core/math/quat.h"
#include "graphics/command/command_buffer.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

using namespace f4;

namespace f4::demo {

// ---------------------------------------------------------------- ECS state
struct HierarchyScene::Impl {
    ecs::Registry registry{32};
    ecs::Entity sun, earth, moon;   // earth orbits sun, moon orbits earth (slower)
    Impl() : registry(32) {}
};

namespace {

// ------------------------------------------------------------------ mesh gen
struct Vtx { float x, y, z; float r, g, b; };

// Indexed UV sphere, unit radius centered at origin.
// Vertices: pos(3) + color(3) = 24 bytes. Indices: CCW winding for outward normals with Back culling.
void makeSphere(std::vector<Vtx>& verts, std::vector<std::uint16_t>& idx,
                int stacks, int slices) {
    const float pi = 3.14159265359f;
    verts.clear(); idx.clear();
    verts.reserve((stacks + 1) * slices + 2);
    // top pole
    verts.push_back({0, 1, 0, 1, 1, 1});
    for (int i = 1; i <= stacks - 1; ++i) {
        float lat = pi * (0.5f - float(i) / stacks);
        float y = std::sin(lat), r = std::cos(lat);
        for (int j = 0; j < slices; ++j) {
            float lon = 2 * pi * float(j) / slices;
            verts.push_back({r * std::cos(lon), y, r * std::sin(lon), 1, 1, 1});
        }
    }
    // bottom pole
    verts.push_back({0, -1, 0, 1, 1, 1});

    auto ringIdx = [&](int ring) { return 1 + (ring - 1) * slices; };
    // top cap (CCW from outside = looking down from +Y)
    for (int j = 0; j < slices; ++j) {
        int a = 1 + j;
        int b = 1 + (j + 1) % slices;
        idx.push_back(0); idx.push_back(b); idx.push_back(a);
    }
    // quads between rings (CCW from outside)
    for (int i = 1; i < stacks - 1; ++i) {
        for (int j = 0; j < slices; ++j) {
            int a = ringIdx(i) + j;
            int b = ringIdx(i) + (j + 1) % slices;
            int c = ringIdx(i + 1) + (j + 1) % slices;
            int d = ringIdx(i + 1) + j;
            // Two triangles per quad, CCW from outside
            idx.push_back(a); idx.push_back(b); idx.push_back(c);
            idx.push_back(a); idx.push_back(c); idx.push_back(d);
        }
    }
    // bottom cap (CCW from outside = looking up from below)
    int lastRing = ringIdx(stacks - 1);
    for (int j = 0; j < slices; ++j) {
        int a = lastRing + j;
        int b = lastRing + (j + 1) % slices;
        // CCW from outside (looking up from below): bottomPole, a, b
        idx.push_back((int)verts.size() - 1); idx.push_back(a); idx.push_back(b);
    }
}

constexpr const char* kVs = R"glsl(
#version 430 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
layout(std140,binding=0) uniform Ubo{ mat4 viewProj; mat4 model; vec4 tint; };
out vec3 vColor;
out vec3 vNormalVS;
void main(){
    gl_Position = viewProj * model * vec4(aPos,1.0);
    // world-space normal: sphere is unit, so its position IS its normal (before model scale).
    vec3 worldN = normalize(mat3(model) * aPos);
    vNormalVS = worldN;
    vColor = aColor * tint.rgb;
}
)glsl";
constexpr const char* kFs = R"glsl(
#version 430 core
in vec3 vColor;
in vec3 vNormalVS;
out vec4 fragColor;
void main(){
    // soft two-sided lighting: dark side never falls below a readable floor
    vec3 N = normalize(vNormalVS);
    float light = 0.75 + 0.25 * max(N.y * 0.5 + N.z * 0.5 + N.x * 0.5, 0.0);
    fragColor = vec4(vColor * light, 1.0);
}
)glsl";
} // namespace

// ---------------------------------------------------------------- lifecycle
bool HierarchyScene::init(gfx::IDevice* dev, platform::Window*) {
    dev_ = dev;
    impl_ = new Impl();
    auto& reg = impl_->registry;

    // Hierarchy: Sun (root, at origin) -> Earth (orbits sun) -> Moon (orbits earth, slower).
    impl_->sun = reg.create();
    auto& sLt = reg.emplace<ecs::LocalTransform>(impl_->sun);
    sLt.position = math::Vec3(0, 0, 0);
    sLt.rotation = math::Quat::identity();
    sLt.scale = math::Vec3(1.4f, 1.4f, 1.4f);

    impl_->earth = reg.create();
    auto& eLt = reg.emplace<ecs::LocalTransform>(impl_->earth);
    eLt.position = math::Vec3(2.6f, 0, 0);
    eLt.rotation = math::Quat::identity();
    eLt.scale = math::Vec3(0.8f, 0.8f, 0.8f);
    reg.emplace<ecs::Parent>(impl_->earth, impl_->sun);

    impl_->moon = reg.create();
    auto& mLt = reg.emplace<ecs::LocalTransform>(impl_->moon);
    mLt.position = math::Vec3(1.5f, 0, 0);
    mLt.rotation = math::Quat::identity();
    mLt.scale = math::Vec3(0.45f, 0.45f, 0.45f);
    reg.emplace<ecs::Parent>(impl_->moon, impl_->earth);

    ecs::updateWorldTransforms(reg);

    // Sphere (indexed)
    std::vector<Vtx> sVerts; std::vector<std::uint16_t> sIdx;
    makeSphere(sVerts, sIdx, 32, 48);
    sphereIndexCount_ = (std::uint32_t)sIdx.size();
    {
        gfx::BufferDesc d; d.sizeBytes = sVerts.size() * sizeof(Vtx); d.usage = gfx::BufferUsage::Vertex;
        auto r = dev->createBuffer(d, std::span(reinterpret_cast<const std::byte*>(sVerts.data()), sVerts.size() * sizeof(Vtx)));
        if (!r.hasValue()) return false;
        vbSphere_ = r.value();
    }
    {
        gfx::BufferDesc d; d.sizeBytes = sIdx.size() * sizeof(std::uint16_t); d.usage = gfx::BufferUsage::Index;
        auto r = dev->createBuffer(d, std::span(reinterpret_cast<const std::byte*>(sIdx.data()), sIdx.size() * sizeof(std::uint16_t)));
        if (!r.hasValue()) return false;
        ibSphere_ = r.value();
    }

    // World grid: the fixed 3D background coordinate system.
    engine::WorldGridDesc gd;
    gd.extent = 5.0f; gd.divisions = 10; gd.lineThickness = 0.02f;
    if (grid_.create(*dev, gd) != gfx::GfxError::None) return false;

    // Per-object constant ring (std140-aligned: mat4 + mat4 + vec4 = 144, pad to 160).
    if (constRing_.create(*dev, 160, 16) != gfx::GfxError::None) return false;

    // Shaders
    auto mk = [&](gfx::ShaderStage s, const char* src) {
        gfx::ShaderDesc d; d.stage = s; d.blob.resize(std::strlen(src));
        std::memcpy(d.blob.data(), src, d.blob.size()); return dev->createShader(d);
    };
    auto vs = mk(gfx::ShaderStage::Vertex, kVs);
    auto fs = mk(gfx::ShaderStage::Fragment, kFs);
    if (!vs.hasValue() || !fs.hasValue()) return false;
    vs_ = vs.value(); fs_ = fs.value();

    auto makePipe = [&](gfx::ShaderHandle v, gfx::ShaderHandle f) {
        gfx::PipelineStateDesc pd; pd.vertexShader = v; pd.fragmentShader = f;
        pd.topology = gfx::PrimitiveTopology::TriangleList; pd.raster.cullMode = gfx::CullMode::Back;
        pd.depthStencil.depthTestEnable = true; pd.depthStencil.depthWriteEnable = true; pd.depthStencil.depthCompare = gfx::CompareOp::Less;
        gfx::VertexInputLayout::VertexBinding b; b.binding = 0; b.strideBytes = sizeof(Vtx); pd.vertexInput.bindings.push(b);
        gfx::VertexInputLayout::VertexAttribute pa; pa.binding = 0; pa.location = 0; pa.format = gfx::VertexFormat::Float32x3; pa.offsetBytes = 0; pd.vertexInput.attributes.push(pa);
        gfx::VertexInputLayout::VertexAttribute ca; ca.binding = 0; ca.location = 1; ca.format = gfx::VertexFormat::Float32x3; ca.offsetBytes = 12; pd.vertexInput.attributes.push(ca);
        auto r = dev->createPipeline(pd); return r.hasValue() ? r.value() : gfx::PipelineHandle{};
    };
    pipeSphere_ = makePipe(vs_, fs_);
    return pipeSphere_.isValid();
}

void HierarchyScene::shutdown(gfx::IDevice* dev) {
    grid_.destroy(*dev);
    constRing_.destroy(*dev);
    if (pipeSphere_.isValid()) dev->destroyPipeline(pipeSphere_);
    if (fs_.isValid()) dev->destroyShader(fs_);
    if (vs_.isValid()) dev->destroyShader(vs_);
    if (ibSphere_.isValid()) dev->destroyBuffer(ibSphere_);
    if (vbSphere_.isValid()) dev->destroyBuffer(vbSphere_);
    delete impl_; impl_ = nullptr;
}

void HierarchyScene::update(float dt, const Uint8* keys, const SDL_MouseMotionEvent*) {
    time_ += dt;
    bool g = keys[SDL_SCANCODE_G];
    if (g && !gHeld_) showGrid_ = !showGrid_;
    gHeld_ = g;

    // Grid scale controls: S scales up, T scales down (clamped).
    {
        if (keys[SDL_SCANCODE_S] && !sHeld_) { gridExtent_ = std::min(gridExtent_ * 1.4f, 40.f); sHeld_ = true; }
        if (!keys[SDL_SCANCODE_S]) sHeld_ = false;
        if (keys[SDL_SCANCODE_T] && !tHeld_) { gridExtent_ = std::max(gridExtent_ / 1.4f, 0.5f); tHeld_ = true; }
        if (!keys[SDL_SCANCODE_T]) tHeld_ = false;
        if (dev_) {
            engine::WorldGridDesc gd; gd.extent = gridExtent_; gd.divisions = gridDivisions_;
            gd.lineThickness = 0.06f;
            (void)grid_.setScale(*dev_, gd);
        }
    }

    if (!impl_) return;
    auto& reg = impl_->registry;
    // Sun spins slowly (its own rotation/day).
    if (auto* lt = reg.get<ecs::LocalTransform>(impl_->sun)) {
        lt->rotation = math::Quat::fromAxisAngle(math::Vec3(0, 1, 0), time_ * 0.2f);
        ecs::tagTransformDirty(reg, impl_->sun);
    }
    // Earth is a child of Sun offset on +X. To orbit the Sun we rotate Earth about
    // the Sun's Y axis (i.e. Earth's own rotation carries it around the origin).
    // Earth orbits the Sun once per ~4s.
    if (auto* lt = reg.get<ecs::LocalTransform>(impl_->earth)) {
        lt->rotation = math::Quat::fromAxisAngle(math::Vec3(0, 1, 0), time_ * 1.0f);
        ecs::tagTransformDirty(reg, impl_->earth);
    }
    // Moon is a child of Earth offset on +X, tilted ring, orbits SLOWER (~0.4x Earth).
    if (auto* lt = reg.get<ecs::LocalTransform>(impl_->moon)) {
        lt->rotation = math::Quat::fromAxisAngle(math::Vec3(1, 0, 0), 0.35f) *
                       math::Quat::fromAxisAngle(math::Vec3(0, 1, 0), time_ * 0.4f);
        ecs::tagTransformDirty(reg, impl_->moon);
    }
    ecs::updateWorldTransforms(reg);
}

void HierarchyScene::render(gfx::CommandBuffer& cb, const FreeCamera& cam, std::uint32_t w, std::uint32_t h) {
    if (!impl_) return;
    constRing_.beginFrame();
    float aspect = h > 0 ? float(w) / float(h) : 1.f;
    math::Mat4 viewProj = cam.viewProjMatrix(aspect);
    bool atDefault = (cam.position.x == 0.f && cam.position.y == 5.f && cam.position.z == 8.f &&
                      cam.yaw == 1.57079633f && cam.pitch == 0.55f);
    if (atDefault) {
        // Elevated 3/4 view looking down at the XZ ground grid so it reads as the
        // background Cartesian coordinate system.
        math::Vec3 eye{ 5.2f, 6.2f, 5.2f };
        math::Vec3 target(0.0f, 0.0f, 0.0f);
        math::Mat4 view = math::Mat4::lookAtRH(eye, target, math::Vec3(0, 1, 0));
        math::Mat4 proj = math::Mat4::perspectiveRH_ZO(1.04719758f, aspect, 0.1f, 100.f);
        viewProj = proj.mul(view);
    }

    // draw the world background grid (fixed coordinate system)
    if (showGrid_) grid_.render(cb, viewProj);

    // draw spheres using each entity's WorldMatrix. Each body gets its own UBO slot from
    // the ConstantRing so the deferred draws each sample the correct model+tint at submit.
    cb.bindPipeline(pipeSphere_);
    cb.bindVertexBuffer(0, vbSphere_, sizeof(Vtx));
    cb.bindIndexBuffer(ibSphere_, gfx::IndexFormat::U16);
    auto drawOne = [&](ecs::Entity e, math::Vec4 tint) {
        auto* wm = impl_->registry.get<ecs::WorldMatrix>(e);
        if (!wm) return;
        struct Ubo { math::Mat4 viewProj; math::Mat4 model; math::Vec4 tint; };
        Ubo ubo{viewProj, wm->matrix, tint};
        auto slot = constRing_.writeAndBind(*dev_, cb, std::span(reinterpret_cast<const std::byte*>(&ubo), sizeof(ubo)));
        if (slot.valid) cb.drawIndexed(sphereIndexCount_, 1);
    };
    // Sun yellow, Earth blue, Moon gray
    drawOne(impl_->sun, math::Vec4(1.f, 0.85f, 0.15f, 1.f));
    drawOne(impl_->earth, math::Vec4(0.2f, 0.45f, 1.f, 1.f));
    drawOne(impl_->moon, math::Vec4(0.78f, 0.78f, 0.8f, 1.f));
}

} // namespace f4::demo
