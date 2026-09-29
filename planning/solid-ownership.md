# Solid-Column Ownership (2026-09-28)

Reference volume contract for the boundary and corner columns this tree can
already describe. M16 stays partial. This does not install a new WorldStreamer
query, change voxel DDA, or change bilinear height sampling.

## Contract

`classifyTerrainColumn`, `classifyTerrainBoundarySolid` and
`classifyTerrainCornerSolid` answer whether a finite height is inside a solid
column and which sector owns it.

- Height columns are closed at the published-triangle top and open downward.
  `sampleUpwardTriangleHeight` is that top. On a nonplanar quad it disagrees
  with bilinear sampling; the bilinear API is unchanged and is not the solid top.
- Hex columns are closed prisms from an explicit floor to a strictly higher top.
  Below the floor is empty. A top at or below the floor is `HexFloorInversion`.
- On a boundary plane or corner, the greatest containing top wins. Equal tops
  keep the canonical sector: negative then positive, or NE, NW, SW, SE.
  The wall band runs from the lowest strictly shorter incident top through the
  owner's top, including the lower edge of `meshTerrainBoundaryWalls`.
- Crossing spans keep that builder's split, so a wall-triangle centroid and the
  column owner name the same side.
- A shared profile breakpoint whose endpoint heights differ has no positive-area
  triangle. The sample is `DiscontinuousHeight`. Neighboring spans still
  classify, and the existing wall builder still emits their faces.
- Corner arms use the junction sector map. Conflicting heights are
  `InconsistentSector`. Distinct heights that collapse to one float are
  `PrecisionLoss`. Alternating solid and empty sectors, including alternation
  caused by a hex floor, are `NonmanifoldCorner` for every sample on that corner.
- Voxel columns are unsupported. `classifyVoxelStack` names `VoxelOverhang`
  when a vertical stack has two solid runs and `VoxelCavity` when the only run
  sits above air. A bottom-anchored run is still `UnsupportedVoxel`. None of
  these return an owner.

Incomplete profiles, unrepresentable planes and float-collapsed transition
pieces fail closed with the same errors as the wall builder. No epsilon weld
and no partial owner.

## Verification

`tests/engine/test_terrain_solid.cpp` covers wall-centroid agreement, crossing
ownership, buried samples below the wall band, discontinuous breakpoints, hex
floors, corner resolution, nonmanifold joins, inconsistent sectors, precision
collapse, nonplanar tops versus bilinear height, and the named voxel failures.

## Still Open

Full-world policy coverage, renderer use of the engine accepted-view slot,
continuous moving coverage, budget acceptance, and a general voxel solid query
remain open. Surface rays are not this contract.
