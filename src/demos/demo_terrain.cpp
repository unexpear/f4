#include "demos/demo_terrain.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "core/math/mat4.h"
#include "graphics/command/command_buffer.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

namespace f4::demo {

namespace {

// Grid ground plane
constexpr float kGridMax = 10.f;
constexpr int kGridLines = 20;

constexpr const char* kVs = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(std140, binding = 0) uniform Ubo {
    mat4 viewProj;
    mat4 model;
    vec3 lightDir;
    vec3 viewPos;
};
out vec3 vColor;
out vec3 vNormal;
out vec3 vWorldPos;
void main() {
    vec4 worldPos = model * vec4(aPos, 1.0);
    gl_Position = viewProj * worldPos;
    vColor = aColor;
    vNormal = vec3(0.0, 1.0, 0.0);
    vWorldPos = worldPos.xyz;
}
)glsl";

constexpr const char* kFs = R"glsl(
#version 430 core
in vec3 vColor;
in vec3 vNormal;
in vec3 vWorldPos;
layout(std140, binding = 0) uniform Ubo {
    mat4 viewProj;
    mat4 model;
    vec3 lightDir;
    vec3 viewPos;
};
out vec4 fragColor;
void main() {
    vec3 N = vNormal;
    vec3 L = normalize(-lightDir);
    vec3 V = normalize(viewPos - vWorldPos);

    float diff = max(dot(N, L), 0.0);
    vec3 ambient = vec3(0.1);
    vec3 diffuse = diff * vec3(0.7);

    vec3 R = reflect(-L, N);
    float spec = pow(max(dot(V, R), 0.0), 16.0);
    vec3 specular = spec * vec3(0.2);

    vec3 color = vColor * (ambient + diffuse) + specular;
    fragColor = vec4(color, 1.0);
}
)glsl";

} // namespace

