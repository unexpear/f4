# Solid-Column Ownership (2026-09-28)

Reference volume contract for the boundary and corner columns this tree can
already describe. M16 stays partial. `WorldStreamer::classifyPublishedSolid`
uses this contract for published Height/Hex columns. It does not change voxel
DDA or bilinear height sampling, and it does not give voxel columns an owner.

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

## Streaming query

`classifyPublishedSolid` reads retained published meshes, including while a
remesh is in flight. Interior Height/Hex samples use `sampleUpwardTriangleHeight`
on the upward published triangle. A sample on one region plane uses
`classifyTerrainBoundarySolid` over the paired profile of that region edge. A
sample on both planes uses `classifyTerrainCornerSolid`. A voxel policy, a Voxel/Hex edge, or a
Height/Hex edge against voxel, returns `UnsupportedVoxel` and no owner.
`classifyVoxelStack` is unchanged and is not a streaming column walker.

## Still Open

Full-world policy coverage and a general voxel solid query remain open.
Displayed raycasts read the engine accepted-view slot. Plain sync does not
draw a package that slot rejects. An opt-in hard deadline refuses
the next streaming unit and the next copied entry of a shared region map. It
does not preempt the unit already started, and overrun stays visible.
`deterministicAdmission` ignores that deadline. The two-process gate hashes one residency row per world-update tick of
the eight-step sweep, and one presented `sweep_tick_*.bmp` for each of those
ticks. The present uses the view already installed for that tick. It does not
advance world admission or the wall-clock upload budget. That verify path uses
fixed work-unit admission (per-frame count caps), not a hard realtime deadline.
Interactive pacing is unchanged. Discovery returns a Voxel/Hex edge that is inside the window. The published
column on that edge is `UnsupportedVoxel` and has no owner. Height/Voxel still
fails the whole window and is not omitted. The resident bounding union of the
mixed world still fails for that reason. Scoped Height/Hex composition keeps
the pair-adjacency rectangle, and a Height/Hex scope fails a window that
actually contains the Voxel/Hex edge. Surface rays are not this contract.
