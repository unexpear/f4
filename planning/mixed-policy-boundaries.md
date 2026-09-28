# Mixed-Policy Boundary Diagnostic (2026-09-06)

Historical diagnostic increment. The subsequent [world-space footprint work](world-space-streaming.md)
adds opt-in meter-based center admission, fixed diagnostic policy materials, and
112 captures. It supersedes the first next-work item below, not the seam findings.

## Findings

This follows the [tiled hex fix](hex-streaming-layout.md). It is a diagnostic
and lifecycle-test increment, not a cross-policy seam implementation. M16
remains partial. Engine generation, ownership, meshing, and collision behavior
were not changed in this increment.

The new fixture uses seed `kStreamingDemoSeed`, 64 m regions, height as the
default policy, voxel in region (-1,0), and tiled hex in (0,0). Horizontal chunk
radius is one, voxel vertical radius zero, hex cell radius two, and hex world
scale eight. At the zero anchor, seven height, two voxel, and two hex chunks
are resident together. Eight slots per policy bound all transition states to
24 CPU residents, below the diagnostic view's existing 25-resident limit.
This is a bounded fixture, not removal of that adapter limit.

Visual inspection of front, reverse, overhead, and close-boundary captures finds:

- Real openings and vertical discontinuities between the green height surface,
  white voxel surface, and sand-colored hex surface. Sand is an opaque hex
  material edit for identification, not a new terrain generation recipe.
- Whole-chunk regional selection does not enforce geometric ownership.
  `mixed_boundary_audit.csv` finds 83 of 114 upward-facing triangle centroids
  from hex chunk (0,0,0), and 32 of 114 from (0,0,1), in another policy's region.
  These 115 encroaching centroids reproduce in both processes. The audit samples
  centroids with mean vertex-normal Y > 0.5; it is not a complete area, hole,
  triangle-intersection, or collision-coverage test.
- The large opening on the voxel side also includes residency cutoff, not
  solely a seam defect. A one-chunk horizontal radius spans different world
  distances for each policy. Only voxel X=-1 is selected at this anchor, although
  the 64 m voxel region also includes X=-2. Do not interpret the finite outer
  footprint as a missing-face bug or mistake this fixture for equal view ranges.

Code inspection agrees: `WorldStreamer` selects each candidate's policy from
its geometric center. Height skirts and voxel/hex neighbor snapshots only
resolve same-policy adjacency. No cross-policy clipping/transition mesh or
matching collision-ownership contract exists. Taller skirts could hide some
openings but would not resolve encroachment or physical surface ownership.

## Changes And Gates

- Shared mixed fixture configuration and phase changes live in the streaming
  demo state module. The existing height-only disk-save format is unchanged.
- Native `--verify-policies` adds four views and a restored view. It performs
  90 region/camera changes with no settle or worker wait between revisions,
  then returns to the original map/anchor. Hex material edits, all 11 draws,
  CPU mesh bytes and the image must survive. GPU teardown must recover the
  initial resource counts/bytes; existing shader/API/pipeline guards remain.
- The policy gate now checks 25 checkpoints twice in each of two processes:
  100 BMPs. Images match within/across processes; CPU mesh digests match across
  processes and on mixed-home/restored checkpoints, including occluded geometry.
  Mesh CSVs now identify policy and all X/Y/Z coordinates, not only X/Z.
  The centroid-audit CSV must also match across processes. These are
  repeatability gates, not acceptance of the visible boundary defects.
- One portable case tests four configurations: radii one/five and one/four
  workers. Radius five invokes worker planning. It edits one resident chunk
  of each policy, guarantees actual eviction, bursts 90 map/camera mutations,
  and compares final resident keys plus every packed vertex/index byte against
  a separately constructed, equivalently edited reference world. Explicit
  delta checks guard edit persistence. Null-backend view uploads/retirement,
  frame admission counts, resident bounds and worker failures are checked.
  It does not require a timing-dependent stale-completion count; some obsolete
  work may finish before cancellation. It contributes 1,836 assertions.

## Verification

- Windows GCC Release: 9/9 CTest groups; 485 core cases / 333,340 assertions.
  The final native marker-only edit also passed the policy gate again.
- MSVC AddressSanitizer: 6/6 groups; 435 cases / 331,997 assertions. Existing
  17 driver exclusions remain unchanged. ASan does not establish race freedom.
- The mixed-policy case passed in ten additional separate Release processes.
- WebGPU browser smoke: 1/1. The build was already up to date; this native
  diagnostic/test increment is not part of the browser WorldStreamer path.
- All 156 static BMPs match the preceding `e0f59a1cc54d` gallery exactly.
- All five height checkpoints match the preceding `26b0f8b5b164` gallery.
- Final policy gallery:
  `build/release/streaming-policy-captures/4609096be3d6/index.html`.
  Front/reverse/overhead/boundary images were visually inspected; repeated and
  restored images are exact-byte checked by the gate. All 100 final BMPs match
  the visually reviewed `9d3223ffa4c9` gallery; the final rerun clarifies HTML wording.
- The final first-process burst retired 11 GPU meshes and uploaded zero during
  its 90 mutation frames; the settled return rebuilt the visible world. This
  deliberately outruns generation, exercising cancellation/recovery, not smooth
  continuous coverage while policy/camera changes arrive every frame. Do not
  infer a no-pop-in or no-transient-empty-frame guarantee from settled equality.
- Existing height gallery: `build/release/streaming-demo-captures/bf68ee7bf869/index.html`.
  Static gallery: `build/release/static-demo-captures/53dee7b959b9/index.html`.
- Builds/tests use D: TEMP/TMP. User assets are untouched. No new Linux, macOS,
  Metal hardware, or TSan acceptance is claimed.

## Next Work

1. Define a shared world-space residency footprint, with explicit compatibility
   for existing chunk-count callers. Test signed boundaries and non-unit hex
   scale before changing streaming admission behavior.
2. Define deterministic regional surface ownership and transition geometry
   across height/voxel/hex. Rendering, edit replay, and collision must agree;
   a render-only clip is insufficient. Add geometric coverage/overlap checks,
   cross-boundary samples/rays, and near-boundary camera motion before admitting
   watertight seams. Current centroid counts are observations, not golden defects
   that future fixes must preserve.
3. Retain the new mutation/reference gates while profiling larger shared maps
   and non-preemptible work. The cooperative budget is still not a hard deadline.

## Research

The inspection uses the project's region/mesh implementations and the original
[hex coordinate reference](https://www.redblobgames.com/grids/hexagons/#map-storage).
The test harness follows [doctest logging](https://github.com/doctest/doctest/blob/master/doc/markdown/logging.md)
and the gate uses documented [CMake file hashing](https://cmake.org/cmake/help/latest/command/file.html#hash)
and [process execution](https://cmake.org/cmake/help/latest/command/execute_process.html).
