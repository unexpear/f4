#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "core/resources/gltf_loader.h"

using namespace f4;
using resources::GltfScene;
using resources::LoadError;
using resources::loadGltf;

namespace {
auto assembleGlb(const char* json, const std::byte* binary, std::size_t binarySize)
    -> std::vector<std::byte> {
    const std::size_t jsonLen = std::strlen(json);
    const std::size_t jsonPadded = (jsonLen + 3) & ~std::size_t{3};
    const std::size_t binaryPadded = (binarySize + 3) & ~std::size_t{3};
    const std::size_t totalLen = 12 + 8 + jsonPadded + 8 + binaryPadded;
    std::vector<std::byte> result(totalLen);
    std::size_t offset = 0;

    const std::uint32_t magic = 0x46546C67;
    const std::uint32_t version = 2;
    const auto length = static_cast<std::uint32_t>(totalLen);
    std::memcpy(result.data() + offset, &magic, 4); offset += 4;
    std::memcpy(result.data() + offset, &version, 4); offset += 4;
    std::memcpy(result.data() + offset, &length, 4); offset += 4;

    const auto jsonChunkLength = static_cast<std::uint32_t>(jsonPadded);
    const std::uint32_t jsonType = 0x4E4F534A;
    std::memcpy(result.data() + offset, &jsonChunkLength, 4); offset += 4;
    std::memcpy(result.data() + offset, &jsonType, 4); offset += 4;
    std::memcpy(result.data() + offset, json, jsonLen); offset += jsonLen;
    while (offset < 12 + 8 + jsonPadded) {
        result[offset++] = std::byte{' '};
    }

    const auto binaryChunkLength = static_cast<std::uint32_t>(binaryPadded);
    const std::uint32_t binaryType = 0x004E4942;
    std::memcpy(result.data() + offset, &binaryChunkLength, 4); offset += 4;
    std::memcpy(result.data() + offset, &binaryType, 4); offset += 4;
    std::memcpy(result.data() + offset, binary, binarySize);
    return result;
}

auto textBytes(const char* text) -> std::vector<std::byte> {
    const std::size_t size = std::strlen(text);
    std::vector<std::byte> result(size);
    std::memcpy(result.data(), text, size);
    return result;
}

// Build a minimal valid GLB with one mesh: a single triangle.
// Layout: header (12) + JSON chunk (4+4+N) + BIN chunk (4+4+M).
// BIN data: 3×vec3 positions (36 bytes) + 3×uint16 indices (6 bytes) = 42 bytes.
auto makeTriangleGlb(bool indexed = true) -> std::vector<std::byte> {
    // Binary payload: 3 positions + 3 indices
    const float positions[9] = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
    };
    const std::uint16_t indices[3] = {0, 1, 2};

