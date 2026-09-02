#include "demos/demo_cube.h"

#include <cstring>

#include "core/math/mat4.h"
#include "engine/rendering/shader_constants.h"
#include "graphics/command/command_buffer.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

namespace f4::demo {

namespace {

// Per-face solid colors (stored in the aColor vec2 slot, expanded to rgb by shader).
// Layout: pos(3) + normal(3) + faceColor(2) = 8 floats = 32 bytes per vertex
constexpr float kCubeVerts[] = {
    // Front face (z = -0.5) - RED
    -0.5f, -0.5f, -0.5f,  0.f,  0.f, -1.f,  1.f, 0.f,
     0.5f, -0.5f, -0.5f,  0.f,  0.f, -1.f,  1.f, 0.f,
     0.5f,  0.5f, -0.5f,  0.f,  0.f, -1.f,  1.f, 0.f,
    -0.5f,  0.5f, -0.5f,  0.f,  0.f, -1.f,  0.f, 0.f,
    // Back face (z = 0.5)
    -0.5f, -0.5f,  0.5f,  0.f,  0.f,  1.f,  1.f, 0.f,
     0.5f, -0.5f,  0.5f,  0.f,  0.f,  1.f,  0.f, 1.f,
     0.5f,  0.5f,  0.5f,  0.f,  0.f,  1.f,  0.f, 1.f,
    -0.5f,  0.5f,  0.5f,  0.f,  0.f,  1.f,  0.f, 1.f,
    // Left face (x = -0.5)
    -0.5f, -0.5f, -0.5f, -1.f,  0.f,  0.f,  0.f, 1.f,
    -0.5f, -0.5f,  0.5f, -1.f,  0.f,  0.f,  0.f, 1.f,
    -0.5f,  0.5f,  0.5f, -1.f,  0.f,  0.f,  0.f, 1.f,
    -0.5f,  0.5f, -0.5f, -1.f,  0.f,  0.f,  0.f, 1.f,
    // Right face (x = 0.5)
     0.5f, -0.5f, -0.5f,  1.f,  0.f,  0.f,  1.f, 0.f,
     0.5f, -0.5f,  0.5f,  1.f,  0.f,  0.f,  1.f, 0.f,
     0.5f,  0.5f,  0.5f,  1.f,  0.f,  0.f,  1.f, 0.f,
     0.5f,  0.5f, -0.5f,  1.f,  0.f,  0.f,  1.f, 0.f,
    // Top face (y = 0.5)
    -0.5f,  0.5f, -0.5f,  0.f,  1.f,  0.f,  0.f, 1.f,
     0.5f,  0.5f, -0.5f,  0.f,  1.f,  0.f,  1.f, 0.f,
     0.5f,  0.5f,  0.5f,  0.f,  1.f,  0.f,  1.f, 0.f,
    -0.5f,  0.5f,  0.5f,  0.f,  1.f,  0.f,  0.f, 1.f,
    // Bottom face (y = -0.5)
    -0.5f, -0.5f, -0.5f,  0.f, -1.f,  0.f,  1.f, 0.f,
     0.5f, -0.5f, -0.5f,  0.f, -1.f,  0.f,  1.f, 0.f,
     0.5f, -0.5f,  0.5f,  0.f, -1.f,  0.f,  1.f, 0.f,
    -0.5f, -0.5f,  0.5f,  0.f, -1.f,  0.f,  0.f, 1.f,
};

constexpr std::uint16_t kCubeIndices[] = {
    // Front face (z = -0.5)
    0, 2, 1,  2, 0, 3,
    // Back face (z = +0.5)
    4, 5, 6,  6, 7, 4,
    // Left face (x = -0.5)
    8, 9, 10,  10, 11, 8,
    // Right face (x = 0.5)
    12, 14, 13,  14, 12, 15,
    // Top face (y = 0.5)
    16, 18, 17,  18, 16, 19,
    // Bottom face (y = -0.5)
    20, 21, 22,  22, 23, 20,
};

constexpr const char* kVs = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aColor;   // per-face color (rg), b derived
layout(std140, binding = 0) uniform Ubo {
    mat4 viewProj;
    mat4 model;
    vec3 lightDir;
    vec3 viewPos;
};
out vec3 vNormal;
out vec3 vWorldPos;
out vec3 vAlbedo;
void main() {
    vec4 worldPos = model * vec4(aPos, 1.0);
    gl_Position = viewProj * worldPos;
    vNormal = mat3(transpose(inverse(model))) * aNormal;
    vWorldPos = worldPos.xyz;
    vAlbedo = vec3(aColor.x, aColor.y, 0.5 + 0.4 * sin(aColor.x * 6.28 + aColor.y));
}
)glsl";

constexpr const char* kFs = R"glsl(
#version 430 core
in vec3 vNormal;
in vec3 vWorldPos;
in vec3 vAlbedo;
layout(std140, binding = 0) uniform Ubo {
    mat4 viewProj;
    mat4 model;
    vec3 lightDir;
    vec3 viewPos;
};
out vec4 fragColor;
void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(-lightDir);
    vec3 V = normalize(viewPos - vWorldPos);
    vec3 ambient = vec3(0.45, 0.45, 0.48);
    float diff = max(dot(N, L), 0.0);
    vec3 diffuse = diff * vec3(0.55, 0.45, 0.35);
    vec3 R = reflect(-L, N);
    float spec = pow(max(dot(V, R), 0.0), 24.0);
    vec3 specular = spec * vec3(0.35, 0.35, 0.35);
    vec3 color = vAlbedo * (ambient + diffuse) + specular;
    fragColor = vec4(color, 1.0);
}
)glsl";

} // namespace

