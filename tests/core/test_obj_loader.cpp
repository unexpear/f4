#include <doctest/doctest.h>

#include "core/resources/load_error.h"
#include "core/resources/obj_loader.h"
#include "core/resources/mesh.h"

#include <cstring>
#include <vector>

using namespace f4::resources;

static auto toBytes(const char* s) -> std::span<const std::byte> {
    return std::as_bytes(std::span<const char>{s, std::strlen(s)});
}

TEST_CASE("OBJ: single triangle with positions only") {
    const char* obj =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "f 1 2 3\n";

    auto result = loadObj(toBytes(obj));
    REQUIRE(result.hasValue());
    auto& scene = result.value();
    REQUIRE(scene.meshes.size() == 1);
    auto& mesh = scene.meshes[0];

    CHECK(mesh.vertexCount() == 3);
    CHECK(mesh.indexCount() == 3);
    REQUIRE(mesh.attributes.size() == 1);
    CHECK(mesh.attributes[0].semantic == VertexSemantic::Position);
    CHECK(mesh.attributes[0].format == VertexFormat::F32_3);

    // Check positions
    const auto* p = reinterpret_cast<const float*>(
        mesh.streams[0].data.data() + mesh.attributes[0].offsetBytes);
    CHECK(p[0] == doctest::Approx(0.0f));
    CHECK(p[1] == doctest::Approx(0.0f));
    CHECK(p[2] == doctest::Approx(0.0f));
    CHECK(p[3] == doctest::Approx(1.0f));
    CHECK(p[6] == doctest::Approx(0.0f));
    CHECK(p[7] == doctest::Approx(1.0f));
}

TEST_CASE("OBJ: decimal exponents are parsed") {
    const char* obj =
        "v -1.25e1 2.5E-1 3.0\n"
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "f 1 2 3\n";

    auto result = loadObj(toBytes(obj));
    REQUIRE(result.hasValue());
    const auto& mesh = result.value().meshes[0];
    const auto* positions = reinterpret_cast<const float*>(
        mesh.streams[0].data.data() + mesh.attributes[0].offsetBytes);
    CHECK(positions[0] == doctest::Approx(-12.5f));
    CHECK(positions[1] == doctest::Approx(0.25f));
    CHECK(positions[2] == doctest::Approx(3.0f));
}

TEST_CASE("OBJ: triangle with positions, normals, texcoords") {
    const char* obj =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "vt 0.0 0.0\n"
        "vt 1.0 0.0\n"
        "vt 0.0 1.0\n"
        "vn 0.0 0.0 1.0\n"
        "f 1/1/1 2/2/1 3/3/1\n";

    auto result = loadObj(toBytes(obj));
    REQUIRE(result.hasValue());
    auto& mesh = result.value().meshes[0];

    CHECK(mesh.vertexCount() == 3);
    CHECK(mesh.indexCount() == 3);
    REQUIRE(mesh.attributes.size() == 3);

    // Position, Normal, TexCoord0
    CHECK(mesh.attributes[0].semantic == VertexSemantic::Position);
    CHECK(mesh.attributes[1].semantic == VertexSemantic::Normal);
    CHECK(mesh.attributes[2].semantic == VertexSemantic::TexCoord0);

    // Check texcoord of first vertex
    const auto* uv = reinterpret_cast<const float*>(
        mesh.streams[0].data.data() + mesh.attributes[2].offsetBytes);
    CHECK(uv[0] == doctest::Approx(0.0f));
    CHECK(uv[1] == doctest::Approx(0.0f));
}

TEST_CASE("OBJ: face without texcoords (v//vn)") {
    const char* obj =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "vn 0.0 0.0 1.0\n"
        "f 1//1 2//1 3//1\n";

    auto result = loadObj(toBytes(obj));
    REQUIRE(result.hasValue());
    auto& mesh = result.value().meshes[0];
    CHECK(mesh.vertexCount() == 3);
    CHECK(mesh.indexCount() == 3);

    // Should have position and normal but no texcoord
    bool hasPos = false, hasNorm = false, hasUV = false;
    for (const auto& a : mesh.attributes) {
        if (a.semantic == VertexSemantic::Position) hasPos = true;
        if (a.semantic == VertexSemantic::Normal) hasNorm = true;
        if (a.semantic == VertexSemantic::TexCoord0) hasUV = true;
    }
    CHECK(hasPos);
    CHECK(hasNorm);
    CHECK(!hasUV);
}

TEST_CASE("OBJ: face with texcoords only (v/vt)") {
    const char* obj =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "vt 0.0 0.0\n"
        "vt 1.0 0.0\n"
        "vt 0.0 1.0\n"
        "f 1/1 2/2 3/3\n";

    auto result = loadObj(toBytes(obj));
    REQUIRE(result.hasValue());
    auto& mesh = result.value().meshes[0];
    CHECK(mesh.vertexCount() == 3);
    CHECK(mesh.indexCount() == 3);

    bool hasUV = false;
    for (const auto& a : mesh.attributes) {
        if (a.semantic == VertexSemantic::TexCoord0) hasUV = true;
    }
    CHECK(hasUV);
}

TEST_CASE("OBJ: quad fan-triangulated into two triangles") {
    const char* obj =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 1.0 1.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "f 1 2 3 4\n";

    auto result = loadObj(toBytes(obj));
    REQUIRE(result.hasValue());
    auto& mesh = result.value().meshes[0];
    CHECK(mesh.vertexCount() == 4);
    CHECK(mesh.indexCount() == 6); // two triangles
}

