# Personal Project Check: 2026-09-05

## Fix Follow-Up

All five concrete defects below are now fixed in the working tree. The original
failure descriptions and line numbers are retained as an audit checkpoint, not
as a description of the current implementation.

- Mesh/texture creation rejects full pools before GPU allocation. Material
  creation still deduplicates at capacity, otherwise returns an invalid handle;
  glTF material import returns `CapacityExceeded`. Limits remain 64 meshes,
  64 distinct materials and 128 textures; they are safe limits, not removed limits.
- A canonical `RasterState::frontClockwise` field is mapped by OpenGL, Vulkan,
  D3D12, WebGPU and Metal. Material variants and batch keys include reflection
  orientation. This preserves front-facing shader semantics as well as culling.
  Shadows already use no culling and have mixed-reflection runtime coverage.
- Public single-pass recording resets its own error; graph main recording does
  not erase a preceding shadow error. A one-shot NullDevice pipeline failure
  proves error propagation and successful recovery on the following frame.
- Gizmo Y faces wind outward. Scene initialization checks all 108 triangles'
  outward cross products, and the LH toggle uses the reflected pipeline.
- Large streaming plans execute on a worker using an independent RegionMap
  snapshot. Candidate storage is bounded by resident capacity per policy, plans
  are cached at camera chunk boundaries, and region mutations/assignment or
  chunk movement invalidate them. Obsolete work and shutdown are cancellable.
  Old residency is retained while planning; new loads/unloads wait for the result.
  Per-frame candidate priorities still track the exact camera position.

Positive streaming budgets now allow at least one operation per enabled stage
before deferring further work, avoiding starvation with a tiny budget. A zero
budget remains paused. This is explicitly cooperative: an individual operation
or region snapshot can exceed the budget. M16 remains partial until integrated
worst-case profiling supports a stronger time-budget acceptance claim.

Fix verification:

- Release: 7/7 CTest targets; 456 core cases / 105,666 assertions.
- MSVC AddressSanitizer: 6/6 targets; 407 core cases / 104,330 assertions.
  Its existing driver-compatibility filter skips 17 Vulkan/DX12 cases; native
  GPU execution is covered separately by the Release run.
- WebGPU: rebuilt and passed browser CTest, including mixed mirrored-instance
  coverage, two distinct raster batches, and byte-exact restoration of the
  original renderer reference image. Existing reference assets were unchanged.
- Native OpenGL/Vulkan/D3D12: mixed-reflection PBR and shadow coverage/parity.
  Ordinary PBR uses the existing two-code-value tolerance; shadows retain
  their existing Vulkan edge/PCF tolerance, with zero native validation warnings/errors.
- Streaming: radius 128 converges at resident capacity under a 1 microsecond
  cooperative budget; region mutation/replacement, camera movement, cancellation
  of UINT32_MAX-radius work and shutdown are regression-tested. No wall-clock
  threshold is used as a portable correctness test.
- Static: all 156 captures repeated exactly. Of the 26 fixture images, only
  the gizmo changed versus the previous audit; its repaired silhouettes were
  personally inspected and are closed. All other fixtures remained byte-identical.
- Linux/macOS builds and Metal hardware were not rerun on this Windows PC.
  Metal's winding mapping is implemented but not claimed runtime-accepted.

