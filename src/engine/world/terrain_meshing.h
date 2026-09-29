#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

#include "core/containers/array.h"
#include "core/math/vec2.h"
#include "core/math/vec3.h"
#include "core/resources/mesh.h"
#include "core/result.h"
#include "engine/world/world_data.h"

namespace f4::world {

struct TerrainVertex {
    math::Vec3 position{};
    math::Vec3 normal{0.0f, 1.0f, 0.0f};
    math::Vec2 uv{};
    std::array<std::uint8_t, 4> color{{255, 255, 255, 255}};
    std::array<std::uint8_t, 4> materialWeights{{255, 0, 0, 0}};
    std::uint16_t material = 0;
    std::uint16_t biome = 0;
    std::uint8_t ambientOcclusion = 3;
};

struct TerrainSurface {
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::uint16_t material = 0;
};

// Every terrain policy emits this record. TerrainSurface maps contiguous index
// ranges to policy material ids; vertices/indices are otherwise RHI-neutral.
struct MeshData {
    containers::Array<TerrainVertex> vertices;
    containers::Array<std::uint32_t> indices;
    containers::Array<TerrainSurface> surfaces;
};

enum class TerrainMeshError : std::uint8_t {
    InvalidData,
    MissingBlockRegistry,
    UnknownBlock,
    CoordinateOverflow,
    RegionLimitExceeded,
};

// Converts MeshData to the canonical M11 position/normal/UV/color vertex
// layout. Surface material ids remain in MeshData; each range becomes one
// resources::SubMesh so callers can bind the corresponding material.
[[nodiscard]] auto toResourceMesh(const MeshData& source) -> resources::Mesh;

struct VoxelMeshingOptions {
    // Local chunk coordinates. The mesher requests only the one-cell border
    // [-1, 32] and always uses the supplied center VoxelCells for [0, 31].
    VoxelFetch borderFetch;
    // Optional ownership mask over the same local [-1,32] padded coordinates.
    // Applied to center AND border cells before face culling/AO, without changing
    // input cells. Rejected cells behave as air; original block ids are validated.
    std::function<bool(std::int32_t, std::int32_t, std::int32_t)> cellOwned{};
};

using HeightFetch = std::function<float(std::int64_t, std::int64_t)>;
struct HeightMeshingOptions {
    float skirtDepth = 4.0f;
    // A policy transition can force all skirt bottoms to one world height.
    std::optional<float> transitionFloor;
    // Global sample coordinates; used for the one-sample normal border.
    HeightFetch borderFetch;
    // Bits 0..3 select -Z/+X/+Z/-X skirts. Default preserves standalone meshes.
    std::uint8_t skirtEdges = 0x0f;
};

using HexNeighborFetch = std::function<bool(AxialCoord)>;
struct HexMeshingOptions {
    float radius = 1.0f;
    float floorHeight = -8.0f;
    // Global axial coordinates. A present neighbor suppresses the shared wall.
    HexNeighborFetch hasNeighbor;
    // Construct positions directly from global integer corners in one frame.
    // Caller must use zero mesh translation. Default preserves local meshes.
    bool worldSpace = false;
};

// Canonical global lattice construction, rounded once to the mesh float format.
// Shared by world-space hex vertices and their conservative admission bounds.
[[nodiscard]] auto hexLatticePosition(HexCornerCoord lattice, float radius)
    -> Result<math::Vec2, TerrainMeshError>;

[[nodiscard]] auto meshVoxelChunk(const VoxelCells& cells,
                                  const BlockRegistry& registry,
                                  const VoxelMeshingOptions& options = {})
    -> Result<MeshData, TerrainMeshError>;
[[nodiscard]] auto meshHeightChunk(const HeightTile& tile, ChunkCoord coord,
                                   const HeightMeshingOptions& options = {})
    -> Result<MeshData, TerrainMeshError>;
[[nodiscard]] auto meshHexChunk(const HexTile& tile, ChunkCoord coord,
                                const HexMeshingOptions& options = {})
    -> Result<MeshData, TerrainMeshError>;

struct TerrainMeshingContext {
    const BlockRegistry* blocks = nullptr;
    VoxelMeshingOptions voxel;
    HeightMeshingOptions height;
    HexMeshingOptions hex;
};

struct TerrainMeshBuild {
    ChunkCoord coord{};
    PolicyKind policy = PolicyKind::Voxel;
    std::uint64_t sourceVersion = 0;
    MeshData mesh;
};

// Applies the sorted M14 delta overlay to a private base copy before meshing.
[[nodiscard]] auto meshChunkSnapshot(const ChunkSnapshot& snapshot,
                                     const TerrainMeshingContext& context)
    -> Result<TerrainMeshBuild, TerrainMeshError>;

// Main-thread publication gate. A result built from an obsolete snapshot is
// rejected by ChunkStore::markMeshed and cannot clear newer dirty flags.
[[nodiscard]] auto commitTerrainMesh(ChunkStore& store, ChunkHandle handle,
                                     const TerrainMeshBuild& build) -> bool;

} // namespace f4::world
