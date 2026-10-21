# Integrated Streaming World Demo

## Delivered Scope

`f4_streaming_demo` connects the real WorldStreamer to the production ECS/PBR
renderer on native OpenGL. It is a fixed seeded height-world fixture: a radius-2
ring of 25 chunks, four workers, a cooperative 2 ms shared streaming/upload
budget, and at most two GPU mesh uploads per frame. It is separate from the existing
20-scene gallery. No user assets were modified or required.

In the default height fixture the camera drives actual residency, not a separately animated display. Chunk-local
vertices receive chunk-to-world transforms. The adapter uploads only published
`Meshed` versions, retains the previous GPU mesh during CPU remeshing, creates a
replacement before retiring the old mesh, and retires GPU meshes when CPU chunks
unload. At most 25 residents plus one replacement fit the renderer's existing
64-mesh limit; this does not remove that limit. Reload may additionally retain
up to 25 detached meshes for bounded cleanup, for at most 51 live mesh slots.

The edit fixture raises a 17-by-17 sample patch in chunk (0,0,0) to height 38 with
a stone biome. It survives travel away, unload, return, a new WorldStreamer,
and loading the save in a separate process. This is real disk integration for
the fixed fixture, **not a general multi-region world-save format**.

`--verify-policies` adds a separate bounded diagnostic mode for voxel, hex and
stationary regional policy changes. Fixed-anchor inspection is followed by a
camera-driven tiled-hex round trip; the residency source is labeled in captures.
Empty voxel meshes are synchronized
residents with no GPU allocation/draw; the GPU HUD count is logical residency,
not the number of nonempty meshes. All material surfaces draw with one opaque,
vertex-colored diagnostic material, not a production terrain texture table.
See the [tiled hex layout and compatibility gate](hex-streaming-layout.md). The default
height controls and save format are unchanged; policy mode rejects `--load`.

## Run It

From the repository root:

```powershell
cmake --build --preset build-release --target f4_streaming_demo
& ./build/release/src/demos/f4_streaming_demo.exe
ctest --preset test-release -R '^streaming_demo_capture$'
```

A combined native Release test run retains about 0.7 GB of BMP captures in new
directories. Keep disk headroom before running it. The final rerun initially
hit a full C: drive; only redundant task-generated BMPs were removed, and all
affected checks passed afterward. Reviewed galleries and user assets were retained.

The interactive run creates a new `build/streaming-demo-<id>` directory and
prints its location. WASD moves, mouse looks, Shift sprints, Space/Ctrl changes
height, H returns home, E raises the fixed patch, F5 saves, F9 reloads this run's
save into a fresh world, and Escape quits. Editing requires the home chunk to
be resident. F5/F9 queue a single background operation; busy requests are
rejected. Saves snapshot edits at acceptance. Editing pauses during reload
preparation, and the camera resets only when reload succeeds. Repeated reload
also waits for pending GPU cleanup; the HUD reports its count. Save/load errors
are reported without closing the demo. See [background I/O](streaming-async-io.md).
The shared camera's reversed strafe/mouse signs were corrected and regression-tested.

Use `--verify` for the bounded route. `--output <new-directory>` selects an
output location whose parent must exist; existing directories are rejected
instead of overwritten. `--load <world.f4sd>` loads a previous run, either
interactively or with `--verify` for a bounded separate-process load check.

Saves wrap the existing M14 sorted-delta codec in a versioned, seed-identified
M2 chunk. The seed, home chunk, policy and generation recipe are fixed to this
fixture version. Inputs are limited to 128 KiB and validated before replay.
The F5 worker writes a temporary file, checks flush/close, then renames it over the current
demo save. This is not a crash-durable production save system or a checksum
against every possible bit corruption. Demo save streams use filesystem paths;
existing capture-path limits remain unchanged.

## Visual Gates And Fix

Each full verification run produces five settled checkpoints, two captures each:
home, edited, away, returned, and disk-reloaded. The latter two must match the
edited image exactly, and the edit must visibly change more than 100 pixels.
Eight moving samples show the outbound/inbound route without worker waits.
The CTest runs the route twice, compares settled BMP hashes across processes,
then starts a third process from the first run's save and compares its two
captures against the edited view: **38 BMPs total**. Moving images are diagnostic
samples, not required to match asynchronous scheduling across runs.

