#include "demos/demo_render_validation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <span>
#include <vector>

#include "core/math/mat4.h"
#include "core/math/vec3.h"
#include "graphics/buffer/buffer_desc.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/sampler/sampler_desc.h"
#include "graphics/shader/shader_desc.h"
#include "graphics/texture/texture_desc.h"
#ifdef F4_GRAPHICS_BACKEND_metal
#include "demos/metal_demo_shaders.h"
#define F4_VALIDATION_SHADER(glslName, wgslName) f4::metal::wgslName
#elif defined(F4_GRAPHICS_BACKEND_webgpu)
#include "webgpu_shaders.h"
#define F4_VALIDATION_SHADER(glslName, wgslName) f4::web::wgslName
#else
#define F4_VALIDATION_SHADER(glslName, wgslName) glslName
#endif

namespace f4::demo {
namespace {

constexpr float kPi = 3.14159265358979323846f;

auto createShader(gfx::IDevice& device, gfx::ShaderStage stage, const char* source,
                  gfx::ShaderHandle& output) -> bool {
    gfx::ShaderDesc desc;
    desc.stage = stage;
#ifdef F4_GRAPHICS_BACKEND_metal
    std::ifstream stream(source, std::ios::binary | std::ios::ate);
    if (!stream.good()) return false;
    const std::streamsize size = stream.tellg();
    if (size <= 0) return false;
    stream.seekg(0);
    desc.blob.resize(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(desc.blob.data()), size);
    if (!stream.good()) return false;
#else
    const std::size_t length = std::strlen(source);
    desc.blob.resize(length);
    std::memcpy(desc.blob.data(), source, length);
#endif
    const auto result = device.createShader(desc);
    if (!result.hasValue()) return false;
    output = result.value();
    return true;
}

auto createBuffer(gfx::IDevice& device, gfx::BufferUsage usage,
                  std::span<const std::byte> bytes, gfx::BufferHandle& output) -> bool {
    gfx::BufferDesc desc;
    desc.sizeBytes = bytes.size();
    desc.usage = usage;
    const auto result = device.createBuffer(desc, bytes);
    if (!result.hasValue()) return false;
    output = result.value();
    return true;
}

auto groupZero(gfx::PipelineStateDesc& pipeline) -> gfx::BindGroup& {
    if (pipeline.layout.groups.isEmpty()) pipeline.layout.groups.push(gfx::BindGroup{});
    return pipeline.layout.groups[0];
}

void addUniform(gfx::PipelineStateDesc& pipeline, std::uint32_t stages) {
    groupZero(pipeline).slots.push({0, gfx::BindingType::UniformBuffer, stages, 1});
}

void addSampledTexture(gfx::PipelineStateDesc& pipeline, std::uint32_t slot) {
    auto& group = groupZero(pipeline);
    group.slots.push({slot, gfx::BindingType::Texture,
                      gfx::shaderStageBit(gfx::ShaderStage::Fragment), 1});
    group.slots.push({slot, gfx::BindingType::Sampler,
                      gfx::shaderStageBit(gfx::ShaderStage::Fragment), 1});
}

template <typename T>
auto uploadVector(gfx::IDevice& device, gfx::BufferUsage usage,
                  const std::vector<T>& values, gfx::BufferHandle& output) -> bool {
    return createBuffer(device, usage, std::as_bytes(std::span(values)), output);
}

struct SphereVertex {
    float position[3];
    float normal[3];
};

void buildSphere(std::vector<SphereVertex>& vertices, std::vector<std::uint16_t>& indices) {
    constexpr std::uint32_t slices = 32;
    constexpr std::uint32_t stacks = 20;
    vertices.reserve((slices + 1) * (stacks + 1));
    for (std::uint32_t stack = 0; stack <= stacks; ++stack) {
        const float theta = kPi * static_cast<float>(stack) / static_cast<float>(stacks);
        const float ring = std::sin(theta);
        const float y = std::cos(theta);
        for (std::uint32_t slice = 0; slice <= slices; ++slice) {
            const float phi = 2.0f * kPi * static_cast<float>(slice) /
                              static_cast<float>(slices);
            const float x = ring * std::cos(phi);
            const float z = ring * std::sin(phi);
            vertices.push_back({{x * 0.48f, y * 0.48f, z * 0.48f}, {x, y, z}});
        }
    }
    for (std::uint32_t stack = 0; stack < stacks; ++stack) {
        for (std::uint32_t slice = 0; slice < slices; ++slice) {
            const auto a = static_cast<std::uint16_t>(stack * (slices + 1) + slice);
            const auto b = static_cast<std::uint16_t>((stack + 1) * (slices + 1) + slice);
            const auto c = static_cast<std::uint16_t>(b + 1);
            const auto d = static_cast<std::uint16_t>(a + 1);
            indices.insert(indices.end(), {a, c, b, a, d, c});
        }
    }
}

[[maybe_unused]] constexpr const char* kPbrVertex = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aOffset;
layout(location = 3) in vec3 aMaterial;
layout(std140, binding = 0) uniform Scene {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightPos;
};
out vec3 vWorldPos;
out vec3 vNormal;
out vec3 vMaterial;
void main() {
    vWorldPos = aPos + aOffset;
    vNormal = aNormal;
    vMaterial = aMaterial;
    gl_Position = viewProj * vec4(vWorldPos, 1.0);
}
)glsl";

[[maybe_unused]] constexpr const char* kPbrFragment = R"glsl(
#version 430 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec3 vMaterial;
layout(std140, binding = 0) uniform Scene {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightPos;
};
out vec4 fragColor;
const float PI = 3.14159265359;
float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float nDotH = max(dot(N, H), 0.0);
    float denominator = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denominator * denominator, 0.0001);
}
float geometrySchlickGGX(float nDotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}
vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - cosTheta, 5.0);
}
void main() {
    float metallic = clamp(vMaterial.x, 0.0, 1.0);
    float roughness = clamp(vMaterial.y, 0.06, 1.0);
    vec3 albedo = mix(vec3(0.72, 0.08, 0.035), vec3(0.95, 0.58, 0.12), vMaterial.z);
    vec3 N = normalize(vNormal);
    vec3 V = normalize(cameraPos.xyz - vWorldPos);
    vec3 toLight = lightPos.xyz - vWorldPos;
    float distanceSquared = max(dot(toLight, toLight), 0.01);
    vec3 L = normalize(toLight);
    vec3 H = normalize(V + L);
    vec3 radiance = vec3(135.0) / distanceSquared;
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), f0);
    float D = distributionGGX(N, H, roughness);
    float G = geometrySchlickGGX(max(dot(N, V), 0.0), roughness) *
              geometrySchlickGGX(max(dot(N, L), 0.0), roughness);
    vec3 specular = (D * G * F) /
                    max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 0.001);
    vec3 diffuse = (vec3(1.0) - F) * (1.0 - metallic) * albedo / PI;
    vec3 color = vec3(0.025) * albedo +
                 (diffuse + specular) * radiance * max(dot(N, L), 0.0);
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));
    fragColor = vec4(color, 1.0);
}
)glsl";

