#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>

#include "core/containers/array.h"
#include "core/memory/serialization.h"
#include "core/resources/asset_file.h"
#include "core/resources/file_io.h"
#include "core/resources/load_error.h"
#include "core/resources/resource_cache.h"

using namespace f4;
using resources::AssetReader;
using resources::AssetWriter;
using resources::IndexFormat;
using resources::Mesh;
using resources::SubMesh;
using resources::Texture;
using resources::TextureFormat;
using resources::VertexAttribute;
using resources::VertexFormat;
using resources::VertexSemantic;
using resources::VertexStream;

namespace {
auto quadMesh() -> Mesh {
    Mesh mesh;

    VertexStream positions;
    positions.strideBytes = 12u;
    const float xyz[4][3] = {{0.0f, 0.0f, 0.0f},
                             {2.0f, 0.0f, 0.0f},
                             {2.0f, 4.0f, 0.0f},
                             {0.0f, 4.0f, 0.0f}};
    positions.data.resize(sizeof(xyz));
    std::memcpy(positions.data.data(), xyz, sizeof(xyz));
    mesh.streams.push(static_cast<VertexStream&&>(positions));

    VertexStream uvs;
    uvs.strideBytes = 8u;
    const float uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    uvs.data.resize(sizeof(uv));
    std::memcpy(uvs.data.data(), uv, sizeof(uv));
    mesh.streams.push(static_cast<VertexStream&&>(uvs));

    VertexAttribute posAttr;
    posAttr.semantic = VertexSemantic::Position;
    posAttr.format = VertexFormat::F32_3;
    posAttr.stream = 0u;
    posAttr.offsetBytes = 0u;
    mesh.attributes.push(posAttr);

    VertexAttribute uvAttr;
    uvAttr.semantic = VertexSemantic::TexCoord0;
    uvAttr.format = VertexFormat::F32_2;
    uvAttr.stream = 1u;
    uvAttr.offsetBytes = 0u;
    mesh.attributes.push(uvAttr);

    const std::uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    mesh.indexFormat = IndexFormat::U16;
    mesh.indices.resize(sizeof(idx));
    std::memcpy(mesh.indices.data(), idx, sizeof(idx));

    SubMesh halfA{0u, 3u};
    SubMesh halfB{3u, 3u};
    mesh.submeshes.push(halfA);
    mesh.submeshes.push(halfB);

    mesh.bounds = resources::computeBounds(mesh);
    return mesh;
}
} // namespace

TEST_CASE("families are independent and handles are stale-safe") {
    resources::ResourceCache cache;

    const auto meshHandle =
        cache.create<resources::MeshTag>(static_cast<Mesh&&>(quadMesh()));
    const auto texHandle = cache.create<resources::TextureTag>();

    CHECK(meshHandle.isValid());
    CHECK(texHandle.isValid());
    CHECK(cache.liveCount<resources::MeshTag>() == 1u);
    CHECK(cache.liveCount<resources::TextureTag>() == 1u);

    auto* mesh = cache.get(meshHandle);
    REQUIRE(mesh != nullptr);
    CHECK(mesh->vertexCount(0u) == 4u);

    CHECK(cache.destroy(meshHandle));
    CHECK(!cache.contains(meshHandle));

    const auto recycled =
        cache.create<resources::MeshTag>(static_cast<Mesh&&>(quadMesh()));
    CHECK(recycled.index == meshHandle.index);
    CHECK(recycled.generation != meshHandle.generation);

    CHECK(!cache.contains(meshHandle));
    CHECK(cache.get(meshHandle) == nullptr);
    CHECK(cache.get(recycled) != nullptr);
    CHECK(!cache.destroy(meshHandle));
}

TEST_CASE("path registration is exclusive and follows destroy") {
    resources::ResourceCache cache;

    const auto first = cache.createNamed<resources::ShaderTag>("shaders/basic.vert");
    CHECK(first.isValid());
    CHECK(cache.find<resources::ShaderTag>("shaders/basic.vert") == first);
    CHECK(cache.get(first) != nullptr);

    const auto rejected = cache.createNamed<resources::ShaderTag>("shaders/basic.vert");
    CHECK(!rejected.isValid());
    CHECK(cache.liveCount<resources::ShaderTag>() == 1u);

    CHECK(!cache.find<resources::ShaderTag>("shaders/Basic.vert").isValid());
    CHECK(!cache.find<resources::TextureTag>("shaders/basic.vert").isValid());

    CHECK(cache.destroy(first));
    CHECK(!cache.find<resources::ShaderTag>("shaders/basic.vert").isValid());

    const auto reReg = cache.createNamed<resources::ShaderTag>("shaders/basic.vert");
    CHECK(reReg.isValid());
    CHECK(reReg != first);
}

