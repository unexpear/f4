#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

#include <doctest/doctest.h>

#include "backends/null/null_device.h"
#include "engine/rendering/gpu_mesh.h"
#include "engine/world/terrain_meshing.h"

namespace {

using namespace f4;
using namespace f4::world;

auto emptyVoxels() -> VoxelCells {
    VoxelCells cells;
    cells.values.resize(kVoxelCellCount);
    return cells;
}

auto flatHeightTile(float height) -> HeightTile {
    HeightTile tile;
    tile.samples.resize(kHeightSampleCount);
    for (HeightSample& sample : tile.samples) {
        sample.height = height;
        sample.biomeId = 1;
        sample.materialWeights = {{128, 64, 32, 31}};
    }
    return tile;
}

auto flatHexTile(std::uint32_t radius, float height) -> HexTile {
    HexTile tile;
    tile.radius = radius;
    tile.width = radius * 2u + 1u;
    tile.cells.resize(static_cast<std::size_t>(tile.width) * tile.width);
    const std::int32_t signedRadius = static_cast<std::int32_t>(radius);
    for (std::int32_t r = -signedRadius; r <= signedRadius; ++r) {
        const std::int32_t qMin = std::max(-signedRadius, -r - signedRadius);
        const std::int32_t qMax = std::min(signedRadius, -r + signedRadius);
        for (std::int32_t q = qMin; q <= qMax; ++q) {
            const AxialCoord cell{q, r};
            const auto index = tile.index(cell);
            REQUIRE(index.has_value());
            tile.cells[*index] = {static_cast<std::uint8_t>((q - r + 12) % 3),
                                  kInvalidOwner};
            for (std::uint32_t corner = 0; corner < 6; ++corner) {
                tile.cornerHeights.set(hexCorner(cell, corner), height);
            }
        }
    }
    return tile;
}

void hashByte(std::uint64_t& hash, std::uint8_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
}

void hashWord(std::uint64_t& hash, std::uint64_t value, std::uint32_t bytes) {
    for (std::uint32_t i = 0; i < bytes; ++i) {
        hashByte(hash, static_cast<std::uint8_t>(value >> (i * 8u)));
    }
}

auto quantized(float value) -> std::int32_t {
    return static_cast<std::int32_t>(std::lround(value * 100000.0f));
}

auto meshDigest(const MeshData& mesh) -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ull;
    hashWord(hash, mesh.vertices.size(), 8);
    hashWord(hash, mesh.indices.size(), 8);
    for (const TerrainVertex& vertex : mesh.vertices) {
        hashWord(hash, static_cast<std::uint32_t>(quantized(vertex.position.x)), 4);
        hashWord(hash, static_cast<std::uint32_t>(quantized(vertex.position.y)), 4);
        hashWord(hash, static_cast<std::uint32_t>(quantized(vertex.position.z)), 4);
        hashWord(hash, static_cast<std::uint32_t>(quantized(vertex.normal.x)), 4);
        hashWord(hash, static_cast<std::uint32_t>(quantized(vertex.normal.y)), 4);
        hashWord(hash, static_cast<std::uint32_t>(quantized(vertex.normal.z)), 4);
        hashWord(hash, static_cast<std::uint32_t>(quantized(vertex.uv.x)), 4);
        hashWord(hash, static_cast<std::uint32_t>(quantized(vertex.uv.y)), 4);
        for (std::uint8_t channel : vertex.color) hashByte(hash, channel);
        for (std::uint8_t weight : vertex.materialWeights) hashByte(hash, weight);
        hashWord(hash, vertex.material, 2);
        hashWord(hash, vertex.biome, 2);
        hashByte(hash, vertex.ambientOcclusion);
    }
    for (std::uint32_t index : mesh.indices) hashWord(hash, index, 4);
    for (const TerrainSurface& surface : mesh.surfaces) {
        hashWord(hash, surface.firstIndex, 4);
        hashWord(hash, surface.indexCount, 4);
        hashWord(hash, surface.material, 2);
    }
    return hash;
}