[[maybe_unused]] constexpr const char* kFullscreenVertex = R"glsl(
#version 430 core
out vec2 vUv;
void main() {
    vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 p = positions[gl_VertexID];
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)glsl";

[[maybe_unused]] constexpr const char* kMipFragment = R"glsl(
#version 430 core
in vec2 vUv;
uniform sampler2D uTex;
uniform sampler2D uTex1;
out vec4 fragColor;
void main() {
    bool mipmapped = vUv.x >= 0.5;
    float localX = fract(vUv.x * 2.0) * 2.0 - 1.0;
    if (vUv.y > 0.86) {
        vec3 marker = mipmapped ? vec3(0.08, 0.65, 0.32) : vec3(0.85, 0.18, 0.08);
        fragColor = vec4(marker, 1.0);
        return;
    }
    float horizonDistance = max(0.875 - vUv.y, 0.012);
    float perspective = 1.0 / horizonDistance;
    vec2 checkerUv = vec2(localX * perspective * 2.0, perspective * 3.0);
    vec3 sampled = mipmapped ? texture(uTex1, checkerUv).rgb : texture(uTex, checkerUv).rgb;
    float fog = smoothstep(8.0, 65.0, perspective);
    vec3 color = mix(sampled, vec3(0.16, 0.19, 0.22), fog * 0.55);
    if (abs(vUv.x - 0.5) < 0.003) color = vec3(0.95);
    fragColor = vec4(color, 1.0);
}
)glsl";

struct ColorVertex {
    float position[3];
    float color[4];
};

void appendWorldQuad(std::vector<ColorVertex>& output, float left, float right,
                     float nearZ, float farZ, float y, const std::array<float, 4>& color) {
    const ColorVertex a{{left, y, nearZ}, {color[0], color[1], color[2], color[3]}};
    const ColorVertex b{{right, y, nearZ}, {color[0], color[1], color[2], color[3]}};
    const ColorVertex c{{right, y, farZ}, {color[0], color[1], color[2], color[3]}};
    const ColorVertex d{{left, y, farZ}, {color[0], color[1], color[2], color[3]}};
    output.insert(output.end(), {a, c, b, a, d, c});
}

[[maybe_unused]] constexpr const char* kWorldColorVertex = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(std140, binding = 0) uniform Scene { mat4 viewProj; };
out vec4 vColor;
void main() {
    gl_Position = viewProj * vec4(aPos, 1.0);
    vColor = aColor;
}
)glsl";

[[maybe_unused]] constexpr const char* kVertexColorFragment = R"glsl(
#version 430 core
in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)glsl";

struct ScreenVertex {
    float position[2];
    float color[4];
};

void appendScreenQuad(std::vector<ScreenVertex>& output, float centerX, float centerY,
                      float width, float height, const std::array<float, 4>& color) {
    const float left = centerX - width * 0.5f;
    const float right = centerX + width * 0.5f;
    const float bottom = centerY - height * 0.5f;
    const float top = centerY + height * 0.5f;
    const ScreenVertex a{{left, bottom}, {color[0], color[1], color[2], color[3]}};
    const ScreenVertex b{{right, bottom}, {color[0], color[1], color[2], color[3]}};
    const ScreenVertex c{{right, top}, {color[0], color[1], color[2], color[3]}};
    const ScreenVertex d{{left, top}, {color[0], color[1], color[2], color[3]}};
    output.insert(output.end(), {a, b, c, a, c, d});
}

[[maybe_unused]] constexpr const char* kScreenColorVertex = R"glsl(
#version 430 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
out vec4 vColor;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vColor = aColor;
}
)glsl";

[[maybe_unused]] constexpr const char* kNormalMapFragment = R"glsl(
#version 430 core
in vec2 vUv;
uniform sampler2D uTex;
out vec4 fragColor;
void main() {
    bool flipped = vUv.x >= 0.5;
    vec2 panelUv = vec2(fract(vUv.x * 2.0), vUv.y);
    vec2 tiledUv = panelUv * vec2(4.0, 4.0);
    vec3 tangentNormal = texture(uTex, tiledUv).xyz * 2.0 - 1.0;
    if (flipped) tangentNormal.y = -tangentNormal.y;
    vec3 N = normalize(tangentNormal);
    vec3 L = normalize(vec3(-0.45, 0.75, 1.0));
    vec3 V = vec3(0.0, 0.0, 1.0);
    float diffuse = max(dot(N, L), 0.0);
    float specular = pow(max(dot(reflect(-L, N), V), 0.0), 32.0);
    vec3 base = mix(vec3(0.07, 0.24, 0.42), vec3(0.12, 0.55, 0.68),
                    0.5 + 0.5 * sin(tiledUv.x * 3.14159));
    vec3 color = base * (0.16 + 0.84 * diffuse) + vec3(0.7) * specular;
    if (vUv.y > 0.92) color = flipped ? vec3(0.86, 0.2, 0.1) : vec3(0.08, 0.68, 0.35);
    if (abs(vUv.x - 0.5) < 0.003) color = vec3(0.95);
    fragColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
)glsl";

} // namespace

bool PbrSphereGridScene::init(gfx::IDevice* device, platform::Window*) {
    if (initialized_) return true;
    device_ = device;
    std::vector<SphereVertex> vertices;
    std::vector<std::uint16_t> indices;
    buildSphere(vertices, indices);
    indexCount_ = static_cast<std::uint32_t>(indices.size());
    if (!uploadVector(*device, gfx::BufferUsage::Vertex, vertices, vertexBuffer_) ||
        !uploadVector(*device, gfx::BufferUsage::Index, indices, indexBuffer_)) return false;

    struct Instance {
        float offset[3];
        float material[3];
    };
    std::vector<Instance> instances;
    for (std::uint32_t row = 0; row < 5; ++row) {
        for (std::uint32_t column = 0; column < 6; ++column) {
            const float metallic = static_cast<float>(row) / 4.0f;
            const float roughness = 0.06f + 0.94f * static_cast<float>(column) / 5.0f;
            const float hue = 0.15f + 0.7f * static_cast<float>(row) / 4.0f;
            instances.push_back({{(static_cast<float>(column) - 2.5f) * 1.25f,
                                  -0.35f + static_cast<float>(row) * 0.86f, 0.0f},
                                 {metallic, roughness, hue}});
        }
    }
    if (!uploadVector(*device, gfx::BufferUsage::Vertex, instances, instanceBuffer_)) return false;
    if (constants_.create(*device, 96, 1) != gfx::GfxError::None) return false;
    if (!createShader(*device, gfx::ShaderStage::Vertex,
                      F4_VALIDATION_SHADER(kPbrVertex, kValidationPbrVertexWgsl),
                      vertexShader_) ||
        !createShader(*device, gfx::ShaderStage::Fragment,
                      F4_VALIDATION_SHADER(kPbrFragment, kValidationPbrFragmentWgsl),
                      fragmentShader_)) return false;

    gfx::PipelineStateDesc desc;
    desc.vertexShader = vertexShader_;
    desc.fragmentShader = fragmentShader_;
    desc.raster.cullMode = gfx::CullMode::Back;
    desc.depthStencil = {true, true, gfx::CompareOp::Less};
    desc.vertexInput.bindings.push({0, sizeof(SphereVertex), 0});
    desc.vertexInput.bindings.push({1, sizeof(Instance), 1});
    desc.vertexInput.attributes.push({0, 0, gfx::VertexFormat::Float32x3, 0});
    desc.vertexInput.attributes.push({0, 1, gfx::VertexFormat::Float32x3, 12});
    desc.vertexInput.attributes.push({1, 2, gfx::VertexFormat::Float32x3, 0});
    desc.vertexInput.attributes.push({1, 3, gfx::VertexFormat::Float32x3, 12});
    addUniform(desc, gfx::shaderStageBit(gfx::ShaderStage::Vertex) |
                         gfx::shaderStageBit(gfx::ShaderStage::Fragment));
    const auto pipeline = device->createPipeline(desc);
    if (!pipeline.hasValue()) return false;
    pipeline_ = pipeline.value();
    initialized_ = true;
    return true;
}

