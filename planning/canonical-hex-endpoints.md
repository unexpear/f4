# Canonical Regional Hex Endpoints (2026-09-06)

The subsequent [boundary dependency increment](boundary-dependencies.md) adds
immutable two-policy capture and explicit freshness validation. Automatic
transition scheduling and wall generation remain open; evidence below is unchanged.

## Findings And Research

The preceding profile audit found a 1.430511474609375e-6 m uncovered interval
between two hex chunks at Z=64. A new streaming regression reproduced it before
the implementation changed: negative-side coverage was zero at the recorded
interval, despite both chunks being resident and current.

The mesher constructed each corner in local float coordinates; streaming added
a separately rounded float chunk translation. Equal global integer corners
therefore reached different positions through different arithmetic paths.
Clipping an already mismatched shared edge cannot recover a canonical endpoint.

Local inspection confirmed the shared integer corner lattice and height samples.
The official [CGAL predicates/constructions explanation](https://doc.cgal.org/Manual/3.1/doc_html/cgal_manual/Kernel_d/Chapter_predicates_constructions_d.html)
notes that constructing new coordinates can introduce rounding error. The fix
uses a common construction path, not tolerance welding or an exact-real claim.
No external dependency was added.

## Changes

- `HexMeshingOptions::worldSpace` constructs positions directly from global
  integer corner coordinates through `hexLatticePosition`. Intermediate double
  arithmetic is rounded to the float mesh format in a single shared frame.
  Shared corners therefore no longer depend on their source chunk's transform.
- Streaming enables this only for Hex in opt-in `regionalSurfaceOwnership`.
  Regional hex `meshOrigin` is zero; renderer and published triangle queries
  consume the same world-space geometry. Other policies and legacy local hex
  output remain unchanged. Mesh transforms must not be treated as geographic
  chunk-center queries.
- Regional hex admission bounds use the same global lattice construction as
  the vertices. CPU clipping and optional prism caps operate in this shared
  frame, so common input edges take identical clipping arithmetic paths.
- Nonfinite/overflowing coordinates, collapsed projected top triangles and
  unusable top normals return an error rather than publishing invalid geometry.
  This float representation is NOT an unlimited-world precision solution.
  Very distant regional hex geometry can be rejected; camera-relative rebasing
  or a higher-precision representation remains separate work.
- No epsilon, skirt-height, query tolerance, material, save format or region
  revision rule was changed. Existing edit replay and stale-publication paths
  are retained. This does not connect unequal height/hex top surfaces.

## Verification

- Three new cases / 106,248 assertions cover the formerly failing boundary,
  exact emitted shared corners in all six neighbor directions, signed chunks,
  scales 0.125/3.3/8, finite normals and explicit precision/overflow failures.
- The existing one/four-worker regional suite still passes: caps off/on,
  mutations, eviction/return, edit replay and full fresh-mesh equality.
- Native east and north profile gates now require exactly 64 m of uniquely
  covered hex top, zero missing length and zero overlap, without tolerances.
  North subdivision count drops from 74 to 73 by removing the missing interval.
  Both policies' 306 source-mesh ray checks still pass. Height separations remain
  approximately 24.90 m and 26.15 m; those transition walls are still open.
- GCC Release: 9/9 gates, 512 core cases / 465,975 assertions.
- MSVC AddressSanitizer: 6/6 gates, 462 core cases / 464,632 assertions;
  the existing 17 driver exclusions remain unchanged. No race-freedom claim.
- WebGPU rebuild and browser smoke: 1/1. The shared mesher compiles, but the
  browser demo retains the default local mode; this is not acceptance of
  regional streaming on WebGPU.
- All 342 prior BMPs are byte-identical: 148 policy, 38 height, 156 static.
  Baselines: `1beb13c4798f`, `c2f0b2d2e3d3`, `60406e577c0c` respectively.
  Policy captures and profile CSVs/SVGs also repeat exactly in two processes.
- Visually reviewed the final corner capture and the updated chart, which now
  labels missing/overlap as zero for both boundaries. Chart screenshot:
  `output/playwright/canonical-hex-profiles.png`.
- Final galleries under `build/release/`: policy
  `streaming-policy-captures/4ac73583afd4/index.html`, height
  `streaming-demo-captures/e0f6f3301fa9/index.html`, static
  `static-demo-captures/98973945e830/index.html`.
- Assets untouched; TEMP/TMP stayed on D:. No new Linux, Apple hardware,
  physical Metal, TSan, moving-camera seam or hard-budget acceptance.

## Next

M16 remains partial. Add dependency-aware two-policy snapshots and invalidation
before constructing uniquely owned height/hex transition walls. Handle source
edits, unloads, region revisions, missing profiles and stale completions. Define
replacement of old coplanar walls, profile crossings and solid-query ownership;
do not infer volume closure from this local shared-endpoint fix.
