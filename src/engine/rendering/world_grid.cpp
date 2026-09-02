#include "engine/rendering/world_grid.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

namespace f4::engine {

namespace {

struct Vtx { float x, y, z; float r, g, b; };
static_assert(sizeof(Vtx) == 24, "world grid vertex layout");

constexpr const char* kVs = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(std140, binding = 0) uniform Ubo { mat4 viewProj; mat4 model; vec4 tint; };
out vec3 vColor;
void main() { gl_Position = viewProj * model * vec4(aPos, 1.0); vColor = aColor * tint.rgb; }
)glsl";

constexpr const char* kFs = R"glsl(
#version 430 core
in vec3 vColor;
out vec4 fragColor;
void main() { fragColor = vec4(vColor, 1.0); }
)glsl";

} // namespace

auto WorldGrid::create(gfx::IDevice& device, const WorldGridDesc& desc) -> gfx::GfxError {
    if (inited_) return gfx::GfxError::None;
    device_ = &device;
    desc_ = desc;

    // UBO: viewProj + model + tint (mat4 + mat4 + vec4 = 144 bytes)
    {
        gfx::BufferDesc d; d.sizeBytes = 144; d.usage = gfx::BufferUsage::Uniform;
        auto r = device.createBuffer(d, {});
        if (!r.hasValue()) return r.error();
        ubo_ = r.value();
    }

    // Shaders
    auto mk = [&](gfx::ShaderStage s, const char* src) {
        gfx::ShaderDesc d; d.stage = s; d.blob.resize(std::strlen(src));
        std::memcpy(d.blob.data(), src, d.blob.size()); return device.createShader(d);
    };
    auto vs = mk(gfx::ShaderStage::Vertex, kVs);
    auto fs = mk(gfx::ShaderStage::Fragment, kFs);
    if (!vs.hasValue() || !fs.hasValue()) return gfx::GfxError::BackendUnavailable;
    vs_ = vs.value();
    fs_ = fs.value();

    // Pipeline
    gfx::PipelineStateDesc pd;
    pd.vertexShader = vs_; pd.fragmentShader = fs_;
    pd.topology = gfx::PrimitiveTopology::TriangleList;
    pd.raster.cullMode = gfx::CullMode::None;   // lines are boxes; draw both sides
    pd.depthStencil.depthTestEnable = true;
    pd.depthStencil.depthWriteEnable = false;   // reference only: never occlude geometry
    pd.depthStencil.depthCompare = gfx::CompareOp::LessEqual;
    {
        gfx::VertexInputLayout::VertexBinding b; b.binding = 0; b.strideBytes = sizeof(Vtx);
        pd.vertexInput.bindings.push(b);
        gfx::VertexInputLayout::VertexAttribute pa;
        pa.binding = 0; pa.location = 0; pa.format = gfx::VertexFormat::Float32x3; pa.offsetBytes = 0;
        pd.vertexInput.attributes.push(pa);
        gfx::VertexInputLayout::VertexAttribute ca;
        ca.binding = 0; ca.location = 1; ca.format = gfx::VertexFormat::Float32x3; ca.offsetBytes = 12;
        pd.vertexInput.attributes.push(ca);
    }
    auto pipe = device.createPipeline(pd);
    if (!pipe.hasValue()) return gfx::GfxError::BackendUnavailable;
    pipeline_ = pipe.value();

    auto err = rebuild(device);
    if (err != gfx::GfxError::None) return err;

    inited_ = true;
    return gfx::GfxError::None;
}

void WorldGrid::destroy(gfx::IDevice& device) {
    if (pipeline_.isValid()) device.destroyPipeline(pipeline_);
    if (fs_.isValid()) device.destroyShader(fs_);
    if (vs_.isValid()) device.destroyShader(vs_);
    if (ubo_.isValid()) device.destroyBuffer(ubo_);
    if (ib_.isValid()) device.destroyBuffer(ib_);
    if (vb_.isValid()) device.destroyBuffer(vb_);
    vb_ = ib_ = ubo_ = gfx::BufferHandle{};
    pipeline_ = gfx::PipelineHandle{};
    vs_ = fs_ = gfx::ShaderHandle{};
    indexCount_ = 0;
    inited_ = false;
    device_ = nullptr;
}

auto WorldGrid::setScale(gfx::IDevice& device, const WorldGridDesc& desc) -> gfx::GfxError {
    if (!inited_) return create(device, desc);
    // cheap: skip if unchanged
    if (desc.extent == desc_.extent && desc.divisions == desc_.divisions &&
        desc.lineThickness == desc_.lineThickness) return gfx::GfxError::None;
    desc_ = desc;
    return rebuild(device);
}