The initial route found 18 changed pixels after return despite identical CPU
mesh digests for all 25 chunks. Removing skirts experimentally eliminated the
mismatch. Internal height skirts were competing at adjacent tile edges as GPU
handle/draw order changed. The final fix is opt-in
`WorldStreamingConfig::cullSharedHeightSkirts`: worker snapshots suppress an edge
skirt only if the adjacent same-policy tile exists and all shared heights match.
Outer, missing-neighbor and unequal-height edges retain skirts. Unload dirties
neighbors and restores newly exposed skirts. `HeightMeshingOptions::skirtEdges`
defaults to all four edges, preserving existing mesher fixtures/goldens.

GPU buffer caches legitimately grow to a high-water mark as visible draw counts
change. The gate therefore verifies CPU/GPU residency convergence and complete
renderer/chunk resource teardown, rather than falsely equating cache growth
with a leak. NullDevice tests independently check capped uploads, no redundant
uploads, dirty-version retention, retirement, and baseline resource recovery.

Final gallery: `build/release/streaming-demo-captures/56a40da18449/index.html`.
Measured gallery: `build/release/streaming-demo-captures/bb6304522d7f/index.html`.
The 13 distinct route views were inspected in the preceding `9b91cdadee73` run;
settled images are unchanged. The raised patch is legible and terrain is
continuous in those views. Natural terrain is low-contrast, so these images do
not prove every subtle normal is correct. The outer residency edge is visible
against the background. Sparse moving captures cannot establish the absence of
all transient popping, hidden-face defects, or arbitrary-camera failures.

## Measured Costs

Every run writes `frames.csv`, `events.csv`, checkpoint mesh digests and BMPs.
Frame measurements cover streaming/replay, GPU synchronization/upload work,
renderer/HUD recording and submission, and buffer swap. Frame intervals include
inter-frame overhead; capture frames and the following interval are excluded
from pacing observations. Save/edit/reload action wall times are separate.
There is no `waitForIdle()` in the update/render loop. It is used for resize and
final cleanup. These are CPU/driver wall times, not GPU timer-query durations.

The allocation columns count only the engine allocation seam, including worker
activity. They exclude STL, driver and third-party allocations; tracking itself
adds overhead. GPU byte columns are device-accounted buffer storage, not total
VRAM. Verification requests swap interval zero and prints the actual interval.
The route advances by fixed positions per frame, not a fixed real-world speed.

Two local GCC Release runs in `bb6304522d7f` on the i7-12700H, 1280x720,
swap interval zero, observed the following (milliseconds):

| Measurement | Run 1 | Run 2 |
| --- | ---: | ---: |
| Outbound frame-work p95 / max | 4.89 / 9.52 | 5.15 / 6.86 |
| Inbound frame-work p95 / max | 4.46 / 5.90 | 3.88 / 4.62 |
| Cold render/submit max | 52.92 | 43.22 |
| Outbound streaming max | 2.36 | 2.28 |
| Disk-replay streaming max | 3.42 | 3.75 |
| Synchronous save (two writes) | 6.19 / 8.55 | 5.99 / 7.24 |
| Reload decode/reset | 11.11 | 11.61 |

This is the historical, pre-preparation fixture profile, not a statistically
established performance guarantee. The follow-up below measures and removes
lazy terrain pipeline admission from live frames; it does not attribute every
millisecond of these older runs.

### Cold Pipeline Follow-up (2026-09-05)

The initialization call prepared the canonical non-instanced material layout.
Actual terrain uploads pass through `toResourceMesh`, PBR vertex completion,
and an instanced layout. Their first visible draw therefore created a second
material pipeline. Two instrumented baseline runs confirmed that material
pipeline preparation dominated their largest cold render/HUD frame.

`Renderer::ensureMaterialPipeline(material, mesh, mirrored)` now prepares the
exact uploaded mesh's main-pass variant. The demo initializes it using a small
terrain triangle through the real conversion/upload path, then releases that
temporary mesh. The variant remains cached for streamed terrain and remeshes.
The canonical overload remains compatible. No warmup draws or GPU waits were
added. The native verification route fails if pipeline count grows after init.

