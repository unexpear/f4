#include "demos/demo_terrain.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <utility>

#include "core/resources/mesh.h"
#include "engine/world/terrain_meshing.h"
#include "graphics/command/command_buffer.h"
#include "graphics/pipeline/pipeline_state.h"
#include "graphics/shader/shader_desc.h"
#ifdef F4_GRAPHICS_BACKEND_metal
#include "demos/metal_demo_shaders.h"
#define F4_TERRAIN_SHADER(glslName, wgslName) f4::metal::wgslName
#elif defined(F4_GRAPHICS_BACKEND_webgpu)
#include "webgpu_shaders.h"
#define F4_TERRAIN_SHADER(glslName, wgslName) f4::web::wgslName
#else
#define F4_TERRAIN_SHADER(glslName, wgslName) glslName
#endif

namespace f4::demo {

namespace {

[[maybe_unused]] constexpr const char* kVs = R"glsl(
#version 430 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 3) in vec4 aColor;
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
    vec4 world = model * vec4(aPosition, 1.0);
    gl_Position = viewProj * world;
    vColor = aColor.rgb;
    vNormal = normalize(mat3(model) * aNormal);
    vWorldPos = world.xyz;
}
)glsl";

[[maybe_unused]] constexpr const char* kFs = R"glsl(
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
    vec3 N = normalize(vNormal);
    vec3 L = normalize(-lightDir);
    vec3 V = normalize(viewPos - vWorldPos);
    float diffuse = max(dot(N, L), 0.0);
    float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0);
    vec3 lit = vColor * (0.28 + 0.72 * diffuse) + vec3(0.08, 0.11, 0.14) * rim;
    fragColor = vec4(lit, 1.0);
}
)glsl";

struct TerrainUbo {
    math::Mat4 viewProj;
    math::Mat4 model;
    math::Vec3 lightDir;
    float lightPadding = 0.0f;
    math::Vec3 viewPos;
    float viewPadding = 0.0f;
};

static_assert(sizeof(TerrainUbo) == 160);

auto makeVoxelDemo(const world::BlockRegistry& blocks)
    -> Result<world::MeshData, world::TerrainMeshError> {
    world::VoxelCells cells;
    cells.values.resize(world::kVoxelCellCount);
    for (std::uint32_t z = 2; z < 14; ++z) {
        for (std::uint32_t x = 2; x < 14; ++x) {
            const std::uint32_t height = 2u + ((x / 3u + z / 3u) % 4u);
            for (std::uint32_t y = 0; y < height; ++y) {
                cells.values[world::voxelIndex(x, y, z)] =
                    y + 1u == height ? (height >= 4u ? 3u : 2u) : 1u;
            }
        }
    }
    auto result = world::meshVoxelChunk(cells, blocks);
    if (!result) return result.error();
    static constexpr std::array<std::array<std::uint8_t, 3>, 3> colors{{
        {{102, 111, 121}}, {{104, 151, 76}}, {{194, 137, 78}}}};
    for (world::TerrainVertex& vertex : result.value().vertices) {
        const auto& base = colors[std::min<std::size_t>(vertex.material, colors.size() - 1u)];
        const std::uint32_t shade = vertex.color[0];
        vertex.color = {{
            static_cast<std::uint8_t>((base[0] * shade) / 255u),
            static_cast<std::uint8_t>((base[1] * shade) / 255u),
            static_cast<std::uint8_t>((base[2] * shade) / 255u), 255}};
    }
    return static_cast<world::MeshData&&>(result.value());
}

} // namespace

