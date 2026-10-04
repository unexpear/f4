# Solid-Column Ownership (2026-09-28)

Reference volume contract for the boundary and corner columns this tree can
already describe. M16 stays partial. `WorldStreamer::classifyPublishedSolid`
uses this contract for published Height/Hex columns and for interior voxel
columns read from retained horizontal mesh faces. It does not change voxel
DDA or bilinear height sampling. A region plane that touches Voxel still has
no owner.

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
- `classifyVoxelStack` still names `VoxelOverhang` when a bool stack has two
  solid runs and `VoxelCavity` when the only run sits above air. A
  bottom-anchored bool stack is still `UnsupportedVoxel`. That function never
  returns an owner and is not the streaming walker.
- `classifyVoxelColumn` owns a published stack of closed solid spans. A sample
  inside a span is solid Voxel. Air in that column, including the gap of a
  cave or overhang, is an empty Voxel sample. A single top-and-floor
  `TerrainSolidColumn` still rejects Voxel.

Incomplete profiles, unrepresentable planes and float-collapsed transition
pieces fail closed with the same errors as the wall builder. No epsilon weld
and no partial owner.

## Verification

`tests/engine/test_terrain_solid.cpp` covers wall-centroid agreement, crossing
ownership, buried samples below the wall band, discontinuous breakpoints, hex
floors, corner resolution, nonmanifold joins, inconsistent sectors, precision
collapse, nonplanar tops versus bilinear height, the named voxel-stack
refusals, and voxel span ownership for a solid run, an air gap, and an upper run.

## Streaming query

`classifyPublishedSolid` reads retained published meshes, including while a
remesh is in flight. Interior Height/Hex samples use `sampleUpwardTriangleHeight`
on the upward published triangle. A sample on one region plane uses
`classifyTerrainBoundarySolid` over the paired profile of that region edge. A
sample on both planes uses `classifyTerrainCornerSolid`. An interior voxel
sample walks horizontal faces of the retained voxel meshes. An upward face
closes a solid run and a downward face opens the next one. Solid runs are
owned by Voxel. Air in the same column is an empty Voxel sample, including
the gap punched through a published stack. A face stack that does not close,
or two columns that disagree on a shared edge, stays unpublished or
ambiguous. A region plane or corner that touches Voxel, including the
resident Voxel/Hex edge, stays `UnsupportedVoxel` with no owner.
`classifyVoxelStack` is unchanged.

## Still Open

Full-world policy coverage remains open. Interior published voxel columns now
have an owner. Displayed raycasts read the engine accepted-view slot. Plain
sync does not draw a package that slot rejects. An opt-in hard deadline refuses
the next streaming unit and the next copied entry of a shared region map. It
does not preempt the unit already started, and overrun stays visible.
`deterministicAdmission` ignores that deadline. The two-process gate hashes one residency row per world-update tick of
the eight-step sweep, and one presented `sweep_tick_*.bmp` for each of those
ticks. The present uses the view already installed for that tick. It does not
advance world admission or the wall-clock upload budget. That verify path uses
fixed work-unit admission (per-frame count caps), not a hard realtime deadline.
Interactive pacing is unchanged. Discovery returns a Voxel/Hex edge that is inside the window. The published
column on that edge is `UnsupportedVoxel` and has no owner, because the sample
is on the region plane rather than inside one voxel cell. Height/Voxel still
fails the whole window and is not omitted. The resident bounding union of the
mixed world still fails for that reason. Scoped Height/Hex composition keeps
the pair-adjacency rectangle, and a Height/Hex scope fails a window that
actually contains the Voxel/Hex edge. `classifyVoxelStack` still returns no
owner. Surface rays are not this contract.