    // JSON referencing the BIN chunk (buffer 0).
    const char* indexedJson =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
        R"("nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{)"
        R"("POSITION":0},"indices":1}]}],"accessors":[{"bufferView":0,)"
        R"("componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],)"
        R"("max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,)"
        R"("type":"SCALAR"}],"bufferViews":[{"buffer":0,"byteOffset":0,)"
        R"("byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],)"
        R"("buffers":[{"byteLength":42}]})";
    const char* nonIndexedJson =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
        R"("nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{)"
        R"("POSITION":0}}]}],"accessors":[{"bufferView":0,"componentType":5126,)"
        R"("count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
        R"("buffers":[{"byteLength":36}]})";
    const char* json = indexed ? indexedJson : nonIndexedJson;

    const std::size_t jsonLen = std::strlen(json);
    const std::size_t jsonPadded = (jsonLen + 3) & ~std::size_t{3};

    // BIN: positions + indices, padded to 4-byte alignment.
    const std::size_t posBytes = sizeof(positions);
    const std::size_t idxBytes = indexed ? sizeof(indices) : 0;
    const std::size_t binLen = posBytes + idxBytes;
    const std::size_t binPadded = (binLen + 3) & ~std::size_t{3};

    const std::size_t totalLen = 12 + 8 + jsonPadded + 8 + binPadded;

    std::vector<std::byte> buf(totalLen);
    std::size_t off = 0;

    // GLB header.
    const std::uint32_t magic = 0x46546C67;
    const std::uint32_t version = 2;
    const auto len = static_cast<std::uint32_t>(totalLen);
    std::memcpy(buf.data() + off, &magic, 4); off += 4;
    std::memcpy(buf.data() + off, &version, 4); off += 4;
    std::memcpy(buf.data() + off, &len, 4); off += 4;

    // JSON chunk.
    auto jLen = static_cast<std::uint32_t>(jsonPadded);
    const std::uint32_t jsonType = 0x4E4F534A;
    std::memcpy(buf.data() + off, &jLen, 4); off += 4;
    std::memcpy(buf.data() + off, &jsonType, 4); off += 4;
    std::memcpy(buf.data() + off, json, jsonLen); off += jsonLen;
    // Pad with spaces.
    for (std::size_t i = jsonLen; i < jsonPadded; ++i) {
        buf[off++] = std::byte{' '};
    }

    // BIN chunk.
    auto bLen = static_cast<std::uint32_t>(binPadded);
    const std::uint32_t binType = 0x004E4942;
    std::memcpy(buf.data() + off, &bLen, 4); off += 4;
    std::memcpy(buf.data() + off, &binType, 4); off += 4;
    std::memcpy(buf.data() + off, positions, posBytes); off += posBytes;
    if (indexed) {
        std::memcpy(buf.data() + off, indices, idxBytes); off += idxBytes;
    }
    // Pad with zeros.
    for (std::size_t i = binLen; i < binPadded; ++i) {
        buf[off++] = std::byte{0};
    }

    return buf;
}

// Build a GLB with two separate triangle primitives in one mesh.
auto makeTwoPrimitiveGlb() -> std::vector<std::byte> {
    const float positions[12] = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        2.0f, 0.0f, 0.0f,
    };
    const std::uint16_t texcoords[8] = {
        0, 0,
        65535, 0,
        0, 65535,
        65535, 65535,
    };
    const std::uint16_t indices0[3] = {0, 1, 2};
    const std::uint16_t indices1[3] = {1, 3, 2};

    const char* json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
        R"("nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{)"
        R"("POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0},{"attributes":{)"
        R"("POSITION":0,"TEXCOORD_0":1},"indices":3,"material":1}]}],)"
        R"("materials":[{"name":"Paint",)"
        R"("pbrMetallicRoughness":{"baseColorFactor":[0.2,0.4,0.6,0.8],)"
        R"("baseColorTexture":{"index":0,"texCoord":1},"metallicFactor":0.25,)"
        R"("roughnessFactor":0.75,"metallicRoughnessTexture":{"index":1}},)"
        R"("normalTexture":{"index":2,"texCoord":1,"scale":0.5},)"
        R"("occlusionTexture":{"index":3,"strength":0.25},)"
        R"("emissiveTexture":{"index":4},"emissiveFactor":[0.1,0.2,0.3],)"
        R"("alphaMode":"MASK","alphaCutoff":0.3,"doubleSided":true},)"
        R"({"name":"Glass","alphaMode":"BLEND"}],"textures":[{"source":0},)"
        R"({"source":0},{"source":0},{"source":0},{"source":0}],)"
        R"("images":[{"uri":"surface.png"}],"accessors":[{"bufferView":0,"componentType":5126,)"
        R"("count":4,"type":"VEC3","min":[0,0,0],"max":[2,1,0]},)"
        R"({"bufferView":1,"componentType":5123,"normalized":true,"count":4,)"
        R"("type":"VEC2"},{"bufferView":2,"componentType":5123,"count":3,)"
        R"("type":"SCALAR"},{"bufferView":3,"componentType":5123,"count":3,)"
        R"("type":"SCALAR"}],"bufferViews":[{"buffer":0,"byteOffset":0,)"
        R"("byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":16},)"
        R"({"buffer":0,"byteOffset":64,"byteLength":6},)"
        R"({"buffer":0,"byteOffset":70,"byteLength":6}],)"
        R"("buffers":[{"byteLength":76}]})";

    const std::size_t jsonLen = std::strlen(json);
    const std::size_t jsonPadded = (jsonLen + 3) & ~std::size_t{3};

    const std::size_t posBytes = sizeof(positions);
    const std::size_t uvBytes = sizeof(texcoords);
    const std::size_t idxBytes = sizeof(indices0) + sizeof(indices1);
    const std::size_t binLen = posBytes + uvBytes + idxBytes;
    const std::size_t binPadded = (binLen + 3) & ~std::size_t{3};

    const std::size_t totalLen = 12 + 8 + jsonPadded + 8 + binPadded;

    std::vector<std::byte> buf(totalLen);
    std::size_t off = 0;

    const std::uint32_t magic = 0x46546C67;
    const std::uint32_t version = 2;
    const auto len = static_cast<std::uint32_t>(totalLen);
    std::memcpy(buf.data() + off, &magic, 4); off += 4;
    std::memcpy(buf.data() + off, &version, 4); off += 4;
    std::memcpy(buf.data() + off, &len, 4); off += 4;

    auto jLen = static_cast<std::uint32_t>(jsonPadded);
    const std::uint32_t jsonType = 0x4E4F534A;
    std::memcpy(buf.data() + off, &jLen, 4); off += 4;
    std::memcpy(buf.data() + off, &jsonType, 4); off += 4;
    std::memcpy(buf.data() + off, json, jsonLen); off += jsonLen;
    for (std::size_t i = jsonLen; i < jsonPadded; ++i) {
        buf[off++] = std::byte{' '};
    }

    auto bLen = static_cast<std::uint32_t>(binPadded);
    const std::uint32_t binType = 0x004E4942;
    std::memcpy(buf.data() + off, &bLen, 4); off += 4;
    std::memcpy(buf.data() + off, &binType, 4); off += 4;
    std::memcpy(buf.data() + off, positions, posBytes); off += posBytes;
    std::memcpy(buf.data() + off, texcoords, uvBytes); off += uvBytes;
    std::memcpy(buf.data() + off, indices0, sizeof(indices0)); off += sizeof(indices0);
    std::memcpy(buf.data() + off, indices1, sizeof(indices1)); off += sizeof(indices1);
    for (std::size_t i = binLen; i < binPadded; ++i) {
        buf[off++] = std::byte{0};
    }

    return buf;
}

