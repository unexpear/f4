# Canonical Boundary Top-Edge Stitching (2026-09-07)

Follow-up: [immutable replacement packages](boundary-replacement-packages.md)
combine this geometry with trims and walls in one common world-space mesh.
Multi-boundary composition and actual GPU/world publication remain open.

## Findings And Plan

The wall and trim builders existed, but the wall's profile/crossing subdivisions
could meet an unsplit source edge. Matching endpoints alone does not eliminate
those T-junctions. This increment exposes the wall's canonical edge records and
splits existing source top edges to match them on private copies. **M16 remains
partial; no world meshes, rendering or shared published queries are changed.**

Inspected both builders, CPU clipping/interpolation, source provenance and the
native audit before coding. Khronos describes the importance of identical shared
edge vertices for avoiding cracks in its tessellation invariance rules. This CPU
implementation uses that geometric requirement; it does not claim to implement
GPU tessellation or establish shader-transform invariance.
[OpenGL 4.3 specification, tessellation invariance](https://registry.khronos.org/OpenGL/specs/gl/glspec43.core.pdf).
The cut list is validated and sorted explicitly, consistent with the standard
library ordering contract.
[Microsoft algorithm documentation](https://learn.microsoft.com/en-us/cpp/standard-library/algorithm-functions?view=msvc-170).

## Changes And Contract

- `TerrainBoundaryWallBuild::topEdges` records both sides of each wall piece,
  retaining exact tangent parameters and heights. Height crossings share the
  same endpoint record; equal-height pieces retain edge records even though no
  wall face is emitted. Existing wall triangles and material ranges are unchanged.
- `stitchTerrainBoundaryTops` accepts explicit source triangle ids and canonical
  wall points. It supports an existing upward-wound edge on an X/Z boundary,
  not an interior cut through a triangle, underside or arbitrary polygon.
- Cuts may be unordered or repeated. They are sorted deterministically by source
  triangle, tangent and height. The source edge independently evaluates the
  height at the exact tangent parameter; its rounded height must equal the wall
  point. Conflicting points fail instead of tolerance-based welding.
- New local-space vertices must reconstruct the exact canonical world float
  coordinates when translated. Distinct points collapsing onto each other or
  source endpoints fail. Large-offset or incompatible-rounding inputs can be
  rejected; arbitrary-world support is not claimed.
- Each affected triangle becomes a fan from its opposite vertex through the
  ordered boundary cuts. Upward winding and nonzero projected area are checked.
  Continuous/categorical attributes use the existing region-clipping rules.
  Unaffected indices/vertices remain unchanged; no-op layouts remain exact.
- Every output triangle maps to its input id. In the native audit this mapping
  composes with the preceding trim's original-triangle mapping. Source meshes
  and cut inputs must remain immutable during this pure CPU operation.
- Input cap is 524,292 cut records; output triangle limit is 1..131,072.
  Source vertex copies, edge metadata and allocation overhead are not a global
  memory bound. Allocation failures are not converted into status codes; no
  hard-budget or real-time scheduling acceptance is claimed.
- The native audit builds cuts from actual wall edge records and trimmed-source
  profile provenance. Every wall-top segment must match exactly one complete
  edge of the corresponding stitched source triangle, using exact translated
  CPU coordinates rather than a distance tolerance. `height_hex_stitch.csv`
  records per-source counts and is compared across two independent processes.

## Verification

- Three new stitch tests / 350 assertions cover both axes, signed translations,
  cut sorting/repetition, no-op endpoints, attributes/material weights, winding,
  area, ray heights, provenance, invalid inputs, unsupported triangles, exact
  triangle limits and float collapse.
- An end-to-end crossing test extracts both source profiles, builds the wall,
  feeds its records to both source splitters and requires identical crossing
  vertices. Existing wall tests add 13 assertions for crossing/equal-top records.
- East boundary: 17 hex plus 16 height triangles split; 63 plus 16 vertices
  added; 80 exact edge matches per side. North: 10 plus 9 triangles split;
  63 plus 9 vertices added; 73 exact edge matches per side. Total: 52 source
  triangles split, 151 vertices added, 306 complete source edges matched.
- GCC Release: 9/9 gates; 528 core cases / 468,842 assertions.
- MSVC AddressSanitizer: final 6/6 gates; 478 core cases / 467,499 assertions,
  including the end-to-end crossing test. Existing 17 driver exclusions unchanged.
- WebGPU browser smoke: 1/1, build up to date. This native module is not compiled
  into that app; the browser smoke is regression coverage only.
- All 342 previous BMPs are byte-identical: 148 policy, 38 height and 156 static.
  All seven prior profile/wall/trim CSV and SVG files also remain byte-identical.
  No new visual or in-world GPU seam acceptance is claimed; stitching evidence
  is numerical and applies to private CPU copies.
- Baselines: policy `6713d6106144`, height `108c7ef305b7`, static `05867711105e`.
  Final galleries under `build/release/`: policy
  `streaming-policy-captures/d92b8e9bc8ee/index.html`, height
  `streaming-demo-captures/fdd2383ad52f/index.html`, static
  `static-demo-captures/f50e9df879ca/index.html`.
- Assets untouched; build/test TEMP/TMP stayed on D:. No new Linux/Clang parity,
  physical Metal, TSan, global budget or moving-camera acceptance.

## Next

Package trimmed/stitched source meshes and uniquely owned walls in one immutable
replacement with original provenance and dependency stamps. Compose multiple
boundaries without overwriting earlier edits to the same source mesh. Publish
the whole replacement consistently to rendering and shared triangle queries,
rejecting stale results before either consumer accepts them.

CPU coordinate agreement is not sufficient to establish equal GPU clip-space
positions when source meshes and walls use different transforms or arithmetic.
Use a consistent transform path and verify it with in-world static and moving
camera tests. Solid-column assumptions, boundary discovery, asynchronous geometry
jobs, global budgets, multi-boundary corners and unsupported precision cases
remain explicit acceptance work. Do not mark M16 closure complete from this audit.