Same-session GCC Release, 1280x720, swap interval zero (milliseconds):

| Measurement | Baseline 1 | Baseline 2 | Prepared 1 | Prepared 2 |
| --- | ---: | ---: | ---: | ---: |
| Cold render/HUD max | 15.340 | 16.196 | 1.254 | 1.374 |
| Cold material preparation max | 14.729 | 15.699 | 0.0014 | 0.0014 |
| Renderer initialization, including preparation | not logged | not logged | 23.106 | 22.358 |
| Device pipeline count, initial to final | 2 to 3 | 2 to 3 | 2 to 2 | 2 to 2 |

Raw evidence: `build/release/streaming-cold-baseline-{1,2}` and
`build/release/streaming-cold-prepared-{1,2}`. These are process-cold runs with
normal driver caches, not cache-cleared machine-cold measurements. The work is
moved to explicit initialization, not made free; no total-startup improvement
is claimed. Driver timing varies, so these values are observations, not gates.

`PbrRenderSettings::profileTimings` defaults off. The demo opts in and appends
terrain render, main-pass recording, material preparation, graph execution,
HUD, and pipeline-count columns to `frames.csv`. Timings overlap: material
preparation is inside recording, which is inside graph execution, which is
inside terrain rendering. Graph execution includes submission and any shadow
pass; it is not a standalone GPU or submit timer. `events.csv` reports
`renderer_init_with_terrain_pipeline` separately (excluding window/HUD setup).

Follow-up gallery: `build/release/streaming-demo-captures/d1a8098ec2c5/index.html`.
All five settled checkpoints match the instrumented pre-fix baseline exactly;
all 38 streaming and 156 existing-gallery captures passed their repeatability
gates. Home, edited, and two moving views were visually inspected. This does
not strengthen the earlier caveats about low contrast or sparse motion samples.

The OpenGL backend's pipeline creation links attached shaders and queries link
status, matching the official [Khronos glLinkProgram reference](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glLinkProgram.xhtml).
The attribution above comes from the local timers, not an assumed API latency.

### Shared Deadline Follow-up (2026-09-05)

Previously, CPU streaming could use its full 2 ms allowance and then upload two
meshes without considering elapsed time. The demo now reserves 1 ms of a shared
2 ms target for GPU synchronization: its CPU streamer gets a cooperative 1 ms
allowance, and `sync` receives an absolute deadline starting before CPU update
and saved-edit replay. General `WorldStreamingConfig` defaults are unchanged.

Before each ready mesh conversion/upload, sync checks both its count cap and
the shared deadline. Expired deadlines defer additional uploads. One ready
upload is always admitted when the count cap is positive, even after expiry,
to avoid starvation. Unloaded chunk retirement remains mandatory; dirty CPU
versions and budget-deferred replacements retain their prior GPU meshes.
Ready candidates retain the streamer's distance/key order. Count-only callers
can omit the deadline, and count zero still disables all uploads.

The appended CSV columns report `stream_sync_us`, `stream_sync_over_budget_us`,
`upload_deferred`, `upload_budget_deferred`, and
`upload_progress_after_deadline`. Deferred counts describe ready versions left
in that frame, not cumulative unique jobs; dirty/in-flight CPU data is excluded.
Count-cap deferrals take precedence when both limits apply. The progress flag
means an upload was admitted after expiry, not that an earlier upload finished
late. Rendering, swap, and save/reset actions are outside the shared interval.

Individual CPU operations, mandatory retirement, and an admitted upload cannot
be interrupted by this policy. No GPU wait or time-based correctness assertion
was added. This is deadline-aware admission, **not a hard 2 ms guarantee**.

Two Release routes in `build/release/streaming-shared-budget-{1,2}`, same local
1280x720/swap-zero setup, observed combined stream/sync costs (milliseconds):