bool TerrainScene::init(gfx::IDevice* dev, platform::Window*) {
    if (inited_) return true;
    device_ = dev;

    // Build grid geometry (lines as quads for ground)
    std::vector<float> gridVerts;
    std::vector<std::uint16_t> gridIndices;
    gridVerts.reserve((kGridLines + 1) * (kGridLines + 1) * 6);
    gridIndices.reserve(kGridLines * kGridLines * 6);

    for (int z = 0; z <= kGridLines; ++z) {
        for (int x = 0; x <= kGridLines; ++x) {
            float wx = (x - kGridLines * 0.5f) * (2.f * kGridMax / kGridLines);
            float wz = (z - kGridLines * 0.5f) * (2.f * kGridMax / kGridLines);
            float c = ((x + z) % 2 == 0) ? 0.35f : 0.25f;
            gridVerts.push_back(wx); gridVerts.push_back(0.f); gridVerts.push_back(wz);
            gridVerts.push_back(c); gridVerts.push_back(c); gridVerts.push_back(c);
        }
    }
    for (int gz = 0; gz < kGridLines; ++gz) {
        for (int gx = 0; gx < kGridLines; ++gx) {
            int a = gz * (kGridLines + 1) + gx;
            int b = a + 1;
            int c = a + (kGridLines + 1);
            int d = c + 1;
            gridIndices.push_back(a); gridIndices.push_back(b); gridIndices.push_back(d);
            gridIndices.push_back(d); gridIndices.push_back(c); gridIndices.push_back(a);
        }
    }

    // Grid vertex buffer
    gfx::BufferDesc vbd;
    vbd.sizeBytes = static_cast<std::uint32_t>(gridVerts.size() * sizeof(float));
    vbd.usage = gfx::BufferUsage::Vertex;
    auto vb = dev->createBuffer(vbd, std::span(reinterpret_cast<const std::byte*>(gridVerts.data()), gridVerts.size() * sizeof(float)));
    if (!vb.hasValue()) return false;
    vbGrid_ = vb.value();

    // Grid index buffer
    gfx::BufferDesc ibd;
    ibd.sizeBytes = static_cast<std::uint32_t>(gridIndices.size() * sizeof(std::uint16_t));
    ibd.usage = gfx::BufferUsage::Index;
    auto ib = dev->createBuffer(ibd, std::span(reinterpret_cast<const std::byte*>(gridIndices.data()), gridIndices.size() * sizeof(std::uint16_t)));
    if (!ib.hasValue()) return false;
    ibGrid_ = ib.value();

    // Simple cube for objects on terrain (position, color)
    constexpr float kCube[] = {
        // Front
        -0.5f, -0.5f, -0.5f, 1,1,1,
         0.5f, -0.5f, -0.5f, 1,1,1,
         0.5f,  0.5f, -0.5f, 1,1,1,
        -0.5f,  0.5f, -0.5f, 1,1,1,
        // Back
        -0.5f, -0.5f,  0.5f, 1,1,1,
         0.5f, -0.5f,  0.5f, 1,1,1,
         0.5f,  0.5f,  0.5f, 1,1,1,
        -0.5f,  0.5f,  0.5f, 1,1,1,
        // Left
        -0.5f, -0.5f, -0.5f, 1,1,1,
        -0.5f, -0.5f,  0.5f, 1,1,1,
        -0.5f,  0.5f,  0.5f, 1,1,1,
        -0.5f,  0.5f, -0.5f, 1,1,1,
        // Right
         0.5f, -0.5f, -0.5f, 1,1,1,
         0.5f, -0.5f,  0.5f, 1,1,1,
         0.5f,  0.5f,  0.5f, 1,1,1,
         0.5f,  0.5f, -0.5f, 1,1,1,
        // Top
        -0.5f,  0.5f, -0.5f, 1,1,1,
         0.5f,  0.5f, -0.5f, 1,1,1,
         0.5f,  0.5f,  0.5f, 1,1,1,
        -0.5f,  0.5f,  0.5f, 1,1,1,
        // Bottom
        -0.5f, -0.5f, -0.5f, 1,1,1,
         0.5f, -0.5f, -0.5f, 1,1,1,
         0.5f, -0.5f,  0.5f, 1,1,1,
        -0.5f, -0.5f,  0.5f, 1,1,1,
    };
    constexpr std::uint16_t kCubeIdx[] = {
        0,1,2, 2,3,0, 4,5,6, 6,7,4, 8,9,10, 10,11,8,
        12,13,14, 14,15,12, 16,17,18, 18,19,16, 20,21,22, 22,23,20
    };

    gfx::BufferDesc cvbd;
    cvbd.sizeBytes = sizeof(kCube);
    cvbd.usage = gfx::BufferUsage::Vertex;
    auto cvb = dev->createBuffer(cvbd, std::span(reinterpret_cast<const std::byte*>(kCube), sizeof(kCube)));
    if (!cvb.hasValue()) return false;
    vbCube_ = cvb.value();

    gfx::BufferDesc cibd;
    cibd.sizeBytes = sizeof(kCubeIdx);
    cibd.usage = gfx::BufferUsage::Index;
    auto cib = dev->createBuffer(cibd, std::span(reinterpret_cast<const std::byte*>(kCubeIdx), sizeof(kCubeIdx)));
    if (!cib.hasValue()) return false;
    ibCube_ = cib.value();

    // Per-object constant ring (std140: mat4 + mat4 + vec4 + vec3 + padding = 144, pad to 160)
    if (constRing_.create(*dev, 160, 16) != gfx::GfxError::None) return false;

    // Shaders
    auto makeShader = [&](gfx::ShaderStage stage, const char* src) {
        gfx::ShaderDesc d;
        d.stage = stage;
        d.blob.resize(std::strlen(src));
        std::memcpy(d.blob.data(), src, d.blob.size());
        return dev->createShader(d);
    };
    auto vs = makeShader(gfx::ShaderStage::Vertex, kVs);
    auto fs = makeShader(gfx::ShaderStage::Fragment, kFs);
    if (!vs.hasValue() || !fs.hasValue()) return false;
    vs_ = vs.value();
    fs_ = fs.value();

    // Pipeline
    gfx::PipelineStateDesc pd;
    pd.vertexShader = vs_;
    pd.fragmentShader = fs_;
    pd.topology = gfx::PrimitiveTopology::TriangleList;
    pd.raster.cullMode = gfx::CullMode::None;
    pd.depthStencil.depthTestEnable = true;
    pd.depthStencil.depthWriteEnable = true;
    pd.depthStencil.depthCompare = gfx::CompareOp::Less;
    {
        gfx::VertexInputLayout::VertexBinding b;
        b.binding = 0;
        b.strideBytes = 24;
        pd.vertexInput.bindings.push(b);
        gfx::VertexInputLayout::VertexAttribute pa;
        pa.binding = 0; pa.location = 0; pa.format = gfx::VertexFormat::Float32x3; pa.offsetBytes = 0;
        pd.vertexInput.attributes.push(pa);
        gfx::VertexInputLayout::VertexAttribute ca;
        ca.binding = 0; ca.location = 1; ca.format = gfx::VertexFormat::Float32x3; ca.offsetBytes = 12;
        pd.vertexInput.attributes.push(ca);
    }
    auto pipe = dev->createPipeline(pd);
    if (!pipe.hasValue()) return false;
    pipeline_ = pipe.value();

    inited_ = true;
    return true;
}