auto WorldGrid::rebuild(gfx::IDevice& device) -> gfx::GfxError {
    std::vector<Vtx> verts;
    std::vector<std::uint16_t> idx;

    auto pushBox = [&](math::Vec3 a, math::Vec3 b, math::Vec3 color, float t) {
        math::Vec3 dir = b - a;
        math::Vec3 arb = (std::abs(dir.y) < 0.99f) ? math::Vec3(0, 1, 0) : math::Vec3(1, 0, 0);
        math::Vec3 u = math::normalized(math::cross(dir, arb));
        math::Vec3 v = math::normalized(math::cross(dir, u));
        int base = (int)verts.size();
        for (int end = 0; end < 2; ++end) {
            const math::Vec3 origin = end == 0 ? a : b;
            for (int si = -1; si <= 1; si += 2) {
                for (int sj = -1; sj <= 1; sj += 2) {
                    math::Vec3 c = origin + u * (t * si) + v * (t * sj);
                    verts.push_back({c.x, c.y, c.z, color.x, color.y, color.z});
                }
            }
        }
        const std::uint16_t f[12][3] = {
            {0,1,3},{0,3,2}, {4,7,5},{4,6,7},
            {0,2,6},{0,6,4}, {1,5,7},{1,7,3},
            {0,4,5},{0,5,1}, {2,3,7},{2,7,6},
        };
        for (auto& t3 : f) {
            idx.push_back((std::uint16_t)(base + t3[0]));
            idx.push_back((std::uint16_t)(base + t3[1]));
            idx.push_back((std::uint16_t)(base + t3[2]));
        }
    };

    const float extent = desc_.extent;
    const float step = 2.0f * extent / (float)desc_.divisions;
    const float t = desc_.lineThickness;
    const float c = desc_.cellBrightness;
    const math::Vec3 cGrid(c, c, c);

    if (desc_.parts & WorldGridPlanes) {
        for (std::uint32_t i = 0; i <= desc_.divisions; ++i) {
            float p = -extent + (float)i * step;
            // XZ (y=0)
            pushBox({p, 0, -extent}, {p, 0, extent}, cGrid, t);
            pushBox({-extent, 0, p}, {extent, 0, p}, cGrid, t);
            // XY (z=0)
            pushBox({p, -extent, 0}, {p, extent, 0}, cGrid, t);
            pushBox({-extent, p, 0}, {extent, p, 0}, cGrid, t);
            // YZ (x=0)
            pushBox({0, -extent, p}, {0, extent, p}, cGrid, t);
            pushBox({0, p, -extent}, {0, p, extent}, cGrid, t);
        }
    }

    if (desc_.parts & WorldGridAxes) {
        const float aT = t * 1.6f;
        pushBox({-extent, 0, 0}, {extent, 0, 0}, math::Vec3(1, 0.15f, 0.15f), aT); // X red
        pushBox({0, -extent, 0}, {0, extent, 0}, math::Vec3(0.15f, 1, 0.15f), aT); // Y green
        pushBox({0, 0, -extent}, {0, 0, extent}, math::Vec3(0.15f, 0.15f, 1), aT); // Z blue
    }

    // upload
    if (vb_.isValid()) device.destroyBuffer(vb_);
    if (ib_.isValid()) device.destroyBuffer(ib_);
    {
        gfx::BufferDesc d; d.sizeBytes = verts.size() * sizeof(Vtx); d.usage = gfx::BufferUsage::Vertex;
        auto r = device.createBuffer(d, std::span(reinterpret_cast<const std::byte*>(verts.data()), verts.size() * sizeof(Vtx)));
        if (!r.hasValue()) return r.error();
        vb_ = r.value();
    }
    {
        gfx::BufferDesc d; d.sizeBytes = idx.size() * sizeof(std::uint16_t); d.usage = gfx::BufferUsage::Index;
        auto r = device.createBuffer(d, std::span(reinterpret_cast<const std::byte*>(idx.data()), idx.size() * sizeof(std::uint16_t)));
        if (!r.hasValue()) return r.error();
        ib_ = r.value();
    }
    indexCount_ = (std::uint32_t)idx.size();
    return gfx::GfxError::None;
}

void WorldGrid::render(gfx::CommandBuffer& cb, const math::Mat4& viewProj) {
    if (!inited_ || device_ == nullptr) return;
    struct Ubo { math::Mat4 viewProj; math::Mat4 model; math::Vec4 tint; };
    Ubo ubo{viewProj, math::Mat4::identity(), math::Vec4(1, 1, 1, 1)};
    device_->writeBuffer(ubo_, 0, std::span(reinterpret_cast<const std::byte*>(&ubo), sizeof(ubo)));
    cb.bindPipeline(pipeline_);
    cb.bindVertexBuffer(0, vb_, sizeof(Vtx));
    cb.bindIndexBuffer(ib_, gfx::IndexFormat::U16);
    cb.bindResource(0, gfx::BindingType::UniformBuffer, ubo_.index, ubo_.generation);
    cb.drawIndexed(indexCount_, 1);
}

} // namespace f4::engine
