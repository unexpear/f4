# Isolated Boundary Wall Geometry (2026-09-07)

Follow-up: [coplanar face trimming](boundary-face-trimming.md) now removes covered
old wall strips on private copies. Coordinated publication and top-edge stitching
remain open; the verification below describes the earlier isolated wall builder.

## Findings And Plan

Complete height/hex profiles describe the two tops but do not connect them.
Existing height skirts and optional hex cut walls can already occupy the same
boundary plane. Simply publishing another wall there would risk coplanar
overdraw and incorrect query hits. This increment implements and verifies the
pure geometry step without changing those working meshes. **M16 remains partial.**

Inspected profile extraction, asynchronous snapshot ownership, region clipping,
height skirts, hex cut walls, the canonical raster state and triangle ray tests.
The engine uses CCW front faces by default. Khronos documents that culling is
based on projected triangle winding, not vertex normals; generated geometry is
therefore checked through both cross products and backface-culled rays.
[Khronos face culling](https://wikis.khronos.org/opengl/Face_Culling).
Float conversion also needs range and topology checks, not just casts.
[Microsoft standard conversions](https://learn.microsoft.com/en-us/cpp/cpp/standard-conversions?view=msvc-170).

## Changes

- `meshTerrainBoundaryWalls` accepts one complete ordered profile partition on
  an X/Z plane. Every span must have exactly one source on both sides. Invalid,
  incomplete, overlapping or discontinuous interval partitions fail; no partial
  mesh is returned. Height discontinuities between adjacent spans are preserved,
  not smoothed into invented top surfaces.
- Within each span the higher side owns the wall and its normal points toward
  the lower side. A strict height crossing is split at one shared point, with
  opposite winding/ownership on either side. Identical tops emit no triangles;
  equality at one endpoint emits a single nondegenerate triangle.
- Output is ordinary world-space float `MeshData` plus one provenance record
  per triangle identifying the input span and owning side. Input spans retain
  source/triangle indices. Surface ranges and vertex material ids use explicit
  caller-supplied per-side wall materials; UVs are tangent/height in world meters.
  The diagnostic uses ids 13/29, not inferred imported texture bindings.
- Coordinates are checked before narrowing. The boundary plane must be exactly
  representable as float. Tangent spans, crossing splits and nonzero endpoint
  height ordering must survive float conversion; collapse fails explicitly.
  There is no epsilon welding or silent dropping of tiny positive-area faces.
  Ordinary endpoint rounding remains: this is not bit-exact welding to every
  source top edge or proof against T-junction cracks.
- Input has at most 131,073 spans; the explicit output limit is 1..131,072
  triangles. Exhaustion is an error, including after a partly built local mesh.
  This bounds geometry counts, not main-thread time or total process memory.
- The native offline audit consumes accepted profile-job results and validates
  freshness again after geometry inspection. The pure builder is worker-safe
  for immutable inputs but is not yet scheduled by `BoundaryProfileJob`.
  No callback, renderer, source mesh or WorldStreamer publication was changed.
- New CSV files export actual float triangles, owner/span ids, total areas and
  ray counts. The new SVG projects those same triangles, not a filled idealized
  profile. CTest compares all three new artifacts across two native processes.

## Verification

- Four new tests / 329 assertions cover X/Z axes, both higher-side owners,
  signed positions, slopes, crossings, equality, discontinuities, material and
  provenance ranges, analytic areas, front/back rays, invalid input, missing or
  overlapping coverage, exact triangle limits and float/crossing collapse.
- Native east boundary: 160 triangles, 1,523.4898220087719 m2, 160 front/back
  culled ray pairs. North: 146 triangles, 1,613.0331565168237 m2, 146 pairs.
  Every generated triangle is nondegenerate and outward wound in this fixture.
- Visually inspected `output/playwright/height-hex-walls.png`, a 1000x700 browser
  render of the generated SVG. Both strips are continuous at that display scale,
  triangulation and labels are legible, and the isolated/non-published status is
  explicit. This is a triangle diagram, not GPU-rendered in-world wall acceptance.
  The final SVG hashes match the inspected version.
- GCC Release: 9/9 gates; 522 core cases / 467,911 assertions.
- MSVC AddressSanitizer: 6/6 gates; 472 core cases / 466,568 assertions;
  existing 17 driver exclusions unchanged. A GCC range-loop copy warning in the
  new test was fixed with a const reference; warning settings were not relaxed.
- WebGPU: 1/1 browser smoke; build up to date. This native module is not in that
  app; the browser smoke is regression coverage, not wall feature coverage.
- All 342 prior BMPs remain byte-identical: 148 policy, 38 height, 156 static.
  Profile CSV/summary remain unchanged. The prior profile SVG changes only its
  status wording from no bridge generated to no bridge published; the gallery
  additionally links the isolated wall evidence.
- Baselines: policy `dfdc30d5eff4`, height `6fbc40abbc2d`, static `a919633658a5`.
  Final galleries under `build/release/`: policy
  `streaming-policy-captures/8614ed6932a2/index.html`, height
  `streaming-demo-captures/9ff886a0e161/index.html`, static
  `static-demo-captures/3111a4e7dfaf/index.html`.
- Assets untouched; build/test TEMP/TMP on D:. No new Linux/Clang arithmetic
  parity, physical Metal, TSan, hard-budget or moving-camera seam acceptance.

## Remaining Work

The wall builder assumes solid columns down to the lower profile, not arbitrary
cavities, overhangs or finite-prism bottom transitions. A complete profile alone
does not establish those solid semantics. It also does not deduplicate separate
requests for the same boundary or reconcile multiple boundaries at corners.

Next define a replacement build that removes/splits only the covered coplanar
skirt/cap portions, reconciles top-edge subdivisions and crossings, and retains
uncovered faces and material provenance. Publish source replacements and the
uniquely owned wall together under the dependency freshness gate to rendering
and shared triangle queries. Then integrate boundary discovery, asynchronous
geometry jobs, global budgets and moving-camera invalidation tests. Do not mark
physical closure complete based on this isolated builder or its diagram.