| Measurement | Run 1 | Run 2 |
| --- | ---: | ---: |
| Cold max | 1.411 | 1.664 |
| Outbound p95 / max | 1.301 / 1.601 | 1.007 / 1.286 |
| Inbound p95 / max | 1.247 / 1.816 | 1.299 / 1.887 |
| Disk-replay max / frames over 2 ms | 2.561 / 1 | 2.265 / 2 |

The second run deferred a second ready upload after the first exhausted the
deadline. Other overruns remained: run 1's worst disk-replay frame spent 2.549 ms
in CPU update/replay and 0.012 ms in sync, with no uploads. Run 2 also had an
admitted upload finish after expiry. No general speedup is established: the
preceding prepared-pipeline runs already stayed below 2 ms in their travel
samples. These measurements validate reporting and expose remaining work.

The seven fixture tests pass 4,623 assertions, including fixed past/future
deadline cases without timing thresholds: zero-cap behavior, deferred counts,
one-upload progress through convergence, mandatory retirement after expiry,
and retention of dirty/deferred replacements. Current gallery:
`build/release/streaming-demo-captures/dc4d37633d53/index.html`. All five settled
checkpoints match the pre-coordination baseline exactly; returned and outbound
views were inspected. Both visual gates (38 streaming and 156 gallery BMPs)
pass; sparse screenshots still do not establish absence of every transient.

Research: the C++ specification defines [steady_clock as monotonic](https://eel.is/c++draft/time.clock.steady),
appropriate for elapsed-time admission. OpenGL buffer creation uses
[glBufferData](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glBufferData.xhtml);
its usage argument is a hint, not a latency constraint. The deadline is checked
outside the existing upload call; backend buffer behavior is unchanged.

## Verification And Next Work

Twelve portable fixture tests cover save round trips, every
truncation, malformed identity/data, validation before edits, all skirt masks,
neighbor-unload skirt restoration, upload caps, resource lifetime, and failed
startup cleanup, shared-deadline admission/progress, and background I/O failure,
ownership, and shutdown behavior, plus direct replay validation and bounded
reload GPU retirement. Renderer tests additionally cover exact-layout reuse after
representative mesh retirement, mirrored variants, invalid/stale handles,
pipeline creation failure/retry, and opt-in timing/reset behavior. The camera
control test adds seven assertions on the SDL/OpenGL build. Existing world,
terrain and streaming golden digests remain unchanged.

Release passes all eight CTest targets (475 core cases / 110,922 assertions).
MSVC ASan passes six (425 core cases /
109,579 assertions, with the existing 17 native-driver cases excluded). WebGPU
rebuild/smoke verifies the unchanged default shared mesher, not this native demo.
No new Linux/macOS run or Metal hardware acceptance is claimed.

M16 remains partial. Terrain pipeline admission is now prepared and measured
outside live frames. The demo now coordinates CPU streaming and GPU upload
admission against a shared deadline, with explicit progress exceptions. Save I/O,
reload decoding/construction, and old CPU world retirement now run on a bounded
worker; see [ownership and measurements](streaming-async-io.md). Replay now uses
direct shared validation instead of scratch generation/serialization, and
reload GPU cleanup is bounded across frames. Next is stress coverage beyond
25 height chunks to voxel/hex and live regional changes, with further profiling
of non-preemptible overruns.
The interactive demo still uses the cooperative deadline. A separate opt-in
clock stops the next streaming unit, and `RegionMap::setBudgeted` stops the
next entry of a shared-map copy. Admitted work is kept when that clock expires.
`set` remains an eager full copy. Do not call this a hard 2 ms frame budget,
a full game, or complete general-world persistence.

## API Research

- [SDL relative mouse deltas](https://wiki.libsdl.org/SDL2/SDL_GetRelativeMouseState)
- [SDL swap interval semantics](https://wiki.libsdl.org/SDL2/SDL_GL_SetSwapInterval)
- [SDL capture surface ownership](https://wiki.libsdl.org/SDL2/SDL_CreateRGBSurfaceWithFormatFrom)
- [SDL BMP output](https://wiki.libsdl.org/SDL2/SDL_SaveBMP)
- [C++ filesystem rename](https://eel.is/c++draft/fs.op.rename)

Capture code follows the existing native static-capture path. Meshing,
publication, transforms, renderer resources and delta framing reuse local APIs.