void checkTriangleWinding(const MeshData& mesh) {
    REQUIRE(mesh.indices.size() % 3u == 0);
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        const TerrainVertex& a = mesh.vertices[mesh.indices[i]];
        const TerrainVertex& b = mesh.vertices[mesh.indices[i + 1u]];
        const TerrainVertex& c = mesh.vertices[mesh.indices[i + 2u]];
        const math::Vec3 geometric = math::cross(b.position - a.position,
                                                 c.position - a.position);
        CHECK(math::dot(geometric, a.normal) > 0.0f);
    }
}

auto readyChunk(ChunkStore& store, ChunkCoord coord, PolicyKind policy,
                const PolicyParams& params = {}) -> ChunkHandle {
    const ChunkHandle handle = store.create(coord, 0x5345'414dull, policy, params);
    REQUIRE(store.queue(handle));
    REQUIRE(store.generate(handle));
    return handle;
}

} // namespace

TEST_CASE("M15: voxel greedy meshing culls interiors and preserves winding") {
    BlockRegistry blocks;
    VoxelCells single = emptyVoxels();
    single.values[voxelIndex(4, 5, 6)] = 2;
    const auto singleResult = meshVoxelChunk(single, blocks);
    REQUIRE(singleResult.hasValue());
    const MeshData& singleMesh = singleResult.value();
    CHECK(singleMesh.vertices.size() == 24);
    CHECK(singleMesh.indices.size() == 36);
    REQUIRE(singleMesh.surfaces.size() == 1);
    CHECK(singleMesh.surfaces[0].material == 1);
    for (const TerrainVertex& vertex : singleMesh.vertices) {
        CHECK(vertex.ambientOcclusion == 3);
        CHECK(vertex.color == std::array<std::uint8_t, 4>{{255, 255, 255, 255}});
    }
    checkTriangleWinding(singleMesh);

    VoxelCells volume = emptyVoxels();
    for (std::uint16_t& cell : volume.values) cell = 2;
    const auto volumeResult = meshVoxelChunk(volume, blocks);
    REQUIRE(volumeResult.hasValue());
    CHECK(volumeResult.value().vertices.size() == 24);
    CHECK(volumeResult.value().indices.size() == 36);
    checkTriangleWinding(volumeResult.value());

    std::uint32_t borderCalls = 0;
    VoxelMeshingOptions surrounded;
    surrounded.borderFetch = [&](std::int32_t x, std::int32_t y,
                                 std::int32_t z) -> std::uint16_t {
        const bool onBorder = x == -1 || y == -1 || z == -1 ||
                              x == 32 || y == 32 || z == 32;
        CHECK(onBorder);
        ++borderCalls;
        return 2;
    };
    const auto surroundedResult = meshVoxelChunk(volume, blocks, surrounded);
    REQUIRE(surroundedResult.hasValue());
    CHECK(surroundedResult.value().vertices.isEmpty());
    CHECK(surroundedResult.value().indices.isEmpty());
    CHECK(borderCalls == 34u * 34u * 34u - 32u * 32u * 32u);
}