The winding implementation follows the
[glTF transform rule](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html),
[OpenGL front-face definition](https://wikis.khronos.org/opengl/GLAPI/glFrontFace),
[Vulkan front-face state](https://docs.vulkan.org/refpages/latest/refpages/source/VkFrontFace.html),
[D3D12 rasterizer state](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_rasterizer_desc)
and [WebGPU primitive state](https://www.w3.org/TR/webgpu/#dictdef-gpuprimitivestate).

## Original Findings

These were the findings before the user requested fixes; see the follow-up above
for current status. No historical failure measurements have been replaced.

### P1: Renderer resource exhaustion has no safe failure path

`src/engine/rendering/renderer.h:145` fixes mesh capacity at 64 and texture
capacity at 128; `src/engine/rendering/material.h:98` fixes material capacity
at 64. Mesh/texture creation calls `acquire()` after GPU allocation without an
available-slot check (`renderer.cpp:481`, `renderer.cpp:538`); material creation
does the same (`material.cpp:330`). `GenerationalArena::acquire()` asserts and
then reads the free-list tail (`src/core/ids/generational_arena.h:51`). Assertions
are disabled under NDEBUG by default. Exhaustion therefore aborts debug builds
and reaches invalid empty-list access in release rather than returning failure.

This is source-confirmed, not a deliberately executed release crash. The
existing more-than-64-draw tests reuse resources, so they do not cover 65 distinct
meshes/materials or 129 textures. Add explicit capacity/error handling or safe
growth, with allocation rollback and boundary tests. Do not merely raise limits.

### P2: Negative scale can remove single-sided geometry

The main material pipeline uses back-face culling (`material.cpp:398`), but
pipeline selection and the batch key do not account for the sign of the model
determinant. `renderer.cpp:663` uses the determinant only for the normal matrix.
The [glTF specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)
requires triangle winding to follow the sign of the global transform determinant.

An offscreen Vulkan probe using the actual Renderer and an unlit triangle
produced these results on this PC:

| X scale | Visible candidates | Draws | Lit pixels |
| --- | --- | --- | --- |
| +1 | 1 | 1 | 512 |
| -1 | 1 | 1 | 0 |

Both renders returned success. Probe source is retained locally at
`build/audit-20260905/mirror_probe.cpp` (ignored, not a committed regression test).
Fix orientation-aware pipeline selection and batching together, including the
shadow path. Add mixed mirrored/non-mirrored instances to native parity tests.
The ordinary cube fixture has positive scale and cannot cover this failure.

### P2: M16 planning can consume the budget and starve streaming

`src/engine/world/world_streaming.cpp:590` builds and sorts the entire desired
ring, constructs its distance map and sorts unload candidates before the first
budget check. The checks at lines 612/626/653/683 all measure from before that
planning work. Repeating an expensive plan can therefore prevent any chunk
loading/publication, even with a stationary camera.

A release probe ran ten updates with one worker, capacity 256 per policy and
the default 2,000 microsecond budget:

| Horizontal radius | Total chunks loaded | Maximum update (microseconds) |
| --- | --- | --- |
| 2 | 25 | 65 |
| 32 | 80 | 2,184 |
| 128 | 0 | 16,590 |

These are observations on this PC, not portable timing thresholds. Source is
retained at `build/audit-20260905/streaming_probe.cpp`. Radius 128 is accepted by
the configuration, but the oversized desired set never starts filling even the
available capacity. M16 is reopened as partial: incremental/cached bounded
planning and forward-progress coverage are needed before budget acceptance.
Existing scheduling, persistence and deterministic-generation work remains valid.

### P2: Main-pass recording clears preceding shadow errors

`Renderer::recordShadow()` stores pipeline/allocation failures at
`src/engine/rendering/renderer.cpp:765` and line 782. `render()` then invokes the
main pass, whose `record()` resets `recordError_` at line 579. If main recording
and submission succeed, `render()` can report success despite an incomplete
shadow pass. This is source-confirmed; no failing-device injection was run.
Preserve frame errors across both passes and add an injected-failure regression.

### P2: Gizmo fixture has reversed top/bottom face winding

`src/demos/demo_gizmo.cpp:39` and line 42 construct the -Y and +Y cube faces
with inward-facing triangle cross products while the pipeline culls back faces.
The static close-up shows broken-looking axis-end silhouettes. The new camera
makes the asymmetric F legible but does not repair this geometry. Correct these
four triangles and add winding coverage; the RH screenshot alone also cannot
certify the interactive LH reflection toggle.

## What We Have

This is a multi-backend C++ rendering and procedural-world foundation, not yet
an integrated game-engine application. It has math, containers/handles, ECS,
hierarchies, scoped asset loaders, resource ownership, an RHI, graph execution,
forward PBR rendering, deterministic noise, voxel/height/hex data and meshers,
and asynchronous world generation/streaming.

OpenGL, Vulkan, D3D12 and browser WebGPU have implemented native paths and
automated acceptance coverage. Metal implementation and historical Apple build
coverage exist, but physical Metal runtime acceptance remains unavailable on
this Windows-only setup. The latest local work has not run on remote Linux/macOS
CI; earlier CI results must not be treated as validation of these new changes.

Important boundaries:

- The hybrid demo has deferred opaque plus forward transparent composition.
  Production `Renderer::render()` remains directional-shadow plus forward PBR.
- Point lights are stored but not consumed by the current forward lighting path.
- The terrain gallery still displays fixed mesher output. A separate integrated
  height-world demo now covers camera-driven CPU/GPU residency, editing and a
  fixed-fixture disk save; it is not a general multi-policy saved-world application.
- WorldStreamer keeps edit overlays across unload in memory. The new demo adds
  explicit disk/process reload for its fixed seeded height fixture, not a general
  WorldStreamer save API.
- M12 has culling, batching, priority jobs and serial graph scheduling, not
  occlusion, multithreaded recording, SIMD or async compute acceptance.
- Asset codec support is explicitly scoped; it does not imply full animation,
  skinning or arbitrary DCC-scene support. Editor, audio, networking and gameplay
  systems are not established by the rendering milestones.

The status table now has 17 complete milestones and three partial milestones:
M12, M16 and M17. Completed delivery scopes are not a claim of zero remaining
bugs; the concrete renderer defects above were repaired in the follow-up.

## Changes And Visual Review

See [Static Screenshot Checks](static-demo-checks.md) for commands and criteria.
Static mode freezes simulation after fixed-step warmup, ignores live input,
uses inspection cameras and replaces timing telemetry with fixed captions.
Cube overview plus six axial views check every face. Cornell is fully framed;
PBR has 30 separated spheres with constant base hue; terrain, particles and
the handedness marker now have inspection-specific framing.

All 20 overview fixtures and six cube views were personally inspected during
this audit, including the changed views after recapture. The positive-scale
cube showed full faces; the PBR parameter ordering and the intentional bad
controls in moire, z-fighting, alpha order and normal orientation are visible.
The gizmo issue was visible before the follow-up repair. Particles remain faint, and overview
terrain captures cannot establish every hidden face or seam. Captions overlap
the top row of the full-screen instancing scenes; inspect full-size captures,
and keep numerical instance/culling tests authoritative for coverage there.

## Original Audit Verification

- Release build and all seven CTest targets passed after the final fixture edits.
  The core executable covers 451 cases / 105,227 assertions.
- The new gate produced 156 BMPs: 26 fixtures, three frames, two independent
  launches per fixture. SHA-256 matched within fixtures and across launches;
  invalid CLI combinations returned the expected failure code.
- An animated OpenGL smoke run completed all 20 scenes / 60 captures earlier
  in the audit. It is separate from the fixed-pose repeatability gate.
- WebGPU Debug build and browser CTest passed during this audit. Later static
  camera/caption edits are not used by its existing reference runner.
- MSVC/ASan and remote Linux/macOS gates were not rerun for the final static
  fixture changes. Metal runtime remains unverified.
- Static equality proves repeatability on this build/device, not animation
  stability, physically correct shading or cross-device byte equality.
- Existing render-graph edits were preserved. The user asset directory was
  not modified. No commit, push or external publishing was performed.

## Streaming Profile Follow-Up

The [CPU profile and measurements](streaming-profile-2026-09-05.md) now cover
cold, settled and moved voxel/height/hex residency plus 50,000 regional overrides.
Repeated planning snapshots share map storage, meshing uses bounded neighbor
lookups, and unload copies only deltas. Snapshot mutation still clones a shared
map; measured first edits took 16-24 ms. This is useful CPU profiling evidence,
not an integrated application or a hard-deadline acceptance result. M16 stays
partial, and the earlier test counts above describe their own checkpoint.

## Next Work

1. The [integrated height demo](streaming-world-demo.md) now supplies frame
   profiles, GPU residency and separate-process save acceptance. Address cold
   pipeline work, combined CPU/upload budgets and expensive synchronous actions
   before restoring full M16 budget acceptance.
2. Extend the demo beyond its fixed height-world fixture to voxel/hex and live
   region stress. Continue M12 optimizations only with measurements.
3. Keep M17 hardware acceptance explicitly open until an actual Mac is available.