void TerrainScene::shutdown(gfx::IDevice* dev) {
    if (!inited_) return;
    if (pipeline_.isValid()) dev->destroyPipeline(pipeline_);
    if (fs_.isValid()) dev->destroyShader(fs_);
    if (vs_.isValid()) dev->destroyShader(vs_);
    if (ibCube_.isValid()) dev->destroyBuffer(ibCube_);
    if (vbCube_.isValid()) dev->destroyBuffer(vbCube_);
    if (ibGrid_.isValid()) dev->destroyBuffer(ibGrid_);
    if (vbGrid_.isValid()) dev->destroyBuffer(vbGrid_);
    constRing_.destroy(*dev);
    inited_ = false;
}

void TerrainScene::onResize(std::uint32_t, std::uint32_t) {}

void TerrainScene::update(float dt, const Uint8*, const SDL_MouseMotionEvent*) {
    time_ += dt;
}

void TerrainScene::render(gfx::CommandBuffer& cb, const FreeCamera& camera, std::uint32_t width, std::uint32_t height) {
    if (!inited_) return;
    constRing_.beginFrame();

    float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.f;
    math::Mat4 viewProj = camera.viewProjMatrix(aspect);
    math::Vec3 lightDir = math::normalized(math::Vec3(0.3f, -1.f, 0.4f));
    math::Vec3 viewPos = camera.position;

    struct GridUboData {
        math::Mat4 viewProj;
        math::Mat4 model;
        math::Vec3 lightDir;
        float pad1;
        math::Vec3 viewPos;
        float pad2;
    };

    // Render grid (ground plane)
    {
        struct CubeUboData {
        math::Mat4 viewProj;
        math::Mat4 model;
        math::Vec3 lightDir;
        float pad1;
        math::Vec3 viewPos;
        float pad2;
    };
        GridUboData ubo;
        ubo.viewProj = viewProj;
        ubo.model = math::Mat4::identity();
        ubo.lightDir = lightDir; ubo.pad1 = 0;
        ubo.viewPos = viewPos; ubo.pad2 = 0;
        auto slot = constRing_.writeAndBind(*device_, cb, std::span(reinterpret_cast<const std::byte*>(&ubo), sizeof(ubo)));
        if (!slot.valid) return;
        cb.bindPipeline(pipeline_);
        cb.bindVertexBuffer(0, vbGrid_, 24);
        cb.bindIndexBuffer(ibGrid_, gfx::IndexFormat::U16);
        cb.bindResource(0, gfx::BindingType::UniformBuffer, slot.handle.index, slot.handle.generation);
        cb.drawIndexed(static_cast<std::uint32_t>(kGridLines * kGridLines * 6), 1);
    }

    // Render animated cubes on the grid
    cb.bindVertexBuffer(0, vbCube_, 24);
    cb.bindIndexBuffer(ibCube_, gfx::IndexFormat::U16);

    for (int i = 0; i < 8; ++i) {
        float angle = time_ * 0.5f + i * 0.785f;
        float radius = 3.f + (i % 3) * 1.5f;
        float x = std::cos(angle) * radius;
        float z = std::sin(angle) * radius;
        float y = 0.5f + std::sin(time_ * 2.f + i) * 0.3f;

        struct CubeUboData {
            math::Mat4 viewProj;
            math::Mat4 model;
            math::Vec3 lightDir;
            float pad1;
            math::Vec3 viewPos;
            float pad2;
        };
        CubeUboData ubo;
        ubo.viewProj = viewProj;
        ubo.model = math::Mat4::translation({x, y, z}).mul(math::Mat4::rotationY(time_ + i));
        ubo.lightDir = lightDir; ubo.pad1 = 0;
        ubo.viewPos = viewPos; ubo.pad2 = 0;
        auto slot = constRing_.writeAndBind(*device_, cb, std::span(reinterpret_cast<const std::byte*>(&ubo), sizeof(ubo)));
        if (!slot.valid) return;
        cb.drawIndexed(36, 1);
    }
}

} // namespace f4::demo