bool CubeScene::init(gfx::IDevice* dev, platform::Window*) {
    if (inited_) return true;
    device_ = dev;

    // Vertex buffer
    gfx::BufferDesc vbd;
    vbd.sizeBytes = sizeof(kCubeVerts);
    vbd.usage = gfx::BufferUsage::Vertex;
    auto vb = dev->createBuffer(vbd, std::span(reinterpret_cast<const std::byte*>(kCubeVerts), sizeof(kCubeVerts)));
    if (!vb.hasValue()) return false;
    vb_ = vb.value();

    // Index buffer
    gfx::BufferDesc ibd;
    ibd.sizeBytes = sizeof(kCubeIndices);
    ibd.usage = gfx::BufferUsage::Index;
    auto ib = dev->createBuffer(ibd, std::span(reinterpret_cast<const std::byte*>(kCubeIndices), sizeof(kCubeIndices)));
    if (!ib.hasValue()) return false;
    ib_ = ib.value();

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
    pd.raster.cullMode = gfx::CullMode::Back;
    pd.depthStencil.depthTestEnable = true;
    pd.depthStencil.depthWriteEnable = true;
    pd.depthStencil.depthCompare = gfx::CompareOp::Less;

    {
        gfx::VertexInputLayout::VertexBinding b;
        b.binding = 0;
        b.strideBytes = 32;
        pd.vertexInput.bindings.push(b);
        gfx::VertexInputLayout::VertexAttribute pa;
        pa.binding = 0;
        pa.location = 0;
        pa.format = gfx::VertexFormat::Float32x3;
        pa.offsetBytes = 0;
        pd.vertexInput.attributes.push(pa);
        gfx::VertexInputLayout::VertexAttribute na;
        na.binding = 0;
        na.location = 1;
        na.format = gfx::VertexFormat::Float32x3;
        na.offsetBytes = 12;
        pd.vertexInput.attributes.push(na);
        gfx::VertexInputLayout::VertexAttribute ua;
        ua.binding = 0;
        ua.location = 2;
        ua.format = gfx::VertexFormat::Float32x2;
        ua.offsetBytes = 24;
        pd.vertexInput.attributes.push(ua);
    }
    auto pipe = dev->createPipeline(pd);
    if (!pipe.hasValue()) return false;
    pipeline_ = pipe.value();

    inited_ = true;
    return true;
}

void CubeScene::shutdown(gfx::IDevice* dev) {
    if (!inited_) return;
    if (pipeline_.isValid()) dev->destroyPipeline(pipeline_);
    if (fs_.isValid()) dev->destroyShader(fs_);
    if (vs_.isValid()) dev->destroyShader(vs_);
    if (ib_.isValid()) dev->destroyBuffer(ib_);
    if (vb_.isValid()) dev->destroyBuffer(vb_);
    constRing_.destroy(*dev);
    inited_ = false;
}

void CubeScene::onResize(std::uint32_t, std::uint32_t) {}

void CubeScene::update(float dt, const Uint8*, const SDL_MouseMotionEvent*) {
    if (!inited_) return;
    angle_ += dt * 0.7f;
    if (angle_ > 6.283185f) angle_ -= 6.283185f;
}

void CubeScene::render(gfx::CommandBuffer& cb, const FreeCamera& cam, std::uint32_t width, std::uint32_t height) {
    if (!inited_) return;
    constRing_.beginFrame();

    float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.f;
    math::Mat4 viewProj = cam.viewProjMatrix(aspect);

    struct UboData {
        math::Mat4 viewProj;
        math::Mat4 model;
        math::Vec3 lightDir;
        float pad1;
        math::Vec3 viewPos;
        float pad2;
    };
    const math::Mat4 model = math::Mat4::rotationY(angle_)
                                 .mul(math::Mat4::rotationX(angle_ * 0.7f))
                                 .mul(math::Mat4::scaling({2.5f, 2.5f, 2.5f}));
    UboData ubo{viewProj, model, math::normalized(math::Vec3(0.5f, -1.f, 0.3f)),
                0, cam.position, 0};

    auto slot = constRing_.writeAndBind(*device_, cb, std::span(reinterpret_cast<const std::byte*>(&ubo), sizeof(ubo)));
    if (slot.valid) {
        cb.bindPipeline(pipeline_);
        cb.bindVertexBuffer(0, vb_, 32);
        cb.bindIndexBuffer(ib_, gfx::IndexFormat::U16);
        cb.bindResource(0, gfx::BindingType::UniformBuffer, slot.handle.index, slot.handle.generation);
        cb.drawIndexed(36, 1);
    }
}

} // namespace f4::demo