TEST_CASE("getOrLoad caches successes and preserves loader errors") {
    resources::ResourceCache cache;
    int loadCalls = 0;

    const auto first = cache.getOrLoad<resources::MeshTag>(
        "meshes/generated.mesh", [&]() -> Result<Mesh, resources::LoadError> {
            ++loadCalls;
            return quadMesh();
        });
    REQUIRE(first.hasValue());
    CHECK(loadCalls == 1);
    CHECK(cache.contains(first.value()));

    const auto second = cache.getOrLoad<resources::MeshTag>(
        "meshes/generated.mesh", [&]() -> Result<Mesh, resources::LoadError> {
            ++loadCalls;
            return resources::LoadError::BadSignature;
        });
    REQUIRE(second.hasValue());
    CHECK(second.value() == first.value());
    CHECK(loadCalls == 1);

    const auto failed = cache.getOrLoad<resources::MeshTag>(
        "meshes/broken.mesh", [&]() -> Result<Mesh, resources::LoadError> {
            ++loadCalls;
            return resources::LoadError::BadSignature;
        });
    REQUIRE(failed.hasError());
    CHECK(failed.error() == resources::LoadError::BadSignature);
    CHECK(loadCalls == 2);
    CHECK(!cache.find<resources::MeshTag>("meshes/broken.mesh").isValid());
    CHECK(cache.liveCount<resources::MeshTag>() == 1u);
}

TEST_CASE("M16: shared resource references keep streamed records alive until final release") {
    resources::ResourceCache cache;
    const auto first = cache.getOrLoad<resources::MeshTag>(
        "meshes/shared.mesh", []() -> Result<Mesh, resources::LoadError> {
            return quadMesh();
        });
    REQUIRE(first.hasValue());
    const auto second = cache.getOrLoad<resources::MeshTag>(
        "meshes/shared.mesh", []() -> Result<Mesh, resources::LoadError> {
            return resources::LoadError::BadSignature;
        });
    REQUIRE(second.hasValue());
    CHECK(second.value() == first.value());
    CHECK(cache.referenceCount(first.value()) == 2);

    REQUIRE(cache.release(first.value()));
    CHECK(cache.contains(second.value()));
    CHECK(cache.referenceCount(second.value()) == 1);
    CHECK(cache.find<resources::MeshTag>("meshes/shared.mesh") == second.value());
    REQUIRE(cache.destroy(second.value()));
    CHECK_FALSE(cache.contains(second.value()));
    CHECK(cache.referenceCount(second.value()) == 0);
    CHECK_FALSE(cache.retain(second.value()));
}

TEST_CASE("clear empties every family and bumps generations") {
    resources::ResourceCache cache;
    const auto mesh = cache.createNamed<resources::MeshTag>(
        "meshes/a.mesh", static_cast<Mesh&&>(quadMesh()));
    const auto tex = cache.createNamed<resources::TextureTag>("textures/a.tex");

    cache.clear();

    CHECK(cache.liveCount<resources::MeshTag>() == 0u);
    CHECK(cache.liveCount<resources::TextureTag>() == 0u);
    CHECK(!cache.contains(mesh));
    CHECK(!cache.contains(tex));
    CHECK(!cache.find<resources::MeshTag>("meshes/a.mesh").isValid());
}

