#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "core/resources/gltf_loader.h"

using namespace f4;
using resources::GltfScene;
using resources::LoadError;
using resources::loadGltf;

namespace {
// Build a minimal valid GLB with one mesh: a single triangle.
// Layout: header (12) + JSON chunk (4+4+N) + BIN chunk (4+4+M).
// BIN data: 3×vec3 positions (36 bytes) + 3×uint16 indices (6 bytes) = 42 bytes.
auto makeTriangleGlb() -> std::vector<std::byte> {
    // Binary payload: 3 positions + 3 indices
    const float positions[9] = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
    };
    const std::uint16_t indices[3] = {0, 1, 2};

    // JSON referencing the BIN chunk (buffer 0).
    const char* json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
        R"("nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{)"
        R"("POSITION":0},"indices":1}]}],"accessors":[{"bufferView":0,)"
        R"("componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],)"
        R"("max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,)"
        R"("type":"SCALAR"}],"bufferViews":[{"buffer":0,"byteOffset":0,)"
        R"("byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],)"
        R"("buffers":[{"byteLength":42}]})";

    const std::size_t jsonLen = std::strlen(json);
    const std::size_t jsonPadded = (jsonLen + 3) & ~std::size_t{3};

    // BIN: positions + indices, padded to 4-byte alignment.
    const std::size_t posBytes = sizeof(positions);
    const std::size_t idxBytes = sizeof(indices);
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
    std::memcpy(buf.data() + off, indices, idxBytes); off += idxBytes;
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
    const std::uint16_t indices0[3] = {0, 1, 2};
    const std::uint16_t indices1[3] = {1, 3, 2};

    const char* json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
        R"("nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{)"
        R"("POSITION":0},"indices":1},{"attributes":{"POSITION":0},)"
        R"("indices":2}]}],"accessors":[{"bufferView":0,"componentType":5126,)"
        R"("count":4,"type":"VEC3","min":[0,0,0],"max":[2,1,0]},)"
        R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"},)"
        R"({"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},)"
        R"({"buffer":0,"byteOffset":48,"byteLength":6},)"
        R"({"buffer":0,"byteOffset":54,"byteLength":6}],)"
        R"("buffers":[{"byteLength":60}]})";

    const std::size_t jsonLen = std::strlen(json);
    const std::size_t jsonPadded = (jsonLen + 3) & ~std::size_t{3};

    const std::size_t posBytes = sizeof(positions);
    const std::size_t idxBytes = sizeof(indices0) + sizeof(indices1);
    const std::size_t binLen = posBytes + idxBytes;
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
    std::memcpy(buf.data() + off, indices0, sizeof(indices0)); off += sizeof(indices0);
    std::memcpy(buf.data() + off, indices1, sizeof(indices1)); off += sizeof(indices1);
    for (std::size_t i = binLen; i < binPadded; ++i) {
        buf[off++] = std::byte{0};
    }

    return buf;
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