void PbrSphereGridScene::shutdown(gfx::IDevice* device) {
    if (pipeline_.isValid()) device->destroyPipeline(pipeline_);
    if (fragmentShader_.isValid()) device->destroyShader(fragmentShader_);
    if (vertexShader_.isValid()) device->destroyShader(vertexShader_);
    if (instanceBuffer_.isValid()) device->destroyBuffer(instanceBuffer_);
    if (indexBuffer_.isValid()) device->destroyBuffer(indexBuffer_);
    if (vertexBuffer_.isValid()) device->destroyBuffer(vertexBuffer_);
    constants_.destroy(*device);
    pipeline_ = {};
    fragmentShader_ = {};
    vertexShader_ = {};
    instanceBuffer_ = {};
    indexBuffer_ = {};
    vertexBuffer_ = {};
    initialized_ = false;
}

void PbrSphereGridScene::onResize(std::uint32_t, std::uint32_t) {}
void PbrSphereGridScene::update(float, const Uint8*, const SDL_MouseMotionEvent*) {}

bool PbrSphereGridScene::prepareStaticCapture(FreeCamera& camera) {
    if (!initialized_) return false;
    camera.position = {0.0f, 2.15f, 7.5f};
    camera.yaw = 1.57079633f;
    camera.pitch = 0.0f;
    // Separate the rows for inspection; the interactive fixture keeps its
    // original layout and the Metal/WebGPU reference captures are unchanged.
    std::array<std::array<float, 6>, 30> instances{};
    for (std::uint32_t row = 0; row < 5; ++row) {
        for (std::uint32_t column = 0; column < 6; ++column) {
            instances[row * 6 + column] = {
                (static_cast<float>(column) - 2.5f) * 1.25f,
                -0.35f + static_cast<float>(row) * 1.25f, 0.0f,
                static_cast<float>(row) / 4.0f,
                0.06f + 0.94f * static_cast<float>(column) / 5.0f,
                0.55f};
        }
    }
    return device_->writeBuffer(instanceBuffer_, 0, std::as_bytes(std::span(instances))) ==
        gfx::GfxError::None;
}

void PbrSphereGridScene::render(gfx::CommandBuffer& cb, const FreeCamera& camera,
                                std::uint32_t width, std::uint32_t height) {
    if (!initialized_) return;
    constants_.beginFrame();
    const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    struct SceneData {
        math::Mat4 viewProj;
        float cameraPos[4];
        float lightPos[4];
    };
    const SceneData data{camera.viewProjMatrix(aspect),
                         {camera.position.x, camera.position.y, camera.position.z, 1.0f},
                         {-3.0f, 7.5f, 5.5f, 1.0f}};
    const auto slot = constants_.writeAndBind(*device_, cb, std::as_bytes(std::span(&data, 1)));
    if (!slot.valid) return;
    cb.bindPipeline(pipeline_);
    cb.bindVertexBuffer(0, vertexBuffer_, sizeof(SphereVertex));
    cb.bindVertexBuffer(1, instanceBuffer_, sizeof(float) * 6);
    cb.bindIndexBuffer(indexBuffer_, gfx::IndexFormat::U16);
    cb.bindResource(0, gfx::BindingType::UniformBuffer, slot.handle.index, slot.handle.generation);
    cb.drawIndexed(indexCount_, 30);
}

bool MipmappingScene::init(gfx::IDevice* device, platform::Window*) {
    if (initialized_) return true;
    gfx::TextureDesc textureDesc;
    textureDesc.width = 256;
    textureDesc.height = 256;
    textureDesc.mipLevels = 9;
    const auto texture = device->createTexture(textureDesc);
    if (!texture.hasValue()) return false;
    texture_ = texture.value();

    std::uint32_t mipWidth = 256;
    std::uint32_t mipHeight = 256;
    std::vector<std::uint8_t> pixels(mipWidth * mipHeight * 4u);
    for (std::uint32_t y = 0; y < mipHeight; ++y) {
        for (std::uint32_t x = 0; x < mipWidth; ++x) {
            const bool white = ((x / 4u) ^ (y / 4u)) % 2u == 0u;
            const std::uint8_t value = white ? 240u : 12u;
            const std::size_t offset = (static_cast<std::size_t>(y) * mipWidth + x) * 4u;
            pixels[offset + 0] = value;
            pixels[offset + 1] = value;
            pixels[offset + 2] = value;
            pixels[offset + 3] = 255u;
        }
    }
    for (std::uint32_t level = 0; level < textureDesc.mipLevels; ++level) {
        if (device->writeTexture(texture_, level, std::as_bytes(std::span(pixels))) !=
            gfx::GfxError::None) return false;
        if (level + 1u == textureDesc.mipLevels) break;
        const std::uint32_t nextWidth = std::max(1u, mipWidth >> 1u);
        const std::uint32_t nextHeight = std::max(1u, mipHeight >> 1u);
        std::vector<std::uint8_t> next(nextWidth * nextHeight * 4u);
        for (std::uint32_t y = 0; y < nextHeight; ++y) {
            for (std::uint32_t x = 0; x < nextWidth; ++x) {
                for (std::uint32_t channel = 0; channel < 4; ++channel) {
                    std::uint32_t sum = 0;
                    for (std::uint32_t oy = 0; oy < 2; ++oy) {
                        for (std::uint32_t ox = 0; ox < 2; ++ox) {
                            const std::uint32_t sourceX = std::min(mipWidth - 1u, x * 2u + ox);
                            const std::uint32_t sourceY = std::min(mipHeight - 1u, y * 2u + oy);
                            sum += pixels[(static_cast<std::size_t>(sourceY) * mipWidth + sourceX) * 4u + channel];
                        }
                    }
                    next[(static_cast<std::size_t>(y) * nextWidth + x) * 4u + channel] =
                        static_cast<std::uint8_t>(sum / 4u);
                }
            }
        }
        pixels = std::move(next);
        mipWidth = nextWidth;
        mipHeight = nextHeight;
    }

    gfx::SamplerDesc noMip;
    noMip.addressU = gfx::AddressMode::Repeat;
    noMip.addressV = gfx::AddressMode::Repeat;
    const auto levelZero = device->createSampler(noMip);
    if (!levelZero.hasValue()) return false;
    levelZeroSampler_ = levelZero.value();
    gfx::SamplerDesc mip = noMip;
    mip.mipFilter = gfx::MipFilter::Linear;
    const auto trilinear = device->createSampler(mip);
    if (!trilinear.hasValue()) return false;
    mipSampler_ = trilinear.value();

    if (!createShader(*device, gfx::ShaderStage::Vertex,
                      F4_VALIDATION_SHADER(kFullscreenVertex,
                                           kValidationFullscreenVertexWgsl),
                      vertexShader_) ||
        !createShader(*device, gfx::ShaderStage::Fragment,
                      F4_VALIDATION_SHADER(kMipFragment, kValidationMipFragmentWgsl),
                      fragmentShader_)) return false;
    gfx::PipelineStateDesc desc;
    desc.vertexShader = vertexShader_;
    desc.fragmentShader = fragmentShader_;
    desc.raster.cullMode = gfx::CullMode::None;
    desc.depthStencil.depthAttachmentEnable = true;
    addSampledTexture(desc, 0);
    addSampledTexture(desc, 1);
    const auto pipeline = device->createPipeline(desc);
    if (!pipeline.hasValue()) return false;
    pipeline_ = pipeline.value();
    initialized_ = true;
    return true;
}

