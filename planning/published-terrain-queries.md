# Published Terrain Surface Queries

Historical query-foundation increment. The subsequent
[regional surface mode](regional-surfaces.md) adds optional CPU clipping and
region-revision provenance; defaults retain the behavior described here.

## Scope And Findings

M16 remains partial. This increment adds the common surface-query foundation
needed to test future regional transition geometry. It does not clip regions,
fill gaps, remove encroachment, or replace existing collision APIs.

The existing voxel query walks solid cells; height sampling interpolates four
samples bilinearly. Neither describes every published mesh surface, and hex had
no surface ray query. A nonplanar height regression demonstrates the distinction:
at local (10.25, 10.75), one raised corner gives triangle height 1 but bilinear
height 0.75. Both APIs keep their explicit, different meanings.

## Implemented Contract

- `raycastTerrainMesh` returns the nearest triangle in the closed world-distance
  interval [0, maxDistance], regardless of finite direction magnitude. Results
  include position, geometric winding normal, barycentrics, triangle index and
  front-face status. Backfaces are included unless explicitly culled.
- Invalid nonfinite inputs, zero direction, negative range, malformed indices
  or nonfinite vertex positions return no hit. Degenerate and coplanar triangles
  do not hit. Exact distance ties select the lowest triangle index.
- `WorldStreamer::raycastPublishedTerrain` queries published CPU meshes across
  height, voxel and hex using their mesh translations. Results include chunk
  key, generational handle and published mesh version. Exact cross-chunk ties
  use policy/X/Y/Z key order rather than worker or hash-map iteration order.
- Successful publication records the source version. While an edit remeshes,
  the retained old mesh and its old version remain queryable. Evicted meshes
  disappear. The current cell version is deliberately not substituted for the
  version of the triangles being tested.
- This is a main-thread reference scan without allocation, acceleration or
  performance acceptance. It tests surfaces, not solid occupancy, materials or
  regional masks. GPU upload may lag CPU publication. It is not a promise of
  current-frame raster/collision synchronization or universal watertightness.

The implementation uses ray-aligned projection and double-precision edge
products, researched against the original
[PBRT triangle intersection discussion](https://pbr-book.org/4ed/Shapes/Triangle_Meshes).
Float coordinates are promoted before translation and normalization. This is
not an implementation of all PBRT numerical error bounds or robust predicates.

## Verification

- Seven new cases / 1,755 assertions cover principal and oblique rays, shared
  edges, winding/culling, closed ranges, signed translations, tiny/large scales,
  invalid data, nonplanar quads, all policies, one/four workers, edit publication,
  eviction/reload and stable shared-corner ties.
- Windows GCC Release: 9/9 CTest gates, 495 cases / 338,022 assertions in core.
- MSVC AddressSanitizer: 6/6 gates, 445 cases / 336,679 assertions in core;
  the existing 17 driver exclusions are unchanged. This is not race coverage.
- WebGPU browser smoke: 1/1; its build was already up to date. The new native
  query implementation is not part of this browser gate's coverage.
- Native policy diagnostics add `meter_surface_probes.csv`; its geometry results
  must match byte-for-byte between two independent processes. Runtime checks
  validate settled chunk/mesh versions without encoding scheduling-dependent
  version counters in the CSV. The first three probes must hit height, voxel
  and hex respectively. Two boundary probes record observations, not ownership
  acceptance. In this fixture, the downward ray at XZ (8, 56) misses.
- The overhead view still shows the hex-boundary holes. All 112 policy BMPs
  match `6bcf0b48616a`; all 156 static BMPs match `bdf383e343cf`; all 38 height
  BMPs match `22e050a5e065`. Image equality is regression evidence, not proof
  that existing seams are correct.
- Current policy gallery: `build/release/streaming-policy-captures/bbc790f88c5d/index.html`.
  Static gallery: `build/release/static-demo-captures/d3c6a7113968/index.html`.
  Height gallery: `build/release/streaming-demo-captures/69d284e7306d/index.html`.
- Builds/tests use D: TEMP/TMP; assets are untouched. No new Linux, Apple,
  physical Metal or TSan acceptance is claimed.

## Next

Define regional ownership and transition geometry shared by rendering, edits
and these surface queries. Add coverage/overlap and moving-camera gates against
that contract. Do not use render-only clipping or taller skirts to hide the
unresolved ownership problem. Acceleration and synchronization require separate
measured work; this reference scan is not a new frame-budget claim.
