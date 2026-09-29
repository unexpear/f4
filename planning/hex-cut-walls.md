# Regional Hex Cut Walls (2026-09-06)

Historical hex increment. The later [voxel cut-face increment](voxel-cut-faces.md)
adds a separate cell-masked mode that preserves voxel cavities. Evidence below
describes the hex checkpoint; height transitions remain open.

## Findings And Scope

Hex meshing already defines a prism volume: a piecewise-triangle top, a fixed
`hexFloorHeight` bottom, and exposed perimeter walls. Regional surface clipping
removed triangles outside owned tiles without closing the resulting cuts.
Unlike a height skirt, a hex cut has an existing, unambiguous lower boundary.

`regionalHexCutWalls` now closes those cuts to that floor. It requires
`regionalSurfaceOwnership`; both remain off by default. This is not a raised
skirt, a bridge between terrain policies, or generic hole filling.
**M16 remains partial.** Height transitions, voxel cuts/cavities, general solid
queries and moving-camera closure acceptance remain open.

## Implementation

- The CPU clipper accepts an optional local-space hex floor, supplied from the
  same immutable job settings as `meshHexChunk`. It requires Hex policy, finite
  inputs, upward tops above the floor, and downward bottoms on that floor.
  A mismatched floor is rejected, including on the fully-owned fast path.
  This validates the prism-specific input contract, not arbitrary mesh topology.
- Every positive-area clipped top polygon contributes its edges on foreign
  policy region planes. Two outward-facing triangles connect each such edge to
  the actual prism floor. No cap is generated at an internal same-policy region
  line or merely along a chunk boundary. Tangential/degenerate polygons add none.
- Coplanar input walls on capped region planes are removed and replaced by the
  top-derived wall segments. This avoids duplicate coplanar geometry/depth ties.
  Top and bottom surfaces retain their clipped geometry. The operation may
  still leave T-junctions in triangle-soup connectivity; no globally indexed
  manifold mesh or exact-real arithmetic guarantee is claimed.
- Caps inherit edge material/color/weights, use axis-aligned outward normals,
  and have meter-based wall UVs. They use the existing bounded index append path.
  Default surface-only output and same-policy fully-owned meshes are preserved.
- Publication, stale region-version rejection, edit replay and GPU upload use
  the existing path. `raycastPublishedTerrain` sees the new cap triangles with
  the same mesh version and region revision as their CPU render source.
  GPU upload may lag. Legacy voxel DDA and height sampling remain unchanged.
- The current hex mesher rejects invalid terrain cells; removing hex cells is
  not implemented as a hole/void editing contract. These caps do not invent one.
  Voxel overhang/cavity handling cannot reuse this top-to-floor algorithm safely.

## Research

Local inspection established the hex prism floor and winding contract before
implementation. The official [CGAL mesh-processing manual](https://doc.cgal.org/latest/Polygon_mesh_processing/index.html)
distinguishes mesh topology from geometric validity and requires closed input
for its inside/outside query. That supports keeping this prism-specific cap
operation separate from a general solid-query claim. No CGAL dependency or code
was added; the existing CPU half-plane clipper is reused.

## Verification

- Two new primitive cases and expanded streaming cases bring the focused set
  to eight cases / 21,355 assertions. Flat prisms test all four wall directions,
  signed translation, above/below-floor misses, inside back-face hits, outward
  culling and the expected 640 cubic-meter signed volume. Additional tests check
  wrong floors, internal same-policy lines and replacement of existing coplanar
  walls with unchanged area, using a generated nonflat hex mesh.
- The one/four-worker mixed reference test now runs with caps both off and on.
  It verifies horizontal wall rays, region provenance, rapid map changes,
  policy eviction/return, explicit stored edits and equality of every vertex
  attribute, index and surface range against a fresh edited reference.
- Windows GCC Release: 9/9 gates, 502 core cases / 359,366 assertions.
- MSVC AddressSanitizer: 6/6 gates, 452 core cases / 358,023 assertions;
  the existing 17 driver exclusions remain unchanged. This is not race coverage.
- WebGPU browser smoke: 1/1; its build was already up to date. The new cap
  implementation and native streaming path are not browser acceptance.
- Native policy gate: 136 BMPs, repeated within two independent processes;
  includes the prior 124 unchanged plus home, east-wall and corner cap views.
  Sixty-four top probes and 32 outward horizontal wall probes must hit Hex;
  CSVs and CPU mesh digests must match across processes, without relaxing
  pixel comparisons. The legacy meter fixture still records the old ray miss.
- The east/corner views visibly show closed prism side faces meeting at the
  corner. Height terrain remains disconnected above them. No full-scene
  watertightness or continuous-motion/no-pop-in acceptance is claimed.
- All 318 pre-existing BMPs match the prior galleries: 124 policy, 156 static,
  and 38 height. Assets are untouched; D: TEMP/TMP is used throughout.
- All 136 final policy BMPs also match the preceding successful gate run
  `0168d2952fe0`, whose east/corner views were visually reviewed. This gives
  exact image equality across four independent processes, not just frame repeats.
- Final policy gallery: `build/release/streaming-policy-captures/ba35bb09b848/index.html`.
  Height gallery: `build/release/streaming-demo-captures/cd32753e2089/index.html`.
  Static gallery: `build/release/static-demo-captures/4f66fe62629c/index.html`.
- No new Linux, Apple hardware, physical Metal or TSan acceptance is claimed.

## Next

Define connecting profiles for unequal height/hex surfaces and a separate
voxel-cut contract that preserves cavity/overhang semantics. Resolve shared
boundary ownership, geometric closure/overlap and solid queries before marking
regional seams complete. Add near-boundary motion and measured scheduling
budgets; fixed screenshots and signed volume checks alone are insufficient.