void MipmappingScene::shutdown(gfx::IDevice* device) {
    if (pipeline_.isValid()) device->destroyPipeline(pipeline_);
    if (fragmentShader_.isValid()) device->destroyShader(fragmentShader_);
    if (vertexShader_.isValid()) device->destroyShader(vertexShader_);
    if (mipSampler_.isValid()) device->destroySampler(mipSampler_);
    if (levelZeroSampler_.isValid()) device->destroySampler(levelZeroSampler_);
    if (texture_.isValid()) device->destroyTexture(texture_);
    pipeline_ = {};
    fragmentShader_ = {};
    vertexShader_ = {};
    mipSampler_ = {};
    levelZeroSampler_ = {};
    texture_ = {};
    initialized_ = false;
}

void MipmappingScene::onResize(std::uint32_t, std::uint32_t) {}
void MipmappingScene::update(float, const Uint8*, const SDL_MouseMotionEvent*) {}

void MipmappingScene::render(gfx::CommandBuffer& cb, const FreeCamera&,
                             std::uint32_t, std::uint32_t) {
    if (!initialized_) return;
    cb.bindPipeline(pipeline_);
    cb.bindResource(0, gfx::BindingType::Texture, texture_.index, texture_.generation);
    cb.bindResource(0, gfx::BindingType::Sampler, levelZeroSampler_.index,
                    levelZeroSampler_.generation);
    cb.bindResource(1, gfx::BindingType::Texture, texture_.index, texture_.generation);
    cb.bindResource(1, gfx::BindingType::Sampler, mipSampler_.index, mipSampler_.generation);
    cb.draw(3);
}

bool ZFightingScene::init(gfx::IDevice* device, platform::Window*) {
    if (initialized_) return true;
    device_ = device;
    std::vector<ColorVertex> vertices;
    constexpr std::array<float, 6> separations = {0.0f, 0.0000001f, 0.000001f,
                                                  0.00001f, 0.0001f, 0.001f};
    for (std::uint32_t i = 0; i < separations.size(); ++i) {
        const float left = -4.8f + static_cast<float>(i) * 1.6f;
        appendWorldQuad(vertices, left, left + 1.35f, 2.0f, -30.0f, 0.0f,
                        {0.16f, 0.18f, 0.21f, 1.0f});
    }
    for (std::uint32_t i = 0; i < separations.size(); ++i) {
        const float left = -4.8f + static_cast<float>(i) * 1.6f;
        const float t = static_cast<float>(i) / static_cast<float>(separations.size() - 1u);
        appendWorldQuad(vertices, left, left + 1.35f, 2.0f, -30.0f, separations[i],
                        {0.9f - t * 0.55f, 0.12f + t * 0.7f, 0.08f, 1.0f});
    }
    vertexCount_ = static_cast<std::uint32_t>(vertices.size());
    if (!uploadVector(*device, gfx::BufferUsage::Vertex, vertices, vertexBuffer_)) return false;
    if (constants_.create(*device, 64, 1) != gfx::GfxError::None) return false;
    if (!createShader(*device, gfx::ShaderStage::Vertex,
                      F4_VALIDATION_SHADER(kWorldColorVertex,
                                           kValidationWorldColorVertexWgsl),
                      vertexShader_) ||
        !createShader(*device, gfx::ShaderStage::Fragment,
                      F4_VALIDATION_SHADER(kVertexColorFragment,
                                           kValidationVertexColorFragmentWgsl),
                      fragmentShader_)) return false;
    gfx::PipelineStateDesc desc;
    desc.vertexShader = vertexShader_;
    desc.fragmentShader = fragmentShader_;
    desc.raster.cullMode = gfx::CullMode::None;
    desc.depthStencil = {true, true, gfx::CompareOp::Less};
    desc.vertexInput.bindings.push({0, sizeof(ColorVertex), 0});
    desc.vertexInput.attributes.push({0, 0, gfx::VertexFormat::Float32x3, 0});
    desc.vertexInput.attributes.push({0, 1, gfx::VertexFormat::Float32x4, 12});
    addUniform(desc, gfx::shaderStageBit(gfx::ShaderStage::Vertex));
    const auto pipeline = device->createPipeline(desc);
    if (!pipeline.hasValue()) return false;
    pipeline_ = pipeline.value();
    initialized_ = true;
    return true;
}

void ZFightingScene::shutdown(gfx::IDevice* device) {
    if (pipeline_.isValid()) device->destroyPipeline(pipeline_);
    if (fragmentShader_.isValid()) device->destroyShader(fragmentShader_);
    if (vertexShader_.isValid()) device->destroyShader(vertexShader_);
    if (vertexBuffer_.isValid()) device->destroyBuffer(vertexBuffer_);
    constants_.destroy(*device);
    pipeline_ = {};
    fragmentShader_ = {};
    vertexShader_ = {};
    vertexBuffer_ = {};
    initialized_ = false;
}