TEST_CASE("M15: voxel materials constrain greedy merges and AO is baked per corner") {
    BlockRegistry blocks;
    REQUIRE(blocks.setEngineBlock(3, {true, true, 7, 2.0f}));
    VoxelCells cells = emptyVoxels();
    cells.values[voxelIndex(4, 4, 4)] = 2;
    cells.values[voxelIndex(5, 4, 4)] = 3;
    const auto layered = meshVoxelChunk(cells, blocks);
    REQUIRE(layered.hasValue());
    REQUIRE(layered.value().surfaces.size() == 2);
    CHECK(layered.value().surfaces[0].material == 1);
    CHECK(layered.value().surfaces[1].material == 7);
    CHECK(layered.value().vertices.size() == 40);
    CHECK(layered.value().indices.size() == 60);

    cells = emptyVoxels();
    cells.values[voxelIndex(1, 1, 1)] = 3;
    cells.values[voxelIndex(2, 0, 1)] = 2;
    cells.values[voxelIndex(2, 1, 0)] = 2;
    const auto occluded = meshVoxelChunk(cells, blocks);
    REQUIRE(occluded.hasValue());
    bool foundDarkCorner = false;
    for (const TerrainVertex& vertex : occluded.value().vertices) {
        if (vertex.material == 7 && vertex.normal == math::Vec3{1.0f, 0.0f, 0.0f} &&
            vertex.ambientOcclusion == 0) {
            foundDarkCorner = true;
            CHECK(vertex.color[0] == 63);
        }
    }
    CHECK(foundDarkCorner);

    VoxelMeshingOptions invalidBorder;
    invalidBorder.borderFetch = [](std::int32_t, std::int32_t, std::int32_t) {
        return static_cast<std::uint16_t>(999);
    };
    const auto invalid = meshVoxelChunk(cells, blocks, invalidBorder);
    REQUIRE(invalid.hasError());
    CHECK(invalid.error() == TerrainMeshError::UnknownBlock);

    REQUIRE(blocks.setEngineBlock(4, {true, false, 4, 0.25f}));
    REQUIRE(blocks.setEngineBlock(5, {true, false, 5, 0.25f}));
    VoxelCells west = emptyVoxels();
    west.values[voxelIndex(31, 5, 5)] = 4;
    VoxelMeshingOptions westOptions;
    westOptions.borderFetch = [](std::int32_t x, std::int32_t y, std::int32_t z) {
        return static_cast<std::uint16_t>(x == 32 && y == 5 && z == 5 ? 5 : 0);
    };
    VoxelCells east = emptyVoxels();
    east.values[voxelIndex(0, 5, 5)] = 5;
    VoxelMeshingOptions eastOptions;
    eastOptions.borderFetch = [](std::int32_t x, std::int32_t y, std::int32_t z) {
        return static_cast<std::uint16_t>(x == -1 && y == 5 && z == 5 ? 4 : 0);
    };
    const auto westMesh = meshVoxelChunk(west, blocks, westOptions);
    const auto eastMesh = meshVoxelChunk(east, blocks, eastOptions);
    REQUIRE(westMesh.hasValue());
    REQUIRE(eastMesh.hasValue());
    std::uint32_t westInterfaceVertices = 0;
    std::uint32_t eastInterfaceVertices = 0;
    for (const TerrainVertex& vertex : westMesh.value().vertices) {
        if (vertex.material == 4 && vertex.normal == math::Vec3{1.0f, 0.0f, 0.0f} &&
            vertex.position.x == 32.0f) {
            ++westInterfaceVertices;
        }
    }
    for (const TerrainVertex& vertex : eastMesh.value().vertices) {
        if (vertex.material == 5 && vertex.normal == math::Vec3{-1.0f, 0.0f, 0.0f} &&
            vertex.position.x == 0.0f) {
            ++eastInterfaceVertices;
        }
    }
    CHECK(westInterfaceVertices == 4);
    CHECK(eastInterfaceVertices == 0);
}

TEST_CASE("M15: height tiles use neighbor normals, fixed triangles, and four skirts") {
    HeightTile tile = flatHeightTile(0.0f);
    for (std::uint32_t z = 0; z < kHeightSamples; ++z) {
        for (std::uint32_t x = 0; x < kHeightSamples; ++x) {
            tile.samples[heightSampleIndex(x, z)].height = static_cast<float>(x);
        }
    }
    HeightMeshingOptions options;
    options.transitionFloor = -2.0f;
    options.borderFetch = [](std::int64_t x, std::int64_t) {
        return static_cast<float>(x);
    };
    const auto result = meshHeightChunk(tile, {}, options);
    REQUIRE(result.hasValue());
    const MeshData& mesh = result.value();
    CHECK(mesh.vertices.size() == 65u * 65u + 4u * 2u * 65u);
    CHECK(mesh.indices.size() == 64u * 64u * 6u + 4u * 64u * 6u);
    REQUIRE(mesh.surfaces.size() == 1);
    const math::Vec3 expectedNormal = math::normalized({-2.0f, 2.0f, 0.0f});
    for (std::uint32_t z : {0u, 31u, 64u}) {
        for (std::uint32_t x : {0u, 32u, 64u}) {
            const TerrainVertex& vertex = mesh.vertices[heightSampleIndex(x, z)];
            CHECK(vertex.normal == expectedNormal);
            CHECK(vertex.materialWeights == std::array<std::uint8_t, 4>{{128, 64, 32, 31}});
        }
    }
    for (std::size_t i = kHeightSampleCount + 1u; i < mesh.vertices.size(); i += 2u) {
        CHECK(mesh.vertices[i].position.y == -2.0f);
    }
    checkTriangleWinding(mesh);
    options.skirtDepth = 0.0f;
    CHECK(meshHeightChunk(tile, {}, options).hasError());
}