auto makeSparseTriangleGlb() -> std::vector<std::byte> {
    const char* json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
        R"("nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{)"
        R"("POSITION":0}}]}],"accessors":[{"bufferView":0,"componentType":5126,)"
        R"("count":3,"type":"VEC3","sparse":{"count":2,"indices":{)"
        R"("bufferView":1,"componentType":5121},"values":{"bufferView":2}}}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48,)"
        R"("byteStride":16},{"buffer":0,"byteOffset":48,"byteLength":2},)"
        R"({"buffer":0,"byteOffset":52,"byteLength":24}],)"
        R"("buffers":[{"byteLength":76}]})";

    std::vector<std::byte> binary(76);
    const std::uint8_t sparseIndices[2] = {1, 2};
    const float sparseValues[6] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
    };
    std::memcpy(binary.data() + 48, sparseIndices, sizeof(sparseIndices));
    std::memcpy(binary.data() + 52, sparseValues, sizeof(sparseValues));
    return assembleGlb(json, binary.data(), binary.size());
}
} // anonymous namespace

TEST_CASE("glTF: single triangle GLB loads") {
    auto glb = makeTriangleGlb();
    REQUIRE(glb.size() > 0);

    auto result = loadGltf(glb);
    REQUIRE(result.hasValue());
    const auto& scene = result.value();
    REQUIRE(scene.meshes.size() == 1);
    const auto& mesh = scene.meshes[0];

    CHECK(mesh.streams.size() == 1);
    CHECK(mesh.attributes.size() == 1);
    CHECK(mesh.attributes[0].semantic == resources::VertexSemantic::Position);
    CHECK(mesh.attributes[0].format == resources::VertexFormat::F32_3);
    CHECK(mesh.vertexCount() == 3);
    CHECK(mesh.indexCount() == 3);
    CHECK(mesh.indexFormat == resources::IndexFormat::U16);
    REQUIRE(mesh.submeshes.size() == 1);
    CHECK(mesh.submeshes[0].firstIndex == 0);
    CHECK(mesh.submeshes[0].indexCount == 3);
    REQUIRE(scene.meshMaterialIndices.size() == 1);
    CHECK(scene.meshMaterialIndices[0] == resources::kInvalidGltfIndex);
}

