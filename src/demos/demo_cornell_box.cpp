#include "demos/demo_cornell_box.h"

#include <cstring>
#include <span>
#include <vector>

#include "core/math/mat4.h"
#include "core/math/vec3.h"
#include "graphics/buffer/buffer_desc.h"
#include "graphics/command/command_buffer.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"

namespace f4::demo {
namespace {

struct Vertex {
    math::Vec3 position;
    math::Vec3 normal;
    float color[4];
};

constexpr const char* kVs = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aColor;
layout(std140, binding = 0) uniform Ubo {
    mat4 viewProj;
    mat4 model;
    vec3 lightDir;
    vec3 viewPos;
};
out vec3 vNormal;
out vec3 vWorldPos;
out vec4 vColor;
void main() {
    vec4 worldPos = model * vec4(aPos, 1.0);
    gl_Position = viewProj * worldPos;
    vNormal = mat3(transpose(inverse(model))) * aNormal;
    vWorldPos = worldPos.xyz;
    vColor = aColor;
}
)glsl";

constexpr const char* kFs = R"glsl(
#version 430 core
in vec3 vNormal;
in vec3 vWorldPos;
in vec4 vColor;
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
    float diffuse = max(dot(N, L), 0.0);
    vec3 H = normalize(L + V);
    float specular = pow(max(dot(N, H), 0.0), 32.0);
    vec3 color = vColor.rgb * (0.18 + 0.82 * diffuse) + vec3(0.18 * specular);
    fragColor = vec4(color, vColor.a);
}
)glsl";

void addQuad(std::vector<Vertex>& vertices, std::vector<std::uint16_t>& indices,
             const math::Vec3& a, const math::Vec3& b, const math::Vec3& c,
             const math::Vec3& d, const math::Vec3& normal, const float (&color)[4]) {
    const auto base = static_cast<std::uint16_t>(vertices.size());
    vertices.push_back({a, normal, {color[0], color[1], color[2], color[3]}});
    vertices.push_back({b, normal, {color[0], color[1], color[2], color[3]}});
    vertices.push_back({c, normal, {color[0], color[1], color[2], color[3]}});
    vertices.push_back({d, normal, {color[0], color[1], color[2], color[3]}});
    indices.insert(indices.end(), {base, static_cast<std::uint16_t>(base + 1),
                                    static_cast<std::uint16_t>(base + 2),
                                    static_cast<std::uint16_t>(base + 2),
                                    static_cast<std::uint16_t>(base + 3), base});
}

void addBox(std::vector<Vertex>& vertices, std::vector<std::uint16_t>& indices,
            const math::Vec3& lo, const math::Vec3& hi, const float (&color)[4]) {
    addQuad(vertices, indices, {lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z},
            {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z}, {-1.f, 0.f, 0.f}, color);
    addQuad(vertices, indices, {hi.x, lo.y, hi.z}, {hi.x, lo.y, lo.z},
            {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {1.f, 0.f, 0.f}, color);
    // Floor-standing boxes omit their hidden bottom to avoid coplanar depth fighting.
    addQuad(vertices, indices, {lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z},
            {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}, {0.f, 1.f, 0.f}, color);
    addQuad(vertices, indices, {hi.x, lo.y, lo.z}, {lo.x, lo.y, lo.z},
            {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {0.f, 0.f, -1.f}, color);
    addQuad(vertices, indices, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z},
            {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}, {0.f, 0.f, 1.f}, color);
}

} // namespace