TEST_CASE("M15: adjacent height meshes share positions and neighbor-fed normals") {
    constexpr std::uint64_t seed = 0x5154u;
    const ChunkCoord westCoord{-1, 0, 2};
    const ChunkCoord eastCoord{0, 0, 2};
    const HeightTile west = generateHeightTile(seed, westCoord, {});
    const HeightTile east = generateHeightTile(seed, eastCoord, {});
    HeightMeshingOptions options;
    options.borderFetch = [&](std::int64_t globalX, std::int64_t globalZ) {
        const std::uint32_t z = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(globalZ - 128, 0, kHeightQuads));
        if (globalX < 0) {
            const std::uint32_t x = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(globalX + 64, 0, kHeightQuads));
            return west.samples[heightSampleIndex(x, z)].height;
        }
        const std::uint32_t x = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(globalX, 0, kHeightQuads));
        return east.samples[heightSampleIndex(x, z)].height;
    };
    const auto westMesh = meshHeightChunk(west, westCoord, options);
    const auto eastMesh = meshHeightChunk(east, eastCoord, options);
    REQUIRE(westMesh.hasValue());
    REQUIRE(eastMesh.hasValue());
    for (std::uint32_t z = 0; z < kHeightSamples; ++z) {
        const TerrainVertex& westVertex =
            westMesh.value().vertices[heightSampleIndex(kHeightQuads, z)];
        const TerrainVertex& eastVertex =
            eastMesh.value().vertices[heightSampleIndex(0, z)];
        CHECK(westVertex.position.y == eastVertex.position.y);
        CHECK(westVertex.normal == eastVertex.normal);
        CHECK(westVertex.position.x + 0.0f == eastVertex.position.x + 64.0f);
    }
}

TEST_CASE("M15: hex meshing emits closed prisms and suppresses stitched border walls") {
    const HexTile one = flatHexTile(0, 2.0f);
    HexMeshingOptions options;
    options.floorHeight = 0.0f;
    const auto result = meshHexChunk(one, {}, options);
    REQUIRE(result.hasValue());
    CHECK(result.value().vertices.size() == 60);
    CHECK(result.value().indices.size() == 72);
    checkTriangleWinding(result.value());
    const resources::Mesh resource = toResourceMesh(result.value());
    CHECK(resource.bounds.min.x == doctest::Approx(-0.8660254f));
    CHECK(resource.bounds.max.x == doctest::Approx(0.8660254f));
    CHECK(resource.bounds.min.y == 0.0f);
    CHECK(resource.bounds.max.y == 2.0f);
    CHECK(resource.bounds.min.z == -1.0f);
    CHECK(resource.bounds.max.z == 1.0f);

    std::uint32_t neighborQueries = 0;
    options.hasNeighbor = [&](AxialCoord) {
        ++neighborQueries;
        return true;
    };
    const auto stitched = meshHexChunk(one, {2, 0, -3}, options);
    REQUIRE(stitched.hasValue());
    CHECK(neighborQueries == 6);
    CHECK(stitched.value().vertices.size() == 36);
    CHECK(stitched.value().indices.size() == 36);

    const HexTile radiusOne = flatHexTile(1, 2.0f);
    options.hasNeighbor = {};
    const auto island = meshHexChunk(radiusOne, {}, options);
    REQUIRE(island.hasValue());
    CHECK(island.value().vertices.size() == 324);
    CHECK(island.value().indices.size() == 360);
    CHECK(island.value().surfaces.size() == 3);
    checkTriangleWinding(island.value());
    options.floorHeight = 2.0f;
    CHECK(meshHexChunk(one, {}, options).hasError());
}