void ZFightingScene::onResize(std::uint32_t, std::uint32_t) {}
void ZFightingScene::update(float, const Uint8*, const SDL_MouseMotionEvent*) {}

void ZFightingScene::render(gfx::CommandBuffer& cb, const FreeCamera& camera,
                            std::uint32_t width, std::uint32_t height) {
    if (!initialized_) return;
    constants_.beginFrame();
    const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const math::Mat4 viewProj = camera.viewProjMatrix(aspect, 1.04719758f, 0.1f, 150.0f);
    const auto slot = constants_.writeAndBind(*device_, cb,
                                              std::as_bytes(std::span(&viewProj, 1)));
    if (!slot.valid) return;
    cb.bindPipeline(pipeline_);
    cb.bindVertexBuffer(0, vertexBuffer_, sizeof(ColorVertex));
    cb.bindResource(0, gfx::BindingType::UniformBuffer, slot.handle.index, slot.handle.generation);
    cb.draw(vertexCount_);
}

bool AlphaOrderScene::init(gfx::IDevice* device, platform::Window*) {
    if (initialized_) return true;
    std::vector<ScreenVertex> panels;
    appendScreenQuad(panels, -0.5f, 0.0f, 0.88f, 1.65f, {0.025f, 0.09f, 0.07f, 1.0f});
    appendScreenQuad(panels, 0.5f, 0.0f, 0.88f, 1.65f, {0.11f, 0.035f, 0.035f, 1.0f});
    if (!uploadVector(*device, gfx::BufferUsage::Vertex, panels, panelBuffer_)) return false;
    const std::array<std::array<float, 4>, 3> colors = {{{0.08f, 0.35f, 1.0f, 0.58f},
                                                          {0.1f, 0.9f, 0.3f, 0.58f},
                                                          {1.0f, 0.16f, 0.08f, 0.58f}}};
    for (std::uint32_t panel = 0; panel < 2; ++panel) {
        const float panelCenter = panel == 0 ? -0.5f : 0.5f;
        for (std::uint32_t layer = 0; layer < 3; ++layer) {
            std::vector<ScreenVertex> quad;
            const float offset = (static_cast<float>(layer) - 1.0f) * 0.12f;
            appendScreenQuad(quad, panelCenter + offset, -offset, 0.50f, 0.68f, colors[layer]);
            const std::uint32_t index = panel * 3u + layer;
            if (!uploadVector(*device, gfx::BufferUsage::Vertex, quad, layerBuffers_[index])) return false;
        }
    }
    if (!createShader(*device, gfx::ShaderStage::Vertex,
                      F4_VALIDATION_SHADER(kScreenColorVertex,
                                           kValidationScreenColorVertexWgsl),
                      vertexShader_) ||
        !createShader(*device, gfx::ShaderStage::Fragment,
                      F4_VALIDATION_SHADER(kVertexColorFragment,
                                           kValidationVertexColorFragmentWgsl),
                      fragmentShader_)) return false;
    gfx::PipelineStateDesc opaque;
    opaque.vertexShader = vertexShader_;
    opaque.fragmentShader = fragmentShader_;
    opaque.raster.cullMode = gfx::CullMode::None;
    opaque.depthStencil.depthAttachmentEnable = true;
    opaque.vertexInput.bindings.push({0, sizeof(ScreenVertex), 0});
    opaque.vertexInput.attributes.push({0, 0, gfx::VertexFormat::Float32x2, 0});
    opaque.vertexInput.attributes.push({0, 1, gfx::VertexFormat::Float32x4, 8});
    auto pipeline = device->createPipeline(opaque);
    if (!pipeline.hasValue()) return false;
    opaquePipeline_ = pipeline.value();
    gfx::PipelineStateDesc alpha = opaque;
    gfx::ColorAttachmentState blend;
    blend.blendEnable = true;
    blend.srcColor = gfx::BlendFactor::SrcAlpha;
    blend.dstColor = gfx::BlendFactor::OneMinusSrcAlpha;
    blend.srcAlpha = gfx::BlendFactor::One;
    blend.dstAlpha = gfx::BlendFactor::OneMinusSrcAlpha;
    alpha.colorAttachments.push(blend);
    pipeline = device->createPipeline(alpha);
    if (!pipeline.hasValue()) return false;
    alphaPipeline_ = pipeline.value();
    initialized_ = true;
    return true;
}

void AlphaOrderScene::shutdown(gfx::IDevice* device) {
    if (alphaPipeline_.isValid()) device->destroyPipeline(alphaPipeline_);
    if (opaquePipeline_.isValid()) device->destroyPipeline(opaquePipeline_);
    if (fragmentShader_.isValid()) device->destroyShader(fragmentShader_);
    if (vertexShader_.isValid()) device->destroyShader(vertexShader_);
    for (auto& buffer : layerBuffers_) {
        if (buffer.isValid()) device->destroyBuffer(buffer);
        buffer = {};
    }
    if (panelBuffer_.isValid()) device->destroyBuffer(panelBuffer_);
    alphaPipeline_ = {};
    opaquePipeline_ = {};
    fragmentShader_ = {};
    vertexShader_ = {};
    panelBuffer_ = {};
    initialized_ = false;
}

void AlphaOrderScene::onResize(std::uint32_t, std::uint32_t) {}
void AlphaOrderScene::update(float, const Uint8*, const SDL_MouseMotionEvent*) {}

void AlphaOrderScene::render(gfx::CommandBuffer& cb, const FreeCamera&,
                             std::uint32_t, std::uint32_t) {
    if (!initialized_) return;
    cb.bindPipeline(opaquePipeline_);
    cb.bindVertexBuffer(0, panelBuffer_, sizeof(ScreenVertex));
    cb.draw(12);
    cb.bindPipeline(alphaPipeline_);
    for (std::uint32_t layer = 0; layer < 3; ++layer) {
        cb.bindVertexBuffer(0, layerBuffers_[layer], sizeof(ScreenVertex));
        cb.draw(6);
    }
    for (std::uint32_t reverse = 0; reverse < 3; ++reverse) {
        cb.bindVertexBuffer(0, layerBuffers_[5u - reverse], sizeof(ScreenVertex));
        cb.draw(6);
    }
}