TEST_CASE("glTF: triangle bounds computed correctly") {
    auto glb = makeTriangleGlb();
    auto result = loadGltf(glb);
    REQUIRE(result.hasValue());
    const auto& mesh = result.value().meshes[0];

    CHECK(mesh.bounds.min.x == doctest::Approx(0.0f));
    CHECK(mesh.bounds.min.y == doctest::Approx(0.0f));
    CHECK(mesh.bounds.min.z == doctest::Approx(0.0f));
    CHECK(mesh.bounds.max.x == doctest::Approx(1.0f));
    CHECK(mesh.bounds.max.y == doctest::Approx(1.0f));
    CHECK(mesh.bounds.max.z == doctest::Approx(0.0f));
}

TEST_CASE("glTF: vertex positions match source data") {
    auto glb = makeTriangleGlb();
    auto result = loadGltf(glb);
    REQUIRE(result.hasValue());
    const auto& mesh = result.value().meshes[0];

    const auto& stream = mesh.streams[mesh.attributes[0].stream];
    REQUIRE(stream.data.size() == 36); // 3 × vec3 × 4 bytes

    float v0[3], v1[3], v2[3];
    std::memcpy(v0, stream.data.data(), 12);
    std::memcpy(v1, stream.data.data() + 12, 12);
    std::memcpy(v2, stream.data.data() + 24, 12);

    CHECK(v0[0] == doctest::Approx(0.0f));
    CHECK(v0[1] == doctest::Approx(0.0f));
    CHECK(v1[0] == doctest::Approx(1.0f));
    CHECK(v2[1] == doctest::Approx(1.0f));
}

TEST_CASE("glTF: index data matches source data") {
    auto glb = makeTriangleGlb();
    auto result = loadGltf(glb);
    REQUIRE(result.hasValue());
    const auto& mesh = result.value().meshes[0];

    REQUIRE(mesh.indices.size() == 6); // 3 × uint16
    std::uint16_t idx[3];
    std::memcpy(idx, mesh.indices.data(), 6);
    CHECK(idx[0] == 0);
    CHECK(idx[1] == 1);
    CHECK(idx[2] == 2);
}

TEST_CASE("glTF: non-indexed triangle receives sequential indices") {
    auto glb = makeTriangleGlb(false);
    auto result = loadGltf(glb);
    REQUIRE(result.hasValue());
    const auto& mesh = result.value().meshes[0];

    REQUIRE(mesh.indices.size() == 6);
    std::uint16_t indices[3]{};
    std::memcpy(indices, mesh.indices.data(), sizeof(indices));
    CHECK(indices[0] == 0);
    CHECK(indices[1] == 1);
    CHECK(indices[2] == 2);
}

TEST_CASE("glTF: sparse values use packed stride") {
    auto glb = makeSparseTriangleGlb();
    auto result = loadGltf(glb);
    REQUIRE(result.hasValue());
    const auto& mesh = result.value().meshes[0];

    CHECK(mesh.bounds.min == math::Vec3(0.0f, 0.0f, 0.0f));
    CHECK(mesh.bounds.max == math::Vec3(1.0f, 1.0f, 0.0f));
}

TEST_CASE("glTF: declared buffer views cannot exceed loaded data") {
    const char* gltf =
        R"({"asset":{"version":"2.0"},"buffers":[{)"
        R"("uri":"data:application/octet-stream;base64,AA==","byteLength":1}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":16},{"buffer":0,)"
        R"("byteLength":48}],"accessors":[{"componentType":5126,"count":4,)"
        R"("type":"VEC3","sparse":{"count":4,"indices":{"bufferView":0,)"
        R"("componentType":5125},"values":{"bufferView":1}}}]})";
    auto bytes = textBytes(gltf);
    auto result = loadGltf(bytes);
    CHECK(result.hasError());
}

