# Coplanar Boundary Face Trimming (2026-09-07)

Follow-up: [top-edge stitching](boundary-top-stitching.md) now reconciles canonical
wall subdivisions with existing source edges on private copies. This report
preserves the earlier trimming scope; coordinated world publication remains open.

## Findings And Plan

The wall builder produces connecting triangles, but publishing them over existing
height skirts/hex caps risks coplanar overdraw. Publication is currently per chunk;
there is not yet an atomic replacement for a set of source meshes and their wall.
This increment implements the source-face subtraction prerequisite on private
copies. **M16 remains partial.** No published terrain or renderer is changed.

Inspected source publication/versioning, demo upload synchronization, region
clipping, attribute interpolation and triangle provenance. The trim reuses the
existing canonical edge clipping and barycentric attribute helpers in
`terrain_regions.cpp`, rather than introducing a different interpolation policy.
Khronos specifies edge clipping and interpolated attributes as separate parts of
polygon processing; this is CPU terrain interpolation, not a replacement for
GPU perspective-correct interpolation.
[OpenGL specification](https://registry.khronos.org/OpenGL/specs/gl/glspec14.pdf).
Narrowing needs explicit range checks, consistent with
[Microsoft standard conversions](https://learn.microsoft.com/en-us/cpp/cpp/standard-conversions?view=msvc-170).

## Changes And Contract

- `trimTerrainBoundaryFaces` removes coplanar positive-area geometry inside one
  X/Z plane's open tangent interval. The strip covers **all heights and both
  windings**. The caller must establish that these faces are replaceable skirts
  or caps; this API does not classify arbitrary walls or infer solid semantics.
- The strip complement is two disjoint closed half-planes. Partly covered faces
  are clipped into those outside portions, preserving winding. Faces on other
  planes and noncoplanar faces, including top triangles, remain untouched.
- Original vertices are retained, including now-unreferenced vertices. Unchanged
  triangles keep original indices and vertex values. Cut vertices interpolate
  continuous attributes; categorical values use the existing dominant-corner
  rule. Material weights sum to 255, and rebuilt surface ranges retain source
  surface materials. No compacting or unrelated remeshing occurs.
- Every output triangle records its original triangle id. This permits callers
  to resolve query/material provenance after removed faces shift index offsets.
  No-op results retain the original mesh layout and surface table exactly.
- Plane and interval endpoints must be exactly representable in the mesh-local
  float frame after translation. Invalid geometry/attributes/surface ranges,
  lost cut precision, collapsed or reversed retained area and exhausted output
  limits return explicit errors, never partially usable geometry. Mathematical
  zero-area clip contacts are omitted; preexisting degenerate faces are retained.
- Output triangle limit is 1..131,072. Original vertex copies and allocation
  overhead are not bounded by that count; no total-memory, hard-deadline or
  allocation-failure recovery acceptance is claimed.
- Native audit trims copies of the settled capped height/hex sources. It checks
  that no positive-area coplanar face remains inside the interval and that top
  subdivisions, endpoint heights, coverage and mapped provenance remain exact.
  The world retains its original meshes and old cut-wall probe behavior.
- `height_hex_trim.csv` records affected counts and removed plane area per axis,
  side and source. CTest compares it across two independent native processes.
  Existing screenshot and profile thresholds are unchanged.

## Verification

- Three new cases / 568 assertions cover both axes, signed translations, partial
  strips, all-height/full removal, both windings, original index preservation,
  attributes, surface materials, provenance, analytic areas and outside/inside
  rays. Invalid data, nonrepresentable cuts, exact output limits and a retained
  sliver that collapses in float are explicit failure cases.
- East audit affects 34 hex cap triangles and 128 height skirt triangles; north
  affects 20 hex cap triangles and 128 height skirt triangles. Height-side removed
  area is exactly 256 m2 per boundary; hex-side removed areas are approximately
  447.10 and 446.09 m2. There are 310 affected source triangles in total.
- GCC Release: 9/9 gates; 525 core cases / 468,479 assertions.
- MSVC AddressSanitizer: final 6/6 gates; 475 core cases / 467,136 assertions,
  including the added sliver check. Existing 17 driver exclusions are unchanged.
- WebGPU: 1/1 browser smoke, build up to date. This native module is not part of
  the browser app; this is regression coverage, not feature acceptance.
- All 342 previous BMPs are byte-identical: 148 policy, 38 height and 156 static.
  All six prior profile/wall CSV and SVG artifacts are also byte-identical.
  No new in-world visual closure is claimed; the new evidence is numerical.
- Baselines: policy `8614ed6932a2`, height `9ff886a0e161`, static `3111a4e7dfaf`.
  Final galleries under `build/release/`: policy
  `streaming-policy-captures/6713d6106144/index.html`, height
  `streaming-demo-captures/108c7ef305b7/index.html`, static
  `static-demo-captures/05867711105e/index.html`.
- Assets untouched; build/test TEMP/TMP stayed on D:. No new Linux/Clang parity,
  Apple hardware, TSan, global budget or moving-camera seam acceptance.

## Next

Reconcile source top-edge subdivisions and crossings, then package trimmed
source meshes and uniquely owned walls into a coordinated, freshness-checked
replacement for rendering and shared triangle queries. Handle multiple boundary
requests without overwriting each other's source edits. The existing profile
job only schedules profiles, and the trim operates on explicit caller-owned
meshes; neither automatically discovers boundaries, proves solid-column
semantics, stitches T-junctions or publishes geometry. These remain acceptance
requirements, not completed plan items.