TEST_CASE("M15: all policy meshes convert to the same RHI record and upload") {
    BlockRegistry blocks;
    VoxelCells voxels = emptyVoxels();
    voxels.values[voxelIndex(1, 1, 1)] = 2;
    const auto voxelMesh = meshVoxelChunk(voxels, blocks);
    const auto heightMesh = meshHeightChunk(flatHeightTile(3.0f), {});
    HexMeshingOptions hexOptions;
    hexOptions.floorHeight = 0.0f;
    const auto hexMesh = meshHexChunk(flatHexTile(0, 2.0f), {}, hexOptions);
    REQUIRE(voxelMesh.hasValue());
    REQUIRE(heightMesh.hasValue());
    REQUIRE(hexMesh.hasValue());

    gfx::NullDevice device;
    for (const MeshData* source : {&voxelMesh.value(), &heightMesh.value(), &hexMesh.value()}) {
        const resources::Mesh resource = toResourceMesh(*source);
        CHECK(resource.vertexCount() == source->vertices.size());
        CHECK(resource.indexCount() == source->indices.size());
        CHECK(resource.attributes.size() == 4);
        CHECK(resource.attributeFor(resources::VertexSemantic::Position) != nullptr);
        CHECK(resource.attributeFor(resources::VertexSemantic::Normal) != nullptr);
        CHECK(resource.attributeFor(resources::VertexSemantic::TexCoord0) != nullptr);
        CHECK(resource.attributeFor(resources::VertexSemantic::Color0) != nullptr);
        auto gpu = engine::createGpuMesh(device, resource);
        REQUIRE(gpu.hasValue());
        CHECK(gpu.value().indexCount == source->indices.size());
        engine::destroyGpuMesh(device, gpu.value());
    }
    CHECK(device.liveBuffers() == 0);
}

TEST_CASE("M15: snapshot meshing applies overlays and rejects stale publication") {
    BlockRegistry blocks;
    ChunkStore store(1);
    const ChunkHandle handle = readyChunk(store, {}, PolicyKind::Voxel);
    REQUIRE(store.edit(handle, voxelIndex(2, 31, 2), CellValue::voxelValue(0)));
    const auto snapshot = store.snapshot(handle);
    REQUIRE(snapshot.has_value());
    TerrainMeshingContext context;
    context.blocks = &blocks;
    const auto first = meshChunkSnapshot(*snapshot, context);
    REQUIRE(first.hasValue());
    CHECK(first.value().sourceVersion == snapshot->version);
    REQUIRE(store.edit(handle, voxelIndex(3, 31, 2), CellValue::voxelValue(0)));
    CHECK_FALSE(commitTerrainMesh(store, handle, first.value()));

    const auto current = store.snapshot(handle);
    REQUIRE(current.has_value());
    const auto second = meshChunkSnapshot(*current, context);
    REQUIRE(second.hasValue());
    REQUIRE(commitTerrainMesh(store, handle, second.value()));
    CHECK(store.state(handle) == ChunkState::Meshed);
    CHECK(store.dirty(handle) == DirtyFlags{});
}