TEST_CASE("glTF: two-primitive mesh loads as two Mesh records") {
    auto glb = makeTwoPrimitiveGlb();
    auto result = loadGltf(glb);
    REQUIRE(result.hasValue());
    const auto& scene = result.value();

    REQUIRE(scene.meshes.size() == 2);
    CHECK(scene.meshes[0].vertexCount() == 4);
    CHECK(scene.meshes[0].indexCount() == 3);
    CHECK(scene.meshes[1].vertexCount() == 4);
    CHECK(scene.meshes[1].indexCount() == 3);
    const auto* uv = scene.meshes[0].attributeFor(resources::VertexSemantic::TexCoord0);
    REQUIRE(uv != nullptr);
    CHECK(uv->format == resources::VertexFormat::F32_2);
    const auto& uvStream = scene.meshes[0].streams[uv->stream];
    CHECK(uvStream.strideBytes == 8);
    REQUIRE(uvStream.data.size() == 8 * sizeof(float));
    float unpackedUvs[8]{};
    std::memcpy(unpackedUvs, uvStream.data.data(), sizeof(unpackedUvs));
    CHECK(unpackedUvs[0] == doctest::Approx(0.0f));
    CHECK(unpackedUvs[2] == doctest::Approx(1.0f));
    CHECK(unpackedUvs[5] == doctest::Approx(1.0f));
    REQUIRE(scene.meshMaterialIndices.size() == 2);
    CHECK(scene.meshMaterialIndices[0] == 0);
    CHECK(scene.meshMaterialIndices[1] == 1);
}

TEST_CASE("glTF: core metallic-roughness materials load") {
    auto glb = makeTwoPrimitiveGlb();
    auto result = loadGltf(glb);
    REQUIRE(result.hasValue());
    const auto& materials = result.value().materials;

    REQUIRE(materials.size() == 2);
    const auto& paint = materials[0];
    CHECK(paint.name == "Paint");
    CHECK(paint.baseColorFactor.x == doctest::Approx(0.2f));
    CHECK(paint.baseColorFactor.y == doctest::Approx(0.4f));
    CHECK(paint.baseColorFactor.z == doctest::Approx(0.6f));
    CHECK(paint.baseColorFactor.w == doctest::Approx(0.8f));
    CHECK(paint.metallicFactor == doctest::Approx(0.25f));
    CHECK(paint.roughnessFactor == doctest::Approx(0.75f));
    CHECK(paint.emissiveFactor.x == doctest::Approx(0.1f));
    CHECK(paint.emissiveFactor.y == doctest::Approx(0.2f));
    CHECK(paint.emissiveFactor.z == doctest::Approx(0.3f));
    CHECK(paint.baseColorTexture.textureIndex == 0);
    CHECK(paint.baseColorTexture.texCoord == 1);
    CHECK(paint.metallicRoughnessTexture.textureIndex == 1);
    CHECK(paint.normalTexture.textureIndex == 2);
    CHECK(paint.normalTexture.texCoord == 1);
    CHECK(paint.normalTexture.scale == doctest::Approx(0.5f));
    CHECK(paint.occlusionTexture.textureIndex == 3);
    CHECK(paint.occlusionTexture.scale == doctest::Approx(0.25f));
    CHECK(paint.emissiveTexture.textureIndex == 4);
    CHECK(paint.alphaMode == resources::GltfAlphaMode::Mask);
    CHECK(paint.alphaCutoff == doctest::Approx(0.3f));
    CHECK(paint.doubleSided);

    const auto& glass = materials[1];
    CHECK(glass.name == "Glass");
    CHECK(glass.baseColorFactor == math::Vec4(1.0f));
    CHECK(glass.metallicFactor == doctest::Approx(1.0f));
    CHECK(glass.roughnessFactor == doctest::Approx(1.0f));
    CHECK(glass.alphaMode == resources::GltfAlphaMode::Blend);
    CHECK(glass.alphaCutoff == doctest::Approx(0.5f));
    CHECK(!glass.doubleSided);
}

TEST_CASE("glTF: empty input rejected") {
    std::vector<std::byte> empty;
    auto result = loadGltf(empty);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::Truncated);
}

TEST_CASE("glTF: garbage input rejected") {
    std::vector<std::byte> garbage = {
        std::byte{'g'}, std::byte{'l'}, std::byte{'T'}, std::byte{'F'}};
    auto result = loadGltf(garbage);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::Truncated);
}