bool NormalMapOrientationScene::init(gfx::IDevice* device, platform::Window*) {
    if (initialized_) return true;
    constexpr std::uint32_t size = 256;
    std::vector<std::uint8_t> pixels(size * size * 4u);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            const float cellX = (u * 2.0f - std::floor(u * 2.0f)) * 2.0f - 1.0f;
            const float cellY = (v * 2.0f - std::floor(v * 2.0f)) * 2.0f - 1.0f;
            const float nx = cellX / 0.82f;
            const float ny = cellY / 0.82f;
            const float radiusSquared = nx * nx + ny * ny;
            math::Vec3 normal{0.0f, 0.0f, 1.0f};
            if (radiusSquared < 1.0f) {
                normal = {nx, ny, std::sqrt(1.0f - radiusSquared)};
            }
            const std::size_t offset = (static_cast<std::size_t>(y) * size + x) * 4u;
            pixels[offset + 0] = static_cast<std::uint8_t>((normal.x * 0.5f + 0.5f) * 255.0f);
            pixels[offset + 1] = static_cast<std::uint8_t>((normal.y * 0.5f + 0.5f) * 255.0f);
            pixels[offset + 2] = static_cast<std::uint8_t>((normal.z * 0.5f + 0.5f) * 255.0f);
            pixels[offset + 3] = 255u;
        }
    }
    gfx::TextureDesc textureDesc;
    textureDesc.width = size;
    textureDesc.height = size;
    const auto texture = device->createTexture(textureDesc);
    if (!texture.hasValue()) return false;
    normalTexture_ = texture.value();
    if (device->writeTexture(normalTexture_, 0, std::as_bytes(std::span(pixels))) !=
        gfx::GfxError::None) return false;
    gfx::SamplerDesc samplerDesc;
    samplerDesc.addressU = gfx::AddressMode::Repeat;
    samplerDesc.addressV = gfx::AddressMode::Repeat;
    const auto sampler = device->createSampler(samplerDesc);
    if (!sampler.hasValue()) return false;
    sampler_ = sampler.value();
    if (!createShader(*device, gfx::ShaderStage::Vertex,
                      F4_VALIDATION_SHADER(kFullscreenVertex,
                                           kValidationFullscreenVertexWgsl),
                      vertexShader_) ||
        !createShader(*device, gfx::ShaderStage::Fragment,
                      F4_VALIDATION_SHADER(kNormalMapFragment,
                                           kValidationNormalMapFragmentWgsl),
                      fragmentShader_)) return false;
    gfx::PipelineStateDesc desc;
    desc.vertexShader = vertexShader_;
    desc.fragmentShader = fragmentShader_;
    desc.raster.cullMode = gfx::CullMode::None;
    desc.depthStencil.depthAttachmentEnable = true;
    addSampledTexture(desc, 0);
    const auto pipeline = device->createPipeline(desc);
    if (!pipeline.hasValue()) return false;
    pipeline_ = pipeline.value();
    initialized_ = true;
    return true;
}

void NormalMapOrientationScene::shutdown(gfx::IDevice* device) {
    if (pipeline_.isValid()) device->destroyPipeline(pipeline_);
    if (fragmentShader_.isValid()) device->destroyShader(fragmentShader_);
    if (vertexShader_.isValid()) device->destroyShader(vertexShader_);
    if (sampler_.isValid()) device->destroySampler(sampler_);
    if (normalTexture_.isValid()) device->destroyTexture(normalTexture_);
    pipeline_ = {};
    fragmentShader_ = {};
    vertexShader_ = {};
    sampler_ = {};
    normalTexture_ = {};
    initialized_ = false;
}

void NormalMapOrientationScene::onResize(std::uint32_t, std::uint32_t) {}
void NormalMapOrientationScene::update(float, const Uint8*, const SDL_MouseMotionEvent*) {}

void NormalMapOrientationScene::render(gfx::CommandBuffer& cb, const FreeCamera&,
                                       std::uint32_t, std::uint32_t) {
    if (!initialized_) return;
    cb.bindPipeline(pipeline_);
    cb.bindResource(0, gfx::BindingType::Texture, normalTexture_.index, normalTexture_.generation);
    cb.bindResource(0, gfx::BindingType::Sampler, sampler_.index, sampler_.generation);
    cb.draw(3);
}