TEST_CASE("mesh records expose layout, submeshes, and bounds") {
    const Mesh mesh = quadMesh();

    CHECK(mesh.vertexCount(0u) == 4u);
    CHECK(mesh.vertexCount(1u) == 4u);
    CHECK(mesh.indexCount() == 6u);
    CHECK(mesh.submeshes.size() == 2u);
    CHECK(mesh.submeshes[0u].firstIndex == 0u);
    CHECK(mesh.submeshes[1u].firstIndex == 3u);

    const VertexAttribute* pos = mesh.attributeFor(VertexSemantic::Position);
    REQUIRE(pos != nullptr);
    CHECK(pos->stream == 0u);
    CHECK(resources::vertexFormatSize(pos->format) == 12u);
    CHECK(mesh.attributeFor(VertexSemantic::Normal) == nullptr);

    CHECK(mesh.bounds.min.x == 0.0f);
    CHECK(mesh.bounds.min.y == 0.0f);
    CHECK(mesh.bounds.max.x == 2.0f);
    CHECK(mesh.bounds.max.y == 4.0f);
    CHECK(mesh.bounds.max.z == 0.0f);

    Mesh positionless;
    positionless.bounds = resources::computeBounds(positionless);
    CHECK(!positionless.bounds.isValid());

    CHECK(resources::indexFormatSize(IndexFormat::U16) == 2u);
    CHECK(resources::indexFormatSize(IndexFormat::U32) == 4u);
}

TEST_CASE("texture records describe formats and mip chains") {
    Texture texture;
    texture.format = TextureFormat::RGBA8_UNORM;

    texture.mips.push(resources::TextureMip{4u, 4u, {}});
    texture.mips.push(resources::TextureMip{2u, 2u, {}});
    texture.mips.push(resources::TextureMip{1u, 1u, {}});

    REQUIRE(texture.mips.size() == 3u);
    CHECK(texture.mips[0u].width * texture.mips[0u].height * 4u == 64u);
    CHECK(texture.mips[2u].width == 1u);

    CHECK(!resources::isBlockCompressed(TextureFormat::RGBA8_UNORM));
    CHECK(!resources::isBlockCompressed(TextureFormat::RGBA16_FLOAT));
    CHECK(resources::isBlockCompressed(TextureFormat::BC7_UNORM));
    CHECK(resources::isBlockCompressed(TextureFormat::ASTC_4X4_UNORM));
}

TEST_CASE("mesh byte accounting sums streams and indices") {
    const Mesh mesh = quadMesh();
    // streams: positions 4*12=48 + uvs 4*8=32; indices: 6*2=12 (U16).
    CHECK(resources::meshVertexBytes(mesh) == 48u + 32u);
    CHECK(resources::meshIndexBytes(mesh) == 12u);
    CHECK(resources::meshBytes(mesh) == 48u + 32u + 12u);
}

TEST_CASE("texture byte accounting and slice-size estimates") {
    auto makeMip = [](std::size_t bytes) -> containers::Array<std::byte> {
        containers::Array<std::byte> a;
        a.resize(bytes);
        return a;
    };
    Texture texture;
    texture.format = TextureFormat::RGBA8_UNORM;
    texture.mips.push(resources::TextureMip{4u, 4u, makeMip(64u)});
    texture.mips.push(resources::TextureMip{2u, 2u, makeMip(16u)});
    texture.mips.push(resources::TextureMip{1u, 1u, makeMip(4u)});
    CHECK(resources::textureBytes(texture) == 64u + 16u + 4u);

    CHECK(resources::textureSliceBytes(4u, 4u, TextureFormat::RGBA8_UNORM) == 64u);
    // BC7: 8x8 texels = 2x2 blocks of 4x4, 16 bytes each.
    CHECK(resources::textureSliceBytes(8u, 8u, TextureFormat::BC7_UNORM) == 4u * 16u);
    CHECK(resources::textureSliceBytes(2u, 2u, TextureFormat::RGBA32_FLOAT) ==
          4u * 16u);
}

TEST_CASE("buffer descriptors compose usage bit flags") {
    using resources::BufferUsage;
    const resources::BufferDesc desc{512u, BufferUsage::Vertex | BufferUsage::TransferDst};

    CHECK(desc.sizeBytes == 512u);
    CHECK(resources::hasUsage(desc.usage, BufferUsage::Vertex));
    CHECK(resources::hasUsage(desc.usage, BufferUsage::TransferDst));
    CHECK(!resources::hasUsage(desc.usage, BufferUsage::Index));
}