TEST_CASE("OBJ: two groups become two meshes") {
    const char* obj =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "v 2.0 0.0 0.0\n"
        "g group1\n"
        "f 1 2 3\n"
        "g group2\n"
        "f 2 4 3\n";

    auto result = loadObj(toBytes(obj));
    REQUIRE(result.hasValue());
    auto& scene = result.value();
    REQUIRE(scene.meshes.size() == 2);
    CHECK(scene.meshes[0].indexCount() == 3);
    CHECK(scene.meshes[1].indexCount() == 3);
}

TEST_CASE("OBJ: comments and empty lines ignored") {
    const char* obj =
        "# This is a comment\n"
        "\n"
        "v 0.0 0.0 0.0\n"
        "# another comment\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "\n"
        "f 1 2 3\n";

    auto result = loadObj(toBytes(obj));
    REQUIRE(result.hasValue());
    CHECK(result.value().meshes.size() == 1);
    CHECK(result.value().meshes[0].vertexCount() == 3);
}

TEST_CASE("OBJ: negative indices") {
    const char* obj =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "f -3 -2 -1\n";

    auto result = loadObj(toBytes(obj));
    REQUIRE(result.hasValue());
    auto& mesh = result.value().meshes[0];
    CHECK(mesh.vertexCount() == 3);
    CHECK(mesh.indexCount() == 3);
}

TEST_CASE("OBJ: bounds computed correctly") {
    const char* obj =
        "v -1.0 -2.0 0.0\n"
        "v 3.0 4.0 0.0\n"
        "v 0.0 0.0 0.0\n"
        "f 1 2 3\n";

    auto result = loadObj(toBytes(obj));
    REQUIRE(result.hasValue());
    auto& mesh = result.value().meshes[0];

    CHECK(mesh.bounds.min.x == doctest::Approx(-1.0f));
    CHECK(mesh.bounds.min.y == doctest::Approx(-2.0f));
    CHECK(mesh.bounds.min.z == doctest::Approx(0.0f));
    CHECK(mesh.bounds.max.x == doctest::Approx(3.0f));
    CHECK(mesh.bounds.max.y == doctest::Approx(4.0f));
    CHECK(mesh.bounds.max.z == doctest::Approx(0.0f));
}

TEST_CASE("OBJ: usemtl and mtllib parsed without error") {
    const char* obj =
        "mtllib material.mtl\n"
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "usemtl Default\n"
        "f 1 2 3\n";

    auto result = loadObj(toBytes(obj));
    REQUIRE(result.hasValue());
    CHECK(result.value().meshes.size() == 1);
}

TEST_CASE("OBJ: empty input rejected") {
    std::vector<std::byte> empty;
    auto result = loadObj(empty);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::Truncated);
}

TEST_CASE("OBJ: no faces rejected") {
    const char* obj =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n";

    auto result = loadObj(toBytes(obj));
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::Truncated);
}

TEST_CASE("OBJ: vertex deduplication — shared vertices across faces") {
    const char* obj =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "v 1.0 1.0 0.0\n"
        "f 1 2 3\n"
        "f 2 4 3\n";

    auto result = loadObj(toBytes(obj));
    REQUIRE(result.hasValue());
    auto& mesh = result.value().meshes[0];
    // Vertices 2 and 3 are shared between the two triangles
    CHECK(mesh.vertexCount() == 4);
    CHECK(mesh.indexCount() == 6);
}

TEST_CASE("OBJ: multiple faces with different vertex combos") {
    const char* obj =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 0.0 1.0 0.0\n"
        "v 1.0 1.0 0.0\n"
        "vt 0.0 0.0\n"
        "vt 1.0 0.0\n"
        "vt 0.0 1.0\n"
        "vt 1.0 1.0\n"
        "f 1/1 2/2 3/3\n"
        "f 2/2 4/4 3/3\n";

    auto result = loadObj(toBytes(obj));
    REQUIRE(result.hasValue());
    auto& mesh = result.value().meshes[0];
    CHECK(mesh.vertexCount() == 4);
    CHECK(mesh.indexCount() == 6);
}

TEST_CASE("OBJ: Windows line endings (CRLF)") {
    const char* obj =
        "v 0.0 0.0 0.0\r\n"
        "v 1.0 0.0 0.0\r\n"
        "v 0.0 1.0 0.0\r\n"
        "f 1 2 3\r\n";

    auto result = loadObj(toBytes(obj));
    REQUIRE(result.hasValue());
    CHECK(result.value().meshes[0].vertexCount() == 3);
}

TEST_CASE("OBJ: polygon with 5+ vertices triangulated") {
    const char* obj =
        "v 0.0 0.0 0.0\n"
        "v 1.0 0.0 0.0\n"
        "v 1.0 1.0 0.0\n"
        "v 0.5 1.5 0.0\n"
        "v 0.0 1.0 0.0\n"
        "f 1 2 3 4 5\n";

    auto result = loadObj(toBytes(obj));
    REQUIRE(result.hasValue());
    auto& mesh = result.value().meshes[0];
    CHECK(mesh.vertexCount() == 5);
    CHECK(mesh.indexCount() == 9); // 3 triangles from a pentagon
}