namespace {

struct CubeVertex {
    float position[3];
    float normal[3];
};

constexpr CubeVertex kCubeVertices[] = {
    {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
    {{0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
    {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
    {{-0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
    {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}},
    {{0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}},
    {{0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f, -0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f, 0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}},
    {{-0.5f, 0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}},
    {{0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, 0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{-0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    {{0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    {{0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}},
    {{-0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}},
    {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}},
    {{0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}},
    {{0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f}},
    {{-0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f}},
};

constexpr std::uint16_t kCubeIndices[] = {
    0, 2, 1, 2, 0, 3, 4, 5, 6, 6, 7, 4,
    8, 9, 10, 10, 11, 8, 12, 14, 13, 14, 12, 15,
    16, 18, 17, 18, 16, 19, 20, 21, 22, 22, 23, 20,
};

[[maybe_unused]] constexpr const char* kGBufferVertex = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(std140, binding = 0) uniform Object {
    mat4 viewProj;
    mat4 model;
    vec4 albedo;
};
out vec3 vNormal;
out vec4 vAlbedo;
void main() {
    vec4 world = model * vec4(aPos, 1.0);
    gl_Position = viewProj * world;
    vNormal = mat3(transpose(inverse(model))) * aNormal;
    vAlbedo = albedo;
}
)glsl";

[[maybe_unused]] constexpr const char* kGBufferFragment = R"glsl(
#version 430 core
in vec3 vNormal;
in vec4 vAlbedo;
layout(location = 0) out vec4 gAlbedo;
layout(location = 1) out vec4 gNormal;
void main() {
    gAlbedo = vAlbedo;
    gNormal = vec4(normalize(vNormal) * 0.5 + 0.5, 1.0);
}
)glsl";

[[maybe_unused]] constexpr const char* kDeferredLightingFragment = R"glsl(
#version 430 core
in vec2 vUv;
uniform sampler2D uTex;
uniform sampler2D uTex1;
out vec4 fragColor;
void main() {
    vec4 albedoSample = texture(uTex, vUv);
    if (albedoSample.a < 0.5) {
        vec3 low = vec3(0.018, 0.032, 0.065);
        vec3 high = vec3(0.11, 0.19, 0.28);
        fragColor = vec4(mix(low, high, vUv.y), 1.0);
        return;
    }
    vec3 N = normalize(texture(uTex1, vUv).xyz * 2.0 - 1.0);
    vec3 L = normalize(vec3(-0.45, 0.8, 0.55));
    float diffuse = max(dot(N, L), 0.0);
    float rim = pow(1.0 - max(N.z, 0.0), 2.0);
    vec3 color = albedoSample.rgb * (0.16 + 0.84 * diffuse) + vec3(0.12) * rim;
    fragColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
)glsl";

[[maybe_unused]] constexpr const char* kForwardGlassFragment = R"glsl(
#version 430 core
in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)glsl";

} // namespace

bool HybridRenderingScene::init(gfx::IDevice* device, platform::Window*) {
    if (initialized_) return true;
    device_ = device;
    if (!createBuffer(*device, gfx::BufferUsage::Vertex,
                      std::as_bytes(std::span(kCubeVertices)), vertexBuffer_) ||
        !createBuffer(*device, gfx::BufferUsage::Index,
                      std::as_bytes(std::span(kCubeIndices)), indexBuffer_)) return false;

    const std::array<std::array<float, 4>, 3> glassColors = {{{0.05f, 0.72f, 0.92f, 0.24f},
                                                               {1.0f, 0.34f, 0.08f, 0.22f},
                                                               {0.20f, 0.95f, 0.48f, 0.20f}}};
    for (std::uint32_t i = 0; i < 3; ++i) {
        std::vector<ScreenVertex> quad;
        appendScreenQuad(quad, (static_cast<float>(i) - 1.0f) * 0.18f,
                         (1.0f - static_cast<float>(i)) * 0.10f,
                         0.72f, 1.18f, glassColors[i]);
        if (!uploadVector(*device, gfx::BufferUsage::Vertex, quad, layerBuffers_[i])) return false;
    }
    if (constants_.create(*device, 144, 4) != gfx::GfxError::None) return false;

    if (!createShader(*device, gfx::ShaderStage::Vertex,
                      F4_VALIDATION_SHADER(kGBufferVertex,
                                           kValidationGBufferVertexWgsl),
                      vertexShader_) ||
        !createShader(*device, gfx::ShaderStage::Fragment,
                      F4_VALIDATION_SHADER(kGBufferFragment,
                                           kValidationGBufferFragmentWgsl),
                      fragmentShader_) ||
        !createShader(*device, gfx::ShaderStage::Vertex,
                      F4_VALIDATION_SHADER(kFullscreenVertex,
                                           kValidationFullscreenVertexWgsl),
                      deferredVertexShader_) ||
        !createShader(*device, gfx::ShaderStage::Fragment,
                      F4_VALIDATION_SHADER(kDeferredLightingFragment,
                                           kValidationDeferredFragmentWgsl),
                      deferredFragmentShader_) ||
        !createShader(*device, gfx::ShaderStage::Vertex,
                      F4_VALIDATION_SHADER(kScreenColorVertex,
                                           kValidationScreenColorVertexWgsl),
                      transparentVertexShader_) ||
        !createShader(*device, gfx::ShaderStage::Fragment,
                      F4_VALIDATION_SHADER(kForwardGlassFragment,
                                           kValidationVertexColorFragmentWgsl),
                      transparentFragmentShader_)) return false;

    gfx::PipelineStateDesc geometry;
    geometry.vertexShader = vertexShader_;
    geometry.fragmentShader = fragmentShader_;
    geometry.raster.cullMode = gfx::CullMode::Back;
    geometry.depthStencil = {true, true, gfx::CompareOp::Less};
    geometry.vertexInput.bindings.push({0, sizeof(CubeVertex), 0});
    geometry.vertexInput.attributes.push({0, 0, gfx::VertexFormat::Float32x3, 0});
    geometry.vertexInput.attributes.push({0, 1, gfx::VertexFormat::Float32x3, 12});
    geometry.colorAttachments.push({});
    gfx::ColorAttachmentState normalTarget;
    normalTarget.format = gfx::TextureFormat::RGBA16_FLOAT;
    geometry.colorAttachments.push(normalTarget);
    addUniform(geometry, gfx::shaderStageBit(gfx::ShaderStage::Vertex));
    auto pipeline = device->createPipeline(geometry);
    if (!pipeline.hasValue()) return false;
    geometryPipeline_ = pipeline.value();

    gfx::PipelineStateDesc deferred;
    deferred.vertexShader = deferredVertexShader_;
    deferred.fragmentShader = deferredFragmentShader_;
    deferred.raster.cullMode = gfx::CullMode::None;
    deferred.depthStencil.depthAttachmentEnable = true;
    addSampledTexture(deferred, 0);
    addSampledTexture(deferred, 1);
    pipeline = device->createPipeline(deferred);
    if (!pipeline.hasValue()) return false;
    deferredPipeline_ = pipeline.value();

    gfx::PipelineStateDesc transparent;
    transparent.vertexShader = transparentVertexShader_;
    transparent.fragmentShader = transparentFragmentShader_;
    transparent.raster.cullMode = gfx::CullMode::None;
    transparent.depthStencil.depthAttachmentEnable = true;
    transparent.vertexInput.bindings.push({0, sizeof(ScreenVertex), 0});
    transparent.vertexInput.attributes.push({0, 0, gfx::VertexFormat::Float32x2, 0});
    transparent.vertexInput.attributes.push({0, 1, gfx::VertexFormat::Float32x4, 8});
    gfx::ColorAttachmentState blend;
    blend.blendEnable = true;
    blend.srcColor = gfx::BlendFactor::SrcAlpha;
    blend.dstColor = gfx::BlendFactor::OneMinusSrcAlpha;
    blend.srcAlpha = gfx::BlendFactor::One;
    blend.dstAlpha = gfx::BlendFactor::OneMinusSrcAlpha;
    transparent.colorAttachments.push(blend);
    pipeline = device->createPipeline(transparent);
    if (!pipeline.hasValue()) return false;
    transparentPipeline_ = pipeline.value();

    gfx::SamplerDesc samplerDesc;
    const auto sampler = device->createSampler(samplerDesc);
    if (!sampler.hasValue()) return false;
    sampler_ = sampler.value();
    initialized_ = true;
    return true;
}

void HybridRenderingScene::destroyTargets() {
    if (device_ == nullptr) return;
    if (gbuffer_.isValid()) device_->destroyFramebuffer(gbuffer_);
    if (depthTexture_.isValid()) device_->destroyTexture(depthTexture_);
    if (normalTexture_.isValid()) device_->destroyTexture(normalTexture_);
    if (albedoTexture_.isValid()) device_->destroyTexture(albedoTexture_);
    gbuffer_ = {};
    depthTexture_ = {};
    normalTexture_ = {};
    albedoTexture_ = {};
    targetWidth_ = 0;
    targetHeight_ = 0;
}

void HybridRenderingScene::shutdown(gfx::IDevice* device) {
    destroyTargets();
    if (transparentPipeline_.isValid()) device->destroyPipeline(transparentPipeline_);
    if (deferredPipeline_.isValid()) device->destroyPipeline(deferredPipeline_);
    if (geometryPipeline_.isValid()) device->destroyPipeline(geometryPipeline_);
    if (transparentFragmentShader_.isValid()) device->destroyShader(transparentFragmentShader_);
    if (transparentVertexShader_.isValid()) device->destroyShader(transparentVertexShader_);
    if (deferredFragmentShader_.isValid()) device->destroyShader(deferredFragmentShader_);
    if (deferredVertexShader_.isValid()) device->destroyShader(deferredVertexShader_);
    if (fragmentShader_.isValid()) device->destroyShader(fragmentShader_);
    if (vertexShader_.isValid()) device->destroyShader(vertexShader_);
    if (sampler_.isValid()) device->destroySampler(sampler_);
    for (auto& buffer : layerBuffers_) {
        if (buffer.isValid()) device->destroyBuffer(buffer);
        buffer = {};
    }
    if (indexBuffer_.isValid()) device->destroyBuffer(indexBuffer_);
    if (vertexBuffer_.isValid()) device->destroyBuffer(vertexBuffer_);
    constants_.destroy(*device);
    transparentPipeline_ = {};
    deferredPipeline_ = {};
    geometryPipeline_ = {};
    transparentFragmentShader_ = {};
    transparentVertexShader_ = {};
    deferredFragmentShader_ = {};
    deferredVertexShader_ = {};
    fragmentShader_ = {};
    vertexShader_ = {};
    sampler_ = {};
    indexBuffer_ = {};
    vertexBuffer_ = {};
    initialized_ = false;
    device_ = nullptr;
}

void HybridRenderingScene::onResize(std::uint32_t width, std::uint32_t height) {
    if (!initialized_ || width == 0 || height == 0 ||
        (width == targetWidth_ && height == targetHeight_)) return;
    destroyTargets();
    gfx::TextureDesc color;
    color.width = width;
    color.height = height;
    const auto albedo = device_->createTexture(color);
    if (!albedo.hasValue()) return;
    albedoTexture_ = albedo.value();
    color.format = gfx::TextureFormat::RGBA16_FLOAT;
    const auto normal = device_->createTexture(color);
    if (!normal.hasValue()) return;
    normalTexture_ = normal.value();
    gfx::TextureDesc depth;
    depth.width = width;
    depth.height = height;
    depth.format = gfx::TextureFormat::Depth32F;
    const auto depthResult = device_->createTexture(depth);
    if (!depthResult.hasValue()) return;
    depthTexture_ = depthResult.value();

    gfx::FramebufferDesc framebuffer;
    framebuffer.extent = {width, height};
    gfx::AttachmentDesc albedoAttachment;
    albedoAttachment.type = gfx::AttachmentType::Color;
    albedoAttachment.texture = albedoTexture_;
    albedoAttachment.clearOnLoad = true;
    albedoAttachment.clear.color[0] = 0.0f;
    albedoAttachment.clear.color[1] = 0.0f;
    albedoAttachment.clear.color[2] = 0.0f;
    albedoAttachment.clear.color[3] = 0.0f;
    framebuffer.attachments.push(albedoAttachment);
    gfx::AttachmentDesc normalAttachment;
    normalAttachment.type = gfx::AttachmentType::Color;
    normalAttachment.texture = normalTexture_;
    normalAttachment.clearOnLoad = true;
    normalAttachment.clear.color[0] = 0.5f;
    normalAttachment.clear.color[1] = 0.5f;
    normalAttachment.clear.color[2] = 1.0f;
    normalAttachment.clear.color[3] = 1.0f;
    framebuffer.attachments.push(normalAttachment);
    gfx::AttachmentDesc depthAttachment;
    depthAttachment.type = gfx::AttachmentType::Depth;
    depthAttachment.texture = depthTexture_;
    depthAttachment.clearOnLoad = true;
    depthAttachment.clear.depth = 1.0f;
    framebuffer.attachments.push(depthAttachment);
    const auto result = device_->createFramebuffer(framebuffer);
    if (!result.hasValue()) return;
    gbuffer_ = result.value();
    targetWidth_ = width;
    targetHeight_ = height;
}

void HybridRenderingScene::update(float dt, const Uint8*, const SDL_MouseMotionEvent*) {
    time_ += dt * 0.45f;
    if (time_ > 2.0f * kPi) time_ -= 2.0f * kPi;
}

void HybridRenderingScene::render(gfx::CommandBuffer&, const FreeCamera&,
                                  std::uint32_t, std::uint32_t) {}

void HybridRenderingScene::renderPasses(gfx::CommandBuffer& cb, const FreeCamera& camera,
                                        std::uint32_t width, std::uint32_t height,
                                        gfx::FramebufferHandle target) {
    if (!initialized_ || !gbuffer_.isValid()) return;
    constants_.beginFrame();
    const gfx::Extent extent{width, height};
    cb.beginRenderPass(gbuffer_);
    cb.setViewport(extent);
    cb.bindPipeline(geometryPipeline_);
    cb.bindVertexBuffer(0, vertexBuffer_, sizeof(CubeVertex));
    cb.bindIndexBuffer(indexBuffer_, gfx::IndexFormat::U16);

    struct ObjectData {
        math::Mat4 viewProj;
        math::Mat4 model;
        float albedo[4];
    };
    const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const math::Mat4 viewProj = camera.viewProjMatrix(aspect);
    const std::array<math::Vec3, 3> positions = {{{-2.1f, 1.1f, -0.5f},
                                                  {0.0f, 1.35f, -1.6f},
                                                  {2.1f, 0.9f, -0.2f}}};
    const std::array<math::Vec3, 3> scales = {{{1.45f, 2.2f, 1.45f},
                                               {1.8f, 1.8f, 1.8f},
                                               {1.4f, 1.8f, 1.4f}}};
    const std::array<std::array<float, 4>, 3> colors = {{{0.04f, 0.42f, 0.78f, 1.0f},
                                                          {0.92f, 0.26f, 0.07f, 1.0f},
                                                          {0.12f, 0.72f, 0.30f, 1.0f}}};
    for (std::uint32_t i = 0; i < 3; ++i) {
        const math::Mat4 model = math::Mat4::translation(positions[i])
                                     .mul(math::Mat4::rotationY(time_ + static_cast<float>(i)))
                                     .mul(math::Mat4::scaling(scales[i]));
        const ObjectData object{viewProj, model,
                                {colors[i][0], colors[i][1], colors[i][2], colors[i][3]}};
        const auto slot = constants_.writeAndBind(*device_, cb,
                                                  std::as_bytes(std::span(&object, 1)));
        if (!slot.valid) continue;
        cb.bindResource(0, gfx::BindingType::UniformBuffer, slot.handle.index,
                        slot.handle.generation);
        cb.drawIndexed(36);
    }
    cb.endRenderPass();

    cb.beginRenderPass(target);
    cb.setViewport(extent);
    cb.bindPipeline(deferredPipeline_);
    cb.bindResource(0, gfx::BindingType::Texture, albedoTexture_.index, albedoTexture_.generation);
    cb.bindResource(0, gfx::BindingType::Sampler, sampler_.index, sampler_.generation);
    cb.bindResource(1, gfx::BindingType::Texture, normalTexture_.index, normalTexture_.generation);
    cb.bindResource(1, gfx::BindingType::Sampler, sampler_.index, sampler_.generation);
    cb.draw(3);

    cb.bindPipeline(transparentPipeline_);
    for (std::uint32_t reverse = 0; reverse < 3; ++reverse) {
        cb.bindVertexBuffer(0, layerBuffers_[2u - reverse], sizeof(ScreenVertex));
        cb.draw(6);
    }
    cb.endRenderPass();
}

} // namespace f4::demo

#undef F4_VALIDATION_SHADER
