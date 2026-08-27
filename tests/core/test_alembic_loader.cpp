#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)
#pragma warning(disable : 4457)
#pragma warning(disable : 4458)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include <Alembic/Abc/All.h>
#include <Alembic/AbcGeom/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "core/resources/alembic_loader.h"
#include "core/resources/load_error.h"
#include "core/resources/mesh.h"

using namespace f4::resources;
using namespace Alembic::Abc;
using namespace Alembic::AbcGeom;

namespace {

// A unit cube as an OPolyMesh: 8 vertices, 6 quad faces. Writes a frozen .abc
// to `path`, which we then read back as bytes to exercise the memory loader.
void makeCubeArchive(const char* path) {
    OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), path);
    OObject top(archive, kTop);
    OPolyMesh meshObj(top, "Cube");
    OPolyMeshSchema& mesh = meshObj.getSchema();

    std::vector<V3f> verts = {
        {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1},
        {-1, 1, -1},  {1, 1, -1},  {1, 1, 1},  {-1, 1, 1},
    };
    std::vector<int32_t> indices = {
        0, 3, 2, 1,   // bottom
        4, 5, 6, 7,   // top
        1, 2, 6, 5,   // +X
        0, 4, 7, 3,   // -X
        2, 3, 7, 6,   // +Z
        0, 1, 5, 4,   // -Z
    };
    std::vector<int32_t> counts(6, 4);

    OPolyMeshSchema::Sample sample(
        V3fArraySample(verts),
        Int32ArraySample(indices.data(), static_cast<std::ptrdiff_t>(indices.size())),
        Int32ArraySample(counts.data(), static_cast<std::ptrdiff_t>(counts.size())));
    mesh.set(sample);
}

std::vector<std::byte> loadBytes(const char* path) {
    FILE* f = nullptr;
#ifdef _MSC_VER
    fopen_s(&f, path, "rb");
#else
    f = fopen(path, "rb");
#endif
    REQUIRE(f != nullptr);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<std::byte> data(sz);
    fread(data.data(), 1, sz, f);
    fclose(f);
    return data;
}

} // namespace

TEST_CASE("Alembic: empty input rejected") {
    std::vector<std::byte> empty;
    auto result = loadAlembic(empty);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::Truncated);
}

TEST_CASE("Alembic: garbage input rejected") {
    std::vector<std::byte> garbage(4);
    garbage[0] = std::byte{'G'};
    garbage[1] = std::byte{'A'};
    garbage[2] = std::byte{'R'};
    garbage[3] = std::byte{'B'};
    auto result = loadAlembic(garbage);
    CHECK(result.hasError());
}

TEST_CASE("Alembic: cube loads a mesh from memory") {
    makeCubeArchive("f4_alembic_test_cube.abc");
    auto data = loadBytes("f4_alembic_test_cube.abc");
    REQUIRE(data.size() > 0);

    auto result = loadAlembic(data);
    REQUIRE(result.hasValue());

    auto& scene = result.value();
    REQUIRE(scene.meshes.size() >= 1);

    bool foundMesh = false;
    for (const auto& mesh : scene.meshes) {
        if (mesh.vertexCount() == 0) {
            continue;
        }
        foundMesh = true;
        // 6 quads -> 12 triangles -> 36 indices.
        CHECK(mesh.vertexCount() >= 8);
        CHECK(mesh.indexCount() >= 12);
        CHECK(mesh.bounds.max.x > mesh.bounds.min.x);
        CHECK(mesh.bounds.max.y > mesh.bounds.min.y);
        CHECK(mesh.bounds.max.z > mesh.bounds.min.z);

        CHECK(mesh.bounds.min.x >= -1.01f);
        CHECK(mesh.bounds.max.x <= 1.01f);
        CHECK(mesh.bounds.min.y >= -1.01f);
        CHECK(mesh.bounds.max.y <= 1.01f);
        CHECK(mesh.bounds.min.z >= -1.01f);
        CHECK(mesh.bounds.max.z <= 1.01f);
    }
    CHECK(foundMesh);
}

TEST_CASE("Alembic: cube has a position attribute stream") {
    makeCubeArchive("f4_alembic_test_cube.abc");
    auto data = loadBytes("f4_alembic_test_cube.abc");

    auto result = loadAlembic(data);
    REQUIRE(result.hasValue());

    bool foundPos = false;
    for (const auto& mesh : result.value().meshes) {
        for (const auto& a : mesh.attributes) {
            if (a.semantic == VertexSemantic::Position) {
                foundPos = true;
                CHECK(a.format == VertexFormat::F32_3);
                CHECK(mesh.streams.size() >= 1);
                CHECK(mesh.streams[0].strideBytes >= 12);
            }
        }
    }
    CHECK(foundPos);
}