TEST_CASE("M15: snapshot dispatch applies height and hex overlays") {
    TerrainMeshingContext context;
    ChunkStore heights(1);
    const ChunkHandle heightHandle = readyChunk(heights, {}, PolicyKind::Height);
    HeightSample heightEdit;
    heightEdit.height = 42.5f;
    heightEdit.biomeId = 3;
    heightEdit.materialWeights = {{0, 0, 0, 255}};
    const std::uint32_t heightIndex = heightSampleIndex(17, 23);
    REQUIRE(heights.edit(heightHandle, heightIndex, CellValue::heightValue(heightEdit)));
    const auto heightSnapshot = heights.snapshot(heightHandle);
    REQUIRE(heightSnapshot.has_value());
    const auto heightBuild = meshChunkSnapshot(*heightSnapshot, context);
    REQUIRE(heightBuild.hasValue());
    CHECK(heightBuild.value().mesh.vertices[heightIndex].position.y == heightEdit.height);
    CHECK(heightBuild.value().mesh.vertices[heightIndex].materialWeights ==
          heightEdit.materialWeights);

    PolicyParams params;
    params.hex.radius = 1;
    ChunkStore hexes(1);
    const ChunkHandle hexHandle = readyChunk(hexes, {}, PolicyKind::Hex, params);
    const auto beforeEdit = hexes.snapshot(hexHandle);
    REQUIRE(beforeEdit.has_value());
    const HexTile* tile = std::get_if<HexTile>(&beforeEdit->base);
    REQUIRE(tile != nullptr);
    const auto centerIndex = tile->index({0, 0});
    REQUIRE(centerIndex.has_value());
    REQUIRE(hexes.edit(hexHandle, *centerIndex, CellValue::hexValue({7, 11})));
    const auto hexSnapshot = hexes.snapshot(hexHandle);
    REQUIRE(hexSnapshot.has_value());
    const auto hexBuild = meshChunkSnapshot(*hexSnapshot, context);
    REQUIRE(hexBuild.hasValue());
    bool foundEditedSurface = false;
    for (const TerrainSurface& surface : hexBuild.value().mesh.surfaces) {
        if (surface.material == 7) foundEditedSurface = true;
    }
    CHECK(foundEditedSurface);
}

TEST_CASE("M15: boundary edits invalidate only present same-policy neighbors") {
    ChunkStore store(6);
    const ChunkHandle center = readyChunk(store, {}, PolicyKind::Voxel);
    const ChunkHandle east = readyChunk(store, {1, 0, 0}, PolicyKind::Voxel);
    const ChunkHandle north = readyChunk(store, {0, 0, 1}, PolicyKind::Voxel);
    const ChunkHandle corner = readyChunk(store, {1, 1, 1}, PolicyKind::Voxel);
    REQUIRE(store.markMeshed(center, store.version(center)));
    REQUIRE(store.markMeshed(east, store.version(east)));
    REQUIRE(store.markMeshed(north, store.version(north)));
    REQUIRE(store.markMeshed(corner, store.version(corner)));
    const std::uint64_t eastVersion = store.version(east);
    const std::uint64_t northVersion = store.version(north);

    REQUIRE(store.edit(center, voxelIndex(12, 12, 12), CellValue::voxelValue(0)));
    CHECK(store.state(east) == ChunkState::Meshed);
    CHECK(store.version(east) == eastVersion);
    REQUIRE(store.markMeshed(center, store.version(center)));

    REQUIRE(store.edit(center, voxelIndex(31, 12, 12), CellValue::voxelValue(0)));
    CHECK(store.state(east) == ChunkState::CellsReady);
    CHECK(store.dirty(east) == DirtyFlags{false, true, true});
    CHECK(store.version(east) == eastVersion + 1u);
    CHECK(store.state(north) == ChunkState::Meshed);
    CHECK(store.version(north) == northVersion);

    REQUIRE(store.markMeshed(center, store.version(center)));
    REQUIRE(store.markMeshed(east, store.version(east)));
    const std::uint64_t cornerVersion = store.version(corner);
    REQUIRE(store.edit(center, voxelIndex(31, 31, 31), CellValue::voxelValue(2)));
    CHECK(store.state(corner) == ChunkState::CellsReady);
    CHECK(store.dirty(corner) == DirtyFlags{false, true, true});
    CHECK(store.version(corner) == cornerVersion + 1u);
}

