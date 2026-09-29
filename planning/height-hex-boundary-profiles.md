# Height/Hex Boundary Profiles (2026-09-06)

Historical audit: the subsequent [canonical endpoint fix](canonical-hex-endpoints.md)
resolves the measured microscopic interval for regional hex meshes. Both
audited boundaries now have zero missing/overlapping length. The measurements
below retain the pre-fix evidence; cross-policy transition walls remain open.

## Findings

The current worker snapshots and neighbor invalidation are same-policy only.
Connecting a height edge to a hex edge needs both actual piecewise-triangle
profiles and coordinated publication dependencies. A taller skirt or sampling
the bilinear height API would not establish either contract.

This increment implements the reference profile extraction/comparison and adds
measured demo evidence. It does NOT generate transition triangles or change
streaming, rendering, raw cell queries or default settings. **M16 is partial.**

In the existing capped regional fixture, the east boundary has 80 profile spans
over 64 meters, with no missing/overlapping interiors and a maximum top-height
separation of 24.897745153958418 meters. The north boundary has 74 spans, no
overlapping interiors, and a maximum paired separation of 26.15319250933744 meters.

The north boundary also exposes a missing hex interval at Z=64:
X=27.71281099319458 to 27.712812423706055, length 1.430511474609375e-6 meters.
Its adjacent hex sources are chunks (-1,0,1), triangle 54, and (0,0,1), triangle
208. Height chunk (0,0,1), triangle 55 covers that interval. This is distinct
from the previously repaired large lattice-packing holes. Mesh corner positions
and chunk translations are separately float-valued; their shared-endpoint
construction needs investigation before claiming numerical closure. No epsilon
weld or unverified root-cause repair was applied here.

## Research And Plan

Inspected `terrain_meshing.cpp`, `terrain_regions.cpp`, published mesh/ray
queries, immutable worker inputs, neighbor invalidation and publication gates.
The official [CGAL mesh-processing manual](https://doc.cgal.org/latest/Polygon_mesh_processing/index.html)
distinguishes combinatorial mesh structure from geometric defects and requires
closed meshes for its inside/outside predicate. This supports separating a
top-profile diagnostic from any solid-volume or watertightness claim. No CGAL
dependency or code was added.

Next steps are to investigate the measured shared endpoint mismatch, establish
two-policy snapshot dependencies, then build uniquely owned transition geometry.
Source edits, region changes, unloads and stale completions must invalidate it.
Existing hex cut walls/height skirts need explicit replacement rules to avoid
coplanar duplicate walls. Profile crossings, material ownership, missing inputs
and solid-query semantics must be handled rather than guessed.

## Changes

- `compareTerrainBoundaryTops` takes immutable mesh views for the negative and
  positive sides of an X/Z plane, a finite tangent interval and an explicit
  combined segment limit (default/max 65,536). Invalid data or limit exhaustion
  returns an error, not partial geometry.
- Geometric upward winding selects tops, independent of smoothed normals.
  Walls, undersides, point-only contacts and triangles with no area on the
  requested side are excluded. Canonically ordered edge intersections use
  double arithmetic; this is not exact-real computation.
- An ordered endpoint sweep preserves the union of both subdivisions. Each
  open span reports coverage counts and, only for unique coverage, endpoint
  heights with source/triangle indices. Gaps and overlapping layers remain
  explicit, including identical duplicate triangles and subpixel gaps. It does
  not extrapolate, choose a topmost layer, weld endpoints or smooth discontinuities.
- Scalar output owns its data; callers must keep inputs immutable during the
  call. This reference utility has no automatic version checks, cache, worker
  dependencies, transition ownership or acceleration structure.
- The native demo extracts both profiles from settled published meshes, checking
  current mesh and region versions before reading them without intervening
  updates. CSVs record breakpoints and triangle keys; a separate local source
  file records mesh-version/region stamps. Those local scheduling-dependent
  version numbers are intentionally not cross-process equality evidence.
- SVG charts show both profiles and measured missing/overlap lengths. Their
  rounded display coordinates do not replace the 17-digit CSV data. The policy
  gate compares profile CSVs, summaries and SVGs exactly across two processes.

## Verification

- Five new cases / 170 assertions: signed translation, mismatched subdivisions,
  both axes, interior/edge cuts, geometric winding, ray agreement, nonplanar
  triangle versus bilinear height, empty sides, duplicate layers, one-float-step
  gaps, reordered sources, invalid inputs and explicit segment limits.
- Native fixture: 306 source-mesh ray checks agree within 0.0001 m. Only spans
  at least 0.001 m wide are probed with float ray origins; smaller intervals
  remain explicitly recorded, not treated as ray-verified or filled.
- GCC Release: 9/9 gates; 509 core cases / 359,727 assertions.
- MSVC AddressSanitizer: 6/6 gates; 459 cases / 358,384 assertions. The existing
  17 driver exclusions remain unchanged; no race-freedom claim.
- WebGPU browser smoke: 1/1; its build was up to date. The new native boundary
  module is not included in that browser app, so this is regression coverage
  only, not boundary-profile acceptance on WebGPU.
- All 342 previous BMPs are byte-identical: 148 policy, 38 height, 156 static.
  Baselines: `2eb0a75b8427`, `78267ad21a26`, `54bd4d5ffbd7` respectively.
- SVG chart rendered in Edge and visually inspected; screenshot at
  `output/playwright/height-hex-profiles.png`. The tiny missing interval is
  subpixel in the overview but its measured length is explicitly labeled.
- Full-suite galleries under `build/release/`: policy
  `streaming-policy-captures/695858c2701f/index.html`, height
  `streaming-demo-captures/c2f0b2d2e3d3/index.html`, static
  `static-demo-captures/60406e577c0c/index.html`.
- Final policy rerun after clarifying the gallery's lattice-versus-numerical
  closure wording: `streaming-policy-captures/1beb13c4798f/index.html`.
  All 148 BMPs and six profile CSV/summary/SVG files match the full-suite run,
  providing equality across four independent native processes.
- Assets untouched. Build/test TEMP/TMP remains on D:. No new Linux, physical
  Metal, Apple hardware, TSan, moving-camera seam or hard-budget acceptance.
