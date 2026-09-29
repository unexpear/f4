# Streaming Policy Audit (2026-09-06)

Historical first audit. Its hex packing/residency findings are resolved by the
[versioned tiled layout follow-up](hex-streaming-layout.md). The current policy
gate has 80 captures including camera-driven hex travel; the 68-capture results
below describe the earlier implementation and are retained as evidence.

## Findings And Fixes

The production demo adapter previously rejected voxel/hex policies and empty
meshes, positioned every chunk on a height grid, and selected only submesh zero.
It now translates each policy's local geometry via `WorldStreamer::meshOrigin`,
tracks empty meshes without GPU allocations, and draws the complete index range
with its single opaque vertex-colored material. Surface material IDs/textures
are not individually bound by this diagnostic adapter. Limits remain 25 logical
residents, 25 detached reload meshes and one replacement within 64 mesh slots.

The first native voxel run exposed missing outer walls and an unload/return
image mismatch despite equal settled CPU mesh hashes. `VoxelMeshingOptions`
specifies LOCAL border coordinates, but `WorldStreamer::submitMesh` interpreted
the quotient as a GLOBAL chunk coordinate. It fetched unrelated chunks around
the origin, suppressing exposed faces and leaving incorrect internal faces.
The fix offsets the quotient by the actual center chunk on all three axes,
using the existing checked offset helper (overflow means no neighbor).

An independent CPU oracle compares all 27 meshes in each of three neighborhoods
at origin, positive and negative X/Y/Z against a bounding-box border fetch. It
checks mesh geometry/index digests and per-vertex AO colors. Adapter regressions
cover signed/scaled origins, empty -> visible -> empty edits, all surface index
draws, three live policy cycles, prepared pipeline reuse and complete GPU teardown.

## Native Diagnostic Gate

Run from the repository using a new output directory with an existing parent:

```powershell
$env:TEMP = 'D:\dev\f4\build\tmp'
$env:TMP = $env:TEMP
.\build\release\src\demos\f4_streaming_demo.exe --verify-policies --output build\policy-check-new
ctest --preset test-release -R streaming_policy_capture
```

The inspection camera and residency anchor are explicitly independent. This
is not evidence of correct camera-driven hex coverage. Four workers run without
`waitForIdle` in the native frame loop. The gate covers nine voxel chunks, nine
hex patches with edited material cells, unload/return, opposite voxel views,
hex undersides, nine empty air chunks, and three stationary
height -> voxel -> hex -> height RegionMap cycles. It verifies visible changes,
expected terrain draw counts, matching returned/restored images, shader/API
diagnostics, prepared pipeline counts and full GPU resource teardown.

Seventeen checkpoints repeat twice per run, then in two separate processes:
**68 BMPs** with exact within/across-process hashes. `--load` is rejected in
policy mode before initialization; the existing height save format is unchanged.
The original 38 height captures and 156 static captures remain separate gates.
Profile CSVs retain cooperative CPU/upload admission and rendering timings;
world replacement between diagnostic scenarios is setup, not timed live reload.

## Open Work

- Hex tiles contain a hex-shaped subset of a square axial allocation, yet chunk
  origins advance by the square width `2R+1`. This leaves holes between patches,
  visible above and below. Repacking or changing ownership requires an explicit
  generation/serialization compatibility decision and seam fixtures.
- The planner divides world X/Z by that same scalar width instead of applying
  the inverse axial transform and geometric `hexRadius`. Correct mesh placement
  alone does not correct residency, region lookup or nearest-priority distances.
- Mixed policies coexisting across regional boundaries, cancellation-heavy live
  mutations, large shared RegionMap copy-on-write edits, general multi-policy
  saves and hard frame-budget acceptance are not closed by this diagnostic.
- Metal still requires Apple hardware. No new Linux/macOS, Metal or TSan result
  is claimed. User assets are untouched.

## Verification

- GCC Release: **9/9** CTest gates, **479 cases / 112,002 assertions** in core.
  The two streaming capture gates passed again after the final empty-air caption
  and CLI rejection check. All five height checkpoints match the preceding
  `a0c48054b692` reviewed gallery byte-for-byte.
- MSVC AddressSanitizer: **6/6**, **429 cases / 110,659 assertions** in core.
  The existing 17 driver-compatibility exclusions remain; native driver tests
  ran in Release. No race-freedom claim follows from ASan.
- WebGPU browser smoke: **1/1**. Its build required no recompilation; it does
  not execute the native WorldStreamer or this OpenGL diagnostic.
- New focused coverage: **16 cases / 6,197 assertions**, comprising the 15
  portable demo cases and the independent signed voxel-neighborhood oracle.
- Visually inspected voxel views from both sides, hex top/underside material
  markers, empty-air retirement and fixed-camera region replacements. Voxel
  outer walls are restored; hex packing holes remain explicitly open. Repeated
  captures match exactly, not a guarantee against every moving-camera artifact.
- Final policy gallery: `build/release/streaming-policy-captures/614ad186ea3c/index.html`.
  Height gallery: `build/release/streaming-demo-captures/eff972c4dd68/index.html`.
  Static gallery: `build/release/static-demo-captures/d135e55eef64/index.html`.
- `git diff --check` passed. The initial build failed because inherited C: TEMP
  had no space; subsequent builds/tests explicitly used the existing D: temp
  directory. No system-wide environment setting or user asset was changed.

## Research

The voxel fix follows the explicit local-coordinate contract in
`terrain_meshing.h` and actual callback arguments in `meshVoxelChunk`. The
mesher's existing pointy-top axial coordinates agree with the original
[Red Blob Games hex-to-pixel derivation](https://www.redblobgames.com/grids/hexagons/#hex-to-pixel).
The new process gate follows the project's existing capture scripts and
[CMake execute_process documentation](https://cmake.org/cmake/help/latest/command/execute_process.html)
for result, diagnostics and bounded timeouts.