bool TerrainScene::init(gfx::IDevice* dev, platform::Window*) {
    if (inited_) return true;
    if (dev == nullptr) return false;
    device_ = dev;
    inited_ = true;
    const auto fail = [&] {
        shutdown(dev);
        return false;
    };

    world::BlockRegistry blocks;
    if (!blocks.setEngineBlock(3, {true, true, 2, 1.5f})) return fail();
    auto voxel = makeVoxelDemo(blocks);
    world::HeightTile heightTile = world::generateHeightTile(0x4d3135u, {}, {});
    for (std::uint32_t z = 0; z < world::kHeightSamples; ++z) {
        for (std::uint32_t x = 0; x < world::kHeightSamples; ++x) {
            world::HeightSample& sample =
                heightTile.samples[world::heightSampleIndex(x, z)];
            const std::uint32_t band = std::min(x / 16u, 3u);
            const std::uint32_t nextBand = std::min(band + 1u, 3u);
            const std::uint8_t blend = static_cast<std::uint8_t>((x % 16u) * 12u);
            sample.materialWeights = {{0, 0, 0, 0}};
            sample.materialWeights[band] = static_cast<std::uint8_t>(255u - blend);
            sample.materialWeights[nextBand] = static_cast<std::uint8_t>(
                sample.materialWeights[nextBand] + blend);
            sample.biomeId = static_cast<std::uint16_t>(band);
        }
    }
    world::HeightMeshingOptions heightOptions;
    heightOptions.skirtDepth = 6.0f;
    auto height = world::meshHeightChunk(heightTile, {}, heightOptions);
    world::HexPolicyParams hexParams;
    hexParams.radius = 4;
    world::HexTile hexTile = world::generateHexTile(0x4d3135u, {}, hexParams);
    const std::int32_t hexRadius = static_cast<std::int32_t>(hexTile.radius);
    for (std::int32_t r = -hexRadius; r <= hexRadius; ++r) {
        for (std::int32_t q = -hexRadius; q <= hexRadius; ++q) {
            const auto index = hexTile.index({q, r});
            if (index.has_value()) {
                hexTile.cells[*index].terrainType =
                    static_cast<std::uint8_t>((q - r + 24) % 3);
            }
        }
    }
    world::HexMeshingOptions hexOptions;
    hexOptions.floorHeight = -9.0f;
    auto hex = world::meshHexChunk(hexTile, {}, hexOptions);
    if (!voxel || !height || !hex) return fail();

    const std::array<const world::MeshData*, 3> sources{{
        &voxel.value(), &height.value(), &hex.value()}};
    meshes_[0].model = math::Mat4::translation({-7.3f, -1.2f, -2.4f}).mul(
        math::Mat4::scaling({0.32f, 0.32f, 0.32f}));
    meshes_[1].model = math::Mat4::translation({-2.56f, -1.6f, -2.56f}).mul(
        math::Mat4::scaling({0.08f, 0.08f, 0.08f}));
    meshes_[2].model = math::Mat4::translation({5.5f, 0.0f, 0.0f}).mul(
        math::Mat4::scaling({0.42f, 0.42f, 0.42f}));

    for (std::size_t i = 0; i < sources.size(); ++i) {
        const resources::Mesh mesh = world::toResourceMesh(*sources[i]);
        if (mesh.streams.size() != 1 || mesh.streams[0].data.isEmpty() ||
            mesh.indices.isEmpty() ||
            mesh.streams[0].data.size() > std::numeric_limits<std::uint32_t>::max() ||
            mesh.indices.size() > std::numeric_limits<std::uint32_t>::max() ||
            mesh.indexCount() > std::numeric_limits<std::uint32_t>::max()) {
            return fail();
        }
        gfx::BufferDesc vertexDesc;
        vertexDesc.sizeBytes = static_cast<std::uint32_t>(mesh.streams[0].data.size());
        vertexDesc.usage = gfx::BufferUsage::Vertex;
        auto vertex = dev->createBuffer(
            vertexDesc, std::span(mesh.streams[0].data.data(), mesh.streams[0].data.size()));
        if (!vertex) return fail();
        meshes_[i].vertexBuffer = vertex.value();

        gfx::BufferDesc indexDesc;
        indexDesc.sizeBytes = static_cast<std::uint32_t>(mesh.indices.size());
        indexDesc.usage = gfx::BufferUsage::Index;
        auto index = dev->createBuffer(
            indexDesc, std::span(mesh.indices.data(), mesh.indices.size()));
        if (!index) return fail();
        meshes_[i].indexBuffer = index.value();
        meshes_[i].indexFormat = mesh.indexFormat == resources::IndexFormat::U16
            ? gfx::IndexFormat::U16
            : gfx::IndexFormat::U32;
        meshes_[i].indexCount = static_cast<std::uint32_t>(mesh.indexCount());
    }

    if (constRing_.create(*dev, sizeof(TerrainUbo), 3) != gfx::GfxError::None) {
        return fail();
    }
    const auto makeShader = [&](gfx::ShaderStage stage, const char* source) {
        gfx::ShaderDesc desc;
        desc.stage = stage;
#ifdef F4_GRAPHICS_BACKEND_metal
        std::ifstream stream(source, std::ios::binary | std::ios::ate);
        if (!stream.good()) return dev->createShader(desc);
        const std::streamsize size = stream.tellg();
        if (size <= 0) return dev->createShader(desc);
        stream.seekg(0);
        desc.blob.resize(static_cast<std::size_t>(size));
        stream.read(reinterpret_cast<char*>(desc.blob.data()), size);
        if (!stream.good()) desc.blob.clear();
#else
        desc.blob.resize(std::strlen(source));
        std::memcpy(desc.blob.data(), source, desc.blob.size());
#endif
        return dev->createShader(desc);
    };
    auto vertexShader = makeShader(
        gfx::ShaderStage::Vertex,
        F4_TERRAIN_SHADER(kVs, kValidationTerrainVertexWgsl));
    auto fragmentShader = makeShader(
        gfx::ShaderStage::Fragment,
        F4_TERRAIN_SHADER(kFs, kValidationTerrainFragmentWgsl));
    if (!vertexShader || !fragmentShader) return fail();
    vs_ = vertexShader.value();
    fs_ = fragmentShader.value();

    gfx::PipelineStateDesc pipeline;
    pipeline.vertexShader = vs_;
    pipeline.fragmentShader = fs_;
    pipeline.topology = gfx::PrimitiveTopology::TriangleList;
    pipeline.raster.cullMode = gfx::CullMode::Back;
    pipeline.depthStencil.depthTestEnable = true;
    pipeline.depthStencil.depthWriteEnable = true;
    pipeline.depthStencil.depthCompare = gfx::CompareOp::Less;
    pipeline.vertexInput.bindings.push({0, 36, 0});
    pipeline.vertexInput.attributes.push({0, 0, gfx::VertexFormat::Float32x3, 0});
    pipeline.vertexInput.attributes.push({0, 1, gfx::VertexFormat::Float32x3, 12});
    pipeline.vertexInput.attributes.push({0, 3, gfx::VertexFormat::UNorm8x4, 32});
    gfx::BindGroup resources;
    resources.slots.push({
        0, gfx::BindingType::UniformBuffer,
        gfx::shaderStageBit(gfx::ShaderStage::Vertex) |
            gfx::shaderStageBit(gfx::ShaderStage::Fragment),
        1});
    pipeline.layout.groups.push(std::move(resources));
    auto createdPipeline = dev->createPipeline(pipeline);
    if (!createdPipeline) return fail();
    pipeline_ = createdPipeline.value();
    return true;
}

