# World-Space Streaming Footprints (2026-09-06)

## Findings And Contract

The [mixed-policy diagnostic](mixed-policy-boundaries.md) selected only two of
the four voxel chunks in a 64 m region. A one-chunk radius describes different
world distances for height, voxel and scaled hex geometry. This was an admission
range problem, separate from the still-open cross-policy boundary geometry.

`WorldStreamingConfig::horizontalHalfExtentMeters` is now an optional finite,
nonnegative float. When present, a chunk's geometric XZ center must lie inside
the closed camera-centered square `[camera.x-H,camera.x+H]` by
`[camera.z-H,camera.z+H]`. It overrides `horizontalRadius`, including when H=0.
An empty optional preserves legacy chunk-count behavior and existing goldens.
Invalid H throws `std::invalid_argument` before stores/workers are created.

This is deliberately a center-admission contract, not a mesh-bounds intersection
query, circular distance, regional surface mask, or universal gap-free coverage
guarantee. H=0 can select nothing when the camera is not at a chunk center.
Voxel vertical rings, center-based RegionMap lookup, nearest-per-policy capacity,
distance/key tie breaking, sparse edits and cooperative budgets are unchanged.

## Implementation

The planner derives its horizontal candidate bounds by inverting the existing
geometric-center lattice. This covers ordinary square chunks and both legacy
and tiled hex layouts without a second independent placement formula. Four
square corners bound the inverse parallelogram; a conservative one-cell margin
prevents inverse-rounding loss at an exact edge. A forward center test makes the
actual admission decision, so the margin cannot admit out-of-footprint centers.
Bounds are clipped to the representable chunk-coordinate domain before casts.

Planning's worker threshold uses those candidate bounds, not the overridden
chunk-count radius. Memory remains capacity-bounded through the nearest heap.
Meter plans invalidate on exact camera XYZ changes as well as region revisions:
even sub-chunk motion can move a footprint edge or change nearest-capacity
selection. Legacy plans retain their chunk-boundary cache behavior.

Large footprints still enumerate candidates. They are cancellable, not a bounded
time nearest-search algorithm. Continuous camera movement can supersede large
plans before publication; old residency remains while planning is pending.
This increment does not establish a hard frame budget, no-pop-in guarantee, or
successful continuous streaming under arbitrary velocity/footprint sizes.

## Visual Diagnostic And Ordering

The existing chunk-count mixed fixture is retained. A new fixed-anchor +/-96 m
comparison has sufficient capacity for 20 residents: 14 height, four voxel and
two hex. Front, overhead and close-boundary screenshots confirm that voxel
chunks (-2,0,0) and (-2,0,1) now fill the previously excluded half-region.
Cross-policy openings, height discontinuities and hex encroachment remain visible.

The first new capture run (`df026abdc2cd`) failed exact process comparison at
pixel (409,421) of `meters_home`: height green versus voxel white, despite identical
CPU mesh hashes. Opaque sorting used worker-dependent mesh handles when all
policies shared a material. This is consistent with an order-dependent boundary
depth tie, not failed generation. The strict comparison was not relaxed.

The diagnostic view now creates three visibly distinct material descriptors in
fixed policy order: cool voxel tint, unchanged height tint, warm hex tint.
Identical descriptors would be deduplicated by the material cache and would not
provide separate groups. All three terrain pipelines are prepared at startup;
preparation stops on failure and releases the temporary mesh. Material/pipeline
groups precede mesh identity in opaque sorting, stabilizing cross-policy ties.
This changes diagnostic tinting/order only, not the engine's region ownership,
mesh geometry, collision or depth comparison. It is NOT a watertight-seam fix.

## Tests And Evidence

- Three new engine cases compare meter admission/nearest capacity to exhaustive
  forward-coordinate enumeration for all policies, both hex layouts, scales
  0.5/3, signed cameras and sub-chunk motion. They cover closed edges, zero and
  invalid extents, very large coordinates, invalid cameras and cancellation of
  enormous bounded-memory plans. A mixed fixture checks all four voxel chunks
  and replacement by height when overrides are erased.
- The existing fresh-reference mutation case now covers eight configurations:
  one/four workers with chunk radii one/five and meter half-extents 96/160.
  Both modes guarantee initial eviction, burst 90 map/camera changes, and compare
  edited vertex/index bytes and sparse deltas after returning to a fresh reference.
  Per-policy capacity eight intentionally exercises truncation in these tests.
- Release full suite: 9/9 gates. Focused streaming/meter tests: 19 cases and
  10,033 assertions, including initialization failure cleanup and GPU retirement.
- MSVC AddressSanitizer: 6/6 gates, 438 cases / 334,924 assertions; the existing
  17 driver exclusions are unchanged. This is not a race-freedom result.
- WebGPU browser smoke: 1/1. Its build was already up to date; the native
  WorldStreamer implementation and policy-view adapter are not browser coverage.
- Native policy gate: 28 checkpoints repeated twice in two processes, 112 BMPs.
  Four successful gate runs produced matching images across all eight processes
  (448 BMPs total); CPU checkpoint digests also match within each process pair.
- Visually reviewed all three final meter views. Exact comparison retains the
  original mixed mutation/return, camera-driven hex route and height reload gates.
- All 156 static BMPs match `53dee7b959b9`; all five height checkpoints match
  `bf68ee7bf869`. Earlier voxel/hex diagnostic colors intentionally change with
  the policy tints; geometry/golden tests are preserved.
- Final policy gallery: `build/release/streaming-policy-captures/6bcf0b48616a/index.html`.
  Height gallery: `build/release/streaming-demo-captures/22e050a5e065/index.html`.
  Static gallery: `build/release/static-demo-captures/bdf383e343cf/index.html`.
- Builds/tests use D: TEMP/TMP. User assets are untouched. No new Linux, macOS,
  physical Metal or TSan acceptance is claimed.

## Next Work And Research

M16 remains partial. Next is a deterministic regional surface/transition contract
shared by rendering, edit replay and collision, followed by geometric coverage,
overlap, ray/sample and moving-camera checks. Fixing center admission does not
make whole hex chunks conform to rectangular region boundaries.

The subsequent [published-surface query increment](published-terrain-queries.md)
adds a versioned triangle probe across all policies and records the visible hex
gap as a ray miss. It leaves geometry and regional ownership unchanged; the
counts and gallery paths above remain this earlier increment's evidence.

The bounds use the inverse-coordinate approach in the original
[hex implementation guide](https://www.redblobgames.com/grids/hexagons/implementation.html).
Range checks follow the [C++ floating-to-integer contract](https://eel.is/c++draft/conv.fpint).
Boundary ordering analysis uses the existing render queue and the documented
[OpenGL depth comparison](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glDepthFunc.xhtml):
with LESS, an equal-depth fragment does not replace the stored one.