TEST_CASE(".f4asset round-trip preserves chunk id/version/payload") {
    struct BlobHeader {
        std::uint32_t kind;
        float weight;
    };

    memory::MemoryWriter storage;
    AssetWriter writer(storage);
    writer.begin();

    const BlobHeader header{7u, 0.25f};
    const std::uint8_t payload[5] = {9u, 8u, 7u, 6u, 5u};
    REQUIRE(writer.addChunk(101u, 1u, &header, sizeof(header)));
    REQUIRE(writer.addChunk(202u, 3u, payload, sizeof(payload)));

    AssetReader reader(storage.bytes());
    CHECK(reader.begin());

    memory::ChunkView chunk;
    REQUIRE(reader.next(chunk));
    CHECK(chunk.id == 101u);
    CHECK(chunk.version == 1u);
    CHECK(chunk.payloadSize == sizeof(header));
    BlobHeader decoded{};
    std::memcpy(&decoded, chunk.payload, sizeof(decoded));
    CHECK(decoded.kind == 7u);
    CHECK(decoded.weight == 0.25f);

    REQUIRE(reader.next(chunk));
    CHECK(chunk.id == 202u);
    CHECK(chunk.version == 3u);
    CHECK(chunk.payloadSize == 5u);
    CHECK(std::memcmp(chunk.payload, payload, sizeof(payload)) == 0);

    CHECK(!reader.next(chunk));
    CHECK(reader.cleanlyFinished());
    CHECK(!reader.error());
}

TEST_CASE(".f4asset rejects corruption") {
    memory::MemoryWriter storage;
    AssetWriter writer(storage);
    writer.begin();
    const std::uint32_t value = 42u;
    REQUIRE(writer.addChunk(1u, 1u, &value, sizeof(value)));

    SUBCASE("wrong magic") {
        containers::Array<std::byte> bad(storage.bytes());
        bad[4] = static_cast<std::byte>(0xFFu);
        AssetReader reader(bad);
        CHECK(!reader.begin());
        CHECK(reader.error());
    }
    SUBCASE("wrong format version") {
        containers::Array<std::byte> bad(storage.bytes());
        bad[8] = static_cast<std::byte>(0x99u);
        AssetReader reader(bad);
        CHECK(!reader.begin());
        CHECK(reader.error());
    }
    SUBCASE("trailing garbage is an error") {
        containers::Array<std::byte> bad(storage.bytes());
        bad.resize(bad.size() + 2u);
        AssetReader reader(bad);
        REQUIRE(reader.begin());
        memory::ChunkView chunk;
        REQUIRE(reader.next(chunk));
        CHECK(!reader.next(chunk));
        CHECK(reader.error());
        CHECK(!reader.cleanlyFinished());
    }
    SUBCASE("truncated payload") {
        containers::Array<std::byte> bad(storage.bytes());
        bad.resize(bad.size() - 2u);
        AssetReader reader(bad);
        REQUIRE(reader.begin());
        memory::ChunkView chunk;
        CHECK(!reader.next(chunk));
        CHECK(reader.error());
    }
    SUBCASE("next before begin") {
        AssetReader reader(storage.bytes());
        memory::ChunkView chunk;
        CHECK(!reader.next(chunk));
    }
}