TEST_CASE("M15: shared height edits mirror into all live tiles at a corner") {
    ChunkStore store(4);
    const ChunkHandle center = readyChunk(store, {}, PolicyKind::Height);
    const ChunkHandle east = readyChunk(store, {1, 0, 0}, PolicyKind::Height);
    const ChunkHandle north = readyChunk(store, {0, 0, 1}, PolicyKind::Height);
    const ChunkHandle diagonal = readyChunk(store, {1, 0, 1}, PolicyKind::Height);
    for (ChunkHandle handle : {center, east, north, diagonal}) {
        REQUIRE(store.markMeshed(handle, store.version(handle)));
    }

    HeightSample edit;
    edit.height = 91.25f;
    edit.biomeId = 8;
    edit.materialWeights = {{1, 2, 3, 249}};
    REQUIRE(store.edit(center, heightSampleIndex(kHeightQuads, kHeightQuads),
                       CellValue::heightValue(edit)));
    REQUIRE(store.cell(east, heightSampleIndex(0, kHeightQuads)).has_value());
    REQUIRE(store.cell(north, heightSampleIndex(kHeightQuads, 0)).has_value());
    REQUIRE(store.cell(diagonal, heightSampleIndex(0, 0)).has_value());
    CHECK(store.cell(east, heightSampleIndex(0, kHeightQuads))->height == edit);
    CHECK(store.cell(north, heightSampleIndex(kHeightQuads, 0))->height == edit);
    CHECK(store.cell(diagonal, heightSampleIndex(0, 0))->height == edit);
    for (ChunkHandle handle : {east, north, diagonal}) {
        CHECK(store.state(handle) == ChunkState::CellsReady);
        CHECK(store.dirty(handle) == DirtyFlags{true, true, true});
    }
}

TEST_CASE("M15: hex border edits dirty the matching axial neighbor chunk") {
    PolicyParams params;
    params.hex.radius = 2;
    ChunkStore store(2);
    const ChunkHandle center = readyChunk(store, {}, PolicyKind::Hex, params);
    const ChunkHandle east = readyChunk(store, {1, 0, 0}, PolicyKind::Hex, params);
    REQUIRE(store.markMeshed(center, store.version(center)));
    REQUIRE(store.markMeshed(east, store.version(east)));
    const auto snapshot = store.snapshot(center);
    REQUIRE(snapshot.has_value());
    const HexTile* tile = std::get_if<HexTile>(&snapshot->base);
    REQUIRE(tile != nullptr);
    const auto edgeIndex = tile->index({2, 0});
    REQUIRE(edgeIndex.has_value());
    REQUIRE(store.edit(center, *edgeIndex, CellValue::hexValue({7, 3})));
    CHECK(store.state(east) == ChunkState::CellsReady);
    CHECK(store.dirty(east) == DirtyFlags{false, true, true});
}

TEST_CASE("M15: policy mesh dumps retain deterministic golden digests") {
    BlockRegistry blocks;
    VoxelCells voxels = emptyVoxels();
    for (std::uint32_t z = 4; z < 9; ++z) {
        for (std::uint32_t x = 3; x < 11; ++x) {
            for (std::uint32_t y = 2; y < 6; ++y) {
                voxels.values[voxelIndex(x, y, z)] = ((x + z) % 3u == 0u) ? 1u : 2u;
            }
        }
    }
    const auto voxel = meshVoxelChunk(voxels, blocks);
    REQUIRE(voxel.hasValue());
    HeightMeshingOptions heightOptions;
    heightOptions.transitionFloor = -4.0f;
    const auto height = meshHeightChunk(generateHeightTile(0x1515u, {-2, 0, 5}, {}),
                                        {-2, 0, 5}, heightOptions);
    HexPolicyParams hexParams;
    hexParams.radius = 2;
    HexMeshingOptions hexOptions;
    hexOptions.floorHeight = -8.0f;
    const auto hex = meshHexChunk(generateHexTile(0x1515u, {3, 0, -4}, hexParams),
                                  {3, 0, -4}, hexOptions);
    REQUIRE(height.hasValue());
    REQUIRE(hex.hasValue());
    const std::uint64_t voxelHash = meshDigest(voxel.value());
    const std::uint64_t heightHash = meshDigest(height.value());
    const std::uint64_t hexHash = meshDigest(hex.value());
    INFO("voxel mesh digest: " << voxelHash);
    INFO("height mesh digest: " << heightHash);
    INFO("hex mesh digest: " << hexHash);
    CHECK(voxelHash == 13321112537366601358ull);
    CHECK(heightHash == 9092355690008189066ull);
    CHECK(hexHash == 1150422022661611982ull);
}