bool CornellBoxScene::init(gfx::IDevice* dev, platform::Window*) {
    if (inited_) return true;
    device_ = dev;

    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
    const float white[4] = {0.78f, 0.75f, 0.68f, 1.f};
    const float red[4] = {0.72f, 0.08f, 0.06f, 1.f};
    const float green[4] = {0.08f, 0.52f, 0.16f, 1.f};
    const float boxWhite[4] = {0.72f, 0.70f, 0.64f, 1.f};
    addQuad(vertices, indices, {-5.f, 0.f, 5.f}, {5.f, 0.f, 5.f},
            {5.f, 0.f, -5.f}, {-5.f, 0.f, -5.f}, {0.f, 1.f, 0.f}, white);
    addQuad(vertices, indices, {-5.f, 6.f, -5.f}, {5.f, 6.f, -5.f},
            {5.f, 6.f, 5.f}, {-5.f, 6.f, 5.f}, {0.f, -1.f, 0.f}, white);
    addQuad(vertices, indices, {-5.f, 0.f, -5.f}, {5.f, 0.f, -5.f},
            {5.f, 6.f, -5.f}, {-5.f, 6.f, -5.f}, {0.f, 0.f, 1.f}, white);
    addQuad(vertices, indices, {-5.f, 0.f, -5.f}, {-5.f, 6.f, -5.f},
            {-5.f, 6.f, 5.f}, {-5.f, 0.f, 5.f}, {1.f, 0.f, 0.f}, red);
    addQuad(vertices, indices, {5.f, 0.f, 5.f}, {5.f, 6.f, 5.f},
            {5.f, 6.f, -5.f}, {5.f, 0.f, -5.f}, {-1.f, 0.f, 0.f}, green);
    addBox(vertices, indices, {-3.2f, 0.f, -2.8f}, {-0.7f, 2.2f, 0.2f}, boxWhite);
    addBox(vertices, indices, {0.8f, 0.f, -3.8f}, {3.2f, 3.6f, -1.1f}, boxWhite);
    indexCount_ = static_cast<std::uint32_t>(indices.size());

    gfx::BufferDesc vertexDesc;
    vertexDesc.sizeBytes = vertices.size() * sizeof(Vertex);
    vertexDesc.usage = gfx::BufferUsage::Vertex;
    auto vertexBuffer = dev->createBuffer(vertexDesc, std::as_bytes(std::span(vertices)));
    if (!vertexBuffer.hasValue()) return false;
    vb_ = vertexBuffer.value();
    gfx::BufferDesc indexDesc;
    indexDesc.sizeBytes = indices.size() * sizeof(std::uint16_t);
    indexDesc.usage = gfx::BufferUsage::Index;
    auto indexBuffer = dev->createBuffer(indexDesc, std::as_bytes(std::span(indices)));
    if (!indexBuffer.hasValue()) return false;
    ib_ = indexBuffer.value();
    if (constRing_.create(*dev, 160, 1) != gfx::GfxError::None) return false;

    auto makeShader = [&](gfx::ShaderStage stage, const char* source) {
        gfx::ShaderDesc desc;
        desc.stage = stage;
        desc.blob.resize(std::strlen(source));
        std::memcpy(desc.blob.data(), source, desc.blob.size());
        return dev->createShader(desc);
    };
    auto vertexShader = makeShader(gfx::ShaderStage::Vertex, kVs);
    auto fragmentShader = makeShader(gfx::ShaderStage::Fragment, kFs);
    if (!vertexShader.hasValue() || !fragmentShader.hasValue()) return false;
    vs_ = vertexShader.value();
    fs_ = fragmentShader.value();

    gfx::PipelineStateDesc pipelineDesc;
    pipelineDesc.vertexShader = vs_;
    pipelineDesc.fragmentShader = fs_;
    pipelineDesc.raster.cullMode = gfx::CullMode::None;
    pipelineDesc.depthStencil.depthTestEnable = true;
    pipelineDesc.depthStencil.depthWriteEnable = true;
    pipelineDesc.depthStencil.depthCompare = gfx::CompareOp::Less;
    pipelineDesc.vertexInput.bindings.push({0, sizeof(Vertex), 0});
    pipelineDesc.vertexInput.attributes.push({0, 0, gfx::VertexFormat::Float32x3, 0});
    pipelineDesc.vertexInput.attributes.push({0, 1, gfx::VertexFormat::Float32x3, 12});
    pipelineDesc.vertexInput.attributes.push({0, 2, gfx::VertexFormat::Float32x4, 24});
    auto pipeline = dev->createPipeline(pipelineDesc);
    if (!pipeline.hasValue()) return false;
    pipeline_ = pipeline.value();
    inited_ = true;
    return true;
}

void CornellBoxScene::shutdown(gfx::IDevice* dev) {
    if (pipeline_.isValid()) dev->destroyPipeline(pipeline_);
    if (fs_.isValid()) dev->destroyShader(fs_);
    if (vs_.isValid()) dev->destroyShader(vs_);
    if (ib_.isValid()) dev->destroyBuffer(ib_);
    if (vb_.isValid()) dev->destroyBuffer(vb_);
    constRing_.destroy(*dev);
    pipeline_ = {};
    fs_ = {};
    vs_ = {};
    ib_ = {};
    vb_ = {};
    inited_ = false;
}

void CornellBoxScene::onResize(std::uint32_t, std::uint32_t) {}
void CornellBoxScene::update(float, const Uint8*, const SDL_MouseMotionEvent*) {}

void CornellBoxScene::render(gfx::CommandBuffer& cb, const FreeCamera& camera,
                             std::uint32_t width, std::uint32_t height) {
    if (!inited_) return;
    constRing_.beginFrame();
    const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.f;
    struct UboData {
        math::Mat4 viewProj;
        math::Mat4 model;
        math::Vec3 lightDir;
        float lightPad;
        math::Vec3 viewPos;
        float viewPad;
    };
    const UboData ubo{camera.viewProjMatrix(aspect), math::Mat4::identity(),
                      math::normalized(math::Vec3(0.35f, -1.f, 0.25f)), 0.f,
                      camera.position, 0.f};
    const auto slot = constRing_.writeAndBind(
        *device_, cb, std::as_bytes(std::span(&ubo, std::size_t{1})));
    if (!slot.valid) return;
    cb.bindPipeline(pipeline_);
    cb.bindVertexBuffer(0, vb_, sizeof(Vertex));
    cb.bindIndexBuffer(ib_, gfx::IndexFormat::U16);
    cb.bindResource(0, gfx::BindingType::UniformBuffer, slot.handle.index, slot.handle.generation);
    cb.drawIndexed(indexCount_, 1);
}

} // namespace f4::demo