TEST_CASE("binary file IO round-trips through disk") {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "f4_resource_tests";
    std::filesystem::create_directories(dir);
    const std::string filePath = (dir / "io_probe.bin").string();

    const std::uint8_t written[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(resources::writeBinaryFile(filePath.c_str(), written, sizeof(written)) ==
          resources::FileIoError::None);

    containers::Array<std::byte> readBack;
    CHECK(resources::readBinaryFile(filePath.c_str(), readBack) == resources::FileIoError::None);
    REQUIRE(readBack.size() == sizeof(written));
    CHECK(std::memcmp(readBack.data(), written, sizeof(written)) == 0);

    CHECK(resources::readBinaryFile((dir / "missing.bin").string().c_str(), readBack) ==
          resources::FileIoError::NotFound);
    CHECK(readBack.isEmpty());

    CHECK(resources::writeBinaryFile((dir / "empty.bin").string().c_str(), nullptr, 0u) ==
          resources::FileIoError::None);
    CHECK(resources::readBinaryFile((dir / "empty.bin").string().c_str(), readBack) ==
          resources::FileIoError::None);
    CHECK(readBack.isEmpty());

    std::filesystem::remove(dir / "io_probe.bin");
    std::filesystem::remove(dir / "empty.bin");
}

namespace {
constexpr std::uint32_t kChunkMeshHeader = 1u;
constexpr std::uint32_t kChunkPositions = 2u;
constexpr std::uint32_t kChunkIndices = 3u;

struct MeshFileHeader {
    std::uint32_t vertexCount;
    std::uint32_t streamStride;
    std::uint32_t indexFormat;
    std::uint32_t submeshCount;
};

auto serializeMesh(const Mesh& mesh, memory::MemoryWriter& out) -> bool {
    resources::AssetWriter writer(out);
    writer.begin();

    const MeshFileHeader header{static_cast<std::uint32_t>(mesh.vertexCount(0u)),
                                mesh.streams[0u].strideBytes,
                                static_cast<std::uint32_t>(mesh.indexFormat),
                                static_cast<std::uint32_t>(mesh.submeshes.size())};
    if (!writer.addChunk(kChunkMeshHeader, 1u, &header, sizeof(header))) {
        return false;
    }
    if (!writer.addChunk(kChunkPositions, 1u, mesh.streams[0u].data.data(),
                         mesh.streams[0u].data.size())) {
        return false;
    }
    return writer.addChunk(kChunkIndices, 1u, mesh.indices.data(), mesh.indices.size());
}
} // namespace

TEST_CASE("end-to-end: mesh saved to .f4asset reloads identically") {
    const Mesh original = quadMesh();
    REQUIRE(original.bounds.isValid());

    memory::MemoryWriter storage;
    REQUIRE(serializeMesh(original, storage));

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "f4_resource_tests";
    std::filesystem::create_directories(dir);
    const std::string assetPath = (dir / "quad.f4asset").string();
    REQUIRE(resources::writeBinaryFile(assetPath.c_str(), storage.bytes().data(),
                                       storage.bytes().size()) ==
            resources::FileIoError::None);

    resources::ResourceCache cache;
    containers::Array<std::byte> image;
    REQUIRE(resources::readBinaryFile(assetPath.c_str(), image) == resources::FileIoError::None);

    AssetReader reader(image);
    REQUIRE(reader.begin());

    Mesh loaded;
    bool sawHeader = false;
    bool sawPositions = false;
    bool sawIndices = false;
    memory::ChunkView chunk;
    while (reader.next(chunk)) {
        if (chunk.id == kChunkMeshHeader && chunk.version == 1u &&
            chunk.payloadSize == sizeof(MeshFileHeader)) {
            MeshFileHeader header{};
            std::memcpy(&header, chunk.payload, sizeof(header));
            VertexStream stream;
            stream.strideBytes = header.streamStride;
            loaded.streams.push(static_cast<VertexStream&&>(stream));
            loaded.indexFormat = static_cast<IndexFormat>(header.indexFormat);
            sawHeader = true;
        } else if (chunk.id == kChunkPositions && chunk.version == 1u && sawHeader) {
            loaded.streams[0u].data.resize(chunk.payloadSize);
            std::memcpy(loaded.streams[0u].data.data(), chunk.payload, chunk.payloadSize);
            sawPositions = true;
        } else if (chunk.id == kChunkIndices && chunk.version == 1u && sawHeader) {
            loaded.indices.resize(chunk.payloadSize);
            std::memcpy(loaded.indices.data(), chunk.payload, chunk.payloadSize);
            sawIndices = true;
        }
    }
    CHECK(!reader.error());
    CHECK(sawHeader);
    CHECK(sawPositions);
    CHECK(sawIndices);

    VertexAttribute posAttr;
    posAttr.semantic = VertexSemantic::Position;
    posAttr.format = VertexFormat::F32_3;
    loaded.attributes.push(posAttr);

    loaded.bounds = resources::computeBounds(loaded);

    const auto handle = cache.createNamed<resources::MeshTag>(
        assetPath.c_str(), static_cast<Mesh&&>(loaded));
    REQUIRE(handle.isValid());
    CHECK(cache.find<resources::MeshTag>(assetPath.c_str()) == handle);

    auto* stored = cache.get(handle);
    REQUIRE(stored != nullptr);
    CHECK(stored->vertexCount(0u) == original.vertexCount(0u));
    CHECK(stored->indexCount() == original.indexCount());
    CHECK(stored->indexFormat == original.indexFormat);
    CHECK(stored->bounds.min.x == original.bounds.min.x);
    CHECK(stored->bounds.max.y == original.bounds.max.y);
    CHECK(std::memcmp(stored->streams[0u].data.data(),
                      original.streams[0u].data.data(),
                      original.streams[0u].data.size()) == 0);
    CHECK(std::memcmp(stored->indices.data(), original.indices.data(),
                      original.indices.size()) == 0);

    std::filesystem::remove(dir / "quad.f4asset");
}