void TerrainScene::shutdown(gfx::IDevice* dev) {
    if (!inited_) return;
    if (pipeline_.isValid()) (void)dev->destroyPipeline(pipeline_);
    if (fs_.isValid()) (void)dev->destroyShader(fs_);
    if (vs_.isValid()) (void)dev->destroyShader(vs_);
    for (TerrainGpuMesh& mesh : meshes_) {
        if (mesh.indexBuffer.isValid()) (void)dev->destroyBuffer(mesh.indexBuffer);
        if (mesh.vertexBuffer.isValid()) (void)dev->destroyBuffer(mesh.vertexBuffer);
        mesh = {};
    }
    constRing_.destroy(*dev);
    pipeline_ = {};
    fs_ = {};
    vs_ = {};
    device_ = nullptr;
    inited_ = false;
}

void TerrainScene::onResize(std::uint32_t, std::uint32_t) {}

void TerrainScene::update(float, const Uint8*, const SDL_MouseMotionEvent*) {}

void TerrainScene::render(gfx::CommandBuffer& cb, const FreeCamera& camera,
                          std::uint32_t width, std::uint32_t height) {
    if (!inited_ || height == 0) return;
    constRing_.beginFrame();
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const math::Mat4 viewProj = camera.viewProjMatrix(aspect);
    const math::Vec3 lightDir = math::normalized({0.45f, -1.0f, 0.3f});
    cb.bindPipeline(pipeline_);
    for (const TerrainGpuMesh& mesh : meshes_) {
        TerrainUbo ubo;
        ubo.viewProj = viewProj;
        ubo.model = mesh.model;
        ubo.lightDir = lightDir;
        ubo.viewPos = camera.position;
        const auto slot = constRing_.writeAndBind(
            *device_, cb,
            std::span(reinterpret_cast<const std::byte*>(&ubo), sizeof(ubo)));
        if (!slot.valid) return;
        cb.bindVertexBuffer(0, mesh.vertexBuffer, 36);
        cb.bindIndexBuffer(mesh.indexBuffer, mesh.indexFormat);
        cb.bindResource(0, gfx::BindingType::UniformBuffer,
                        slot.handle.index, slot.handle.generation);
        cb.drawIndexed(mesh.indexCount, 1);
    }
}

} // namespace f4::demo

#undef F4_TERRAIN_SHADER
