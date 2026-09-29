# Regional Surface Ownership (2026-09-06)

Historical surface-only increment. The later [hex cut-wall increment](hex-cut-walls.md)
adds a separate opt-in prism cap mode; the [voxel cut-face increment](voxel-cut-faces.md)
adds cell-masked voxel cuts. Defaults and the evidence below are unchanged.

## Findings And Scope

The old meter fixture selected only two hex chunk centers in the 64 m hex
region. Two other chunks intersect the region but have centers outside it:
(-1,0,1) and (1,0,0). Whole admitted chunks also extend into other policies.

This increment adds opt-in `WorldStreamingConfig::regionalSurfaceOwnership`.
It combines intersection admission with CPU triangle clipping, rather than
changing the shader or hiding defects with deeper skirts. The tested hex
interior holes are filled, including the previous ray miss at XZ (8,56).
**M16 remains partial:** clipped edges are not capped, unequal terrain heights
are not bridged, and solid collision/volume ownership is not implemented.

## Contract And Compatibility

- Existing center rings/meter footprints and unclipped publication remain the
  default. The new flag changes regional admission, not the camera footprint:
  candidate centers must still pass that footprint and nearest-capacity caps.
- Height/voxel square bounds and conservative hex corner-lattice bounds are
  tested for positive-area intersection with owned region tiles. Bounds use
  the same translation and local-coordinate arithmetic as meshing/rendering.
  An AABB-only corner intersection can admit a chunk whose clipped mesh is empty.
  Finite residency, legacy spaced hex layouts and truncated capacity can still
  leave holes; this is not a global coverage guarantee.
- Workers snapshot the RegionMap with the existing immutable copy-on-write
  storage, generate from the original chunk plus sparse edits, then clip its
  triangles to the closures of owned rectangles. Cell indices, procedural
  recipes, stored edits and save formats do not change. Hidden edits can become
  visible again after policy restoration.
- Clipping preserves winding, surface material ranges and interpolated
  normal/UV/color/AO values. Material weights use largest-remainder rounding to
  retain a sum of 255. Categorical material/biome attributes select the dominant
  original triangle corner, with lowest-corner-index ties. Fully owned meshes
  retain their original layout. Empty ownership produces an empty mesh.
- Region tiles are processed in Z/X order. Intersections canonicalize edge
  direction and snap the cut coordinate before converting back to local float
  vertices. Internal coplanar region faces are emitted once per source triangle.
  Distinct policy closures may still share vertical boundary faces. No epsilon
  overlap strip or generated wall conceals them. Float placement is not an
  exact-real arithmetic guarantee; fixture bounds are checked within 1e-5 m.
- Region changes dirty retained chunks before completion processing, even when
  their keys do not change. Completion checks reject obsolete region revisions.
  `meshRegionRevision` identifies the published mask; it is null in legacy mode.
  Old triangles and their old version/revision remain observable while rebuilding.
- Rendering and `raycastPublishedTerrain` consume the same clipped CPU mesh.
  GPU upload can still lag. The old voxel DDA and bilinear sampler intentionally
  inspect resident cell data, NOT clipped surfaces, and must not be represented
  as matching regional solid collision. This distinction is explicit in headers.
- The public mask helper rejects invalid/nonfinite bounds, coordinates outside
  the int32 world domain, and requests examining more than 4096 region tiles.
  Opt-in construction validates layout/scale and a conservative worst-case tile
  count before starting workers. Unsupported far-world candidate bounds are
  skipped, not cast through integer overflow. Masks are never silently truncated.
- This is a bounded reference implementation, not a hard frame-budget result.
  Region edits currently dirty every resident mesh; per-triangle clipping uses
  temporary vectors. Large-map mutation costs, acceleration and moving-camera
  coverage still need measurement and separate acceptance.

## Research

The clipping pipeline follows successive half-plane polygon clipping described
by the original [Utah PLOT79 implementation documentation](https://www.math.utah.edu/software/plot79/plot79_c/clpsh2.html),
which references Sutherland and Hodgman's 1974 reentrant polygon clipping paper.
No source code was copied. Unlike that historical routine's capacity behavior,
this implementation returns explicit errors instead of overwriting excess output.
Geometry ownership, translation, edit replay and publication rules were verified
against the local meshers, RegionMap, ChunkStore and streaming adapter.

## Verification

- Five new cases / 9,397 assertions cover signed region masks, area partition,
  shared-edge rays, attributes, empty/full ownership, invalid data, work limits,
  coplanar walls, all three policies, map revisions and edit preservation.
- One/four-worker mixed worlds settle to 22 chunks: 14 height, four voxel and
  four hex. All 64 hex interior rays hit; published revisions match. Hex vertices
  stay in the region closure. Burst map edits, actual policy eviction and return
  preserve edits and match every fresh-reference vertex attribute, index and
  surface range for every chunk; restored sparse edits are checked explicitly.
- A 16 m region inside a 64 m height chunk verifies changed ownership with an
  unchanged height handle. Its stored height edit reappears after restoration.
- Windows GCC Release: 9/9 gates, 500 core cases / 347,419 assertions.
- MSVC AddressSanitizer: 6/6 gates, 450 core cases / 346,076 assertions;
  the existing 17 driver exclusions are unchanged. This is not race coverage.
- WebGPU rebuild and browser smoke: 1/1. The shared meshing header rebuilt;
  the new native clipping/WorldStreamer code is not browser acceptance.
- The native gate retains the original 112 BMPs byte-identically and adds three
  regional views repeated twice across two processes: 124 BMPs total. CPU mesh
  digests and the 64-row regional ray CSV must also match across processes.
- Visually reviewed the home, overhead and close comparison. The large hex interior
  holes disappear; the perimeter still exposes the unbridged height difference.
  This is not acceptance of watertight seams or blink-free moving cameras.
- All 156 static BMPs and 38 height BMPs match the preceding galleries
  `d3c6a7113968` and `69d284e7306d` respectively.
- Final policy gallery: `build/release/streaming-policy-captures/941951c6ff2b/index.html`.
  Height gallery: `build/release/streaming-demo-captures/a93d2ed6b830/index.html`.
  Static gallery: `build/release/static-demo-captures/1fffdc7a5208/index.html`.
  All 318 final BMPs match the preceding successful gate run, including the
  visually inspected `d11afee22b02` regional comparison. Two policy gates thus
  retain exact regional-image equality across four independent processes.
- D: TEMP/TMP used throughout; user assets remain untouched. No new Linux,
  macOS, physical Metal or TSan acceptance is claimed.

## Next

Define cut-boundary profiles and deterministic connecting/capping geometry with
a clear solid-volume contract, including voxel cavities and removed hex cells.
Publish it through the same edit/version pipeline and test it with both surface
rays and the eventual volume query. Add near-boundary motion, overlap and closure
checks before considering the seams complete. Keep this partial mode opt-in.
