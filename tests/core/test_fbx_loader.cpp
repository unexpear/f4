#include <doctest/doctest.h>

#include <cstdio>
#include <vector>

#include "core/resources/fbx_loader.h"
#include "core/resources/load_error.h"
#include "core/resources/mesh.h"

using namespace f4::resources;

static const char* kCubeFbx =
    F4_UFBX_TEST_DATA_DIR "/blender_272_cube_7400_binary.fbx";

static std::vector<std::byte> loadTestFbx() {
    FILE* f = nullptr;
#ifdef _MSC_VER
    fopen_s(&f, kCubeFbx, "rb");
#else
    f = fopen(kCubeFbx, "rb");
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

TEST_CASE("FBX: cube loads from real file") {
    auto data = loadTestFbx();
    auto result = loadFbx(data);
    REQUIRE(result.hasValue());

    auto& scene = result.value();
    REQUIRE(scene.meshes.size() >= 1);

    bool hasPos = false;
    for (const auto& mesh : scene.meshes) {
        for (const auto& a : mesh.attributes) {
            if (a.semantic == VertexSemantic::Position) hasPos = true;
        }
    }
    CHECK(hasPos);
}

TEST_CASE("FBX: cube mesh has correct topology") {
    auto data = loadTestFbx();
    auto result = loadFbx(data);
    REQUIRE(result.hasValue());

    auto& scene = result.value();
    bool foundMesh = false;
    for (const auto& mesh : scene.meshes) {
        if (mesh.vertexCount() == 0) continue;
        foundMesh = true;
        CHECK(mesh.vertexCount() >= 8);
        CHECK(mesh.indexCount() >= 12);
        CHECK(mesh.bounds.max.x > mesh.bounds.min.x);
        CHECK(mesh.bounds.max.y > mesh.bounds.min.y);
        CHECK(mesh.bounds.max.z > mesh.bounds.min.z);
    }
    CHECK(foundMesh);
}

TEST_CASE("FBX: cube has normals") {
    auto data = loadTestFbx();
    auto result = loadFbx(data);
    REQUIRE(result.hasValue());

    for (const auto& mesh : result.value().meshes) {
        if (mesh.vertexCount() == 0) continue;
        bool hasNrm = false;
        for (const auto& a : mesh.attributes) {
            if (a.semantic == VertexSemantic::Normal) hasNrm = true;
        }
        CHECK(hasNrm);
    }
}

TEST_CASE("FBX: empty input rejected") {
    std::vector<std::byte> empty;
    auto result = loadFbx(empty);
    CHECK(result.hasError());
    CHECK(result.error() == LoadError::Truncated);
}

TEST_CASE("FBX: garbage input rejected") {
    std::vector<std::byte> garbage(4);
    garbage[0] = std::byte{'G'};
    garbage[1] = std::byte{'A'};
    garbage[2] = std::byte{'R'};
    garbage[3] = std::byte{'B'};
    auto result = loadFbx(garbage);
    CHECK(result.hasError());
}
