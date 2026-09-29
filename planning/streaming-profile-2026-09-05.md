# Streaming CPU Profile: 2026-09-05

## Scope And Reproduction

This increment profiles the M16 `WorldStreamer` CPU service. It is not an
integrated GPU terrain demo, a frame-pacing benchmark, or a hard 2 ms gate.
Measurements are local Windows GCC Release observations on an Intel i7-12700H
(14 cores / 20 logical processors), with four streaming workers.

```powershell
cmake --build --preset build-release --target f4_streaming_bench
& ./build/release/src/f4_streaming_bench.exe > build/streaming-profile.csv
```

The target is explicitly built, excluded from the default build, and not a
timing-based CTest. It fails on missing convergence or failed worker results.
Scenarios cover 25 height chunks, a radius-128 height ring capped at 128 chunks,
25 voxel chunks, 64 hex chunks, and a radius-128 height ring capped at 16 chunks
with 50,000 region overrides outside the requested area. Each measures cold
convergence, 120 settled updates, and a 1,024 m diagonal camera move. The large
region scenario also edits an override while retaining an independent snapshot,
then verifies convergence with the revised map.

Worker waits occur outside measured calls, deliberately delivering bursts of
completed work. Setup, waits, convergence inspections, reporting and shutdown
are excluded. This does not measure continuously moving cameras or end-to-end
generation latency. The map-edit row measures `RegionMap::set` separately.

`WorldStreamingConfig::profileTimings` enables six stage durations; by default
they remain zero. CSV stages are planning, residency preparation, unload, load,
publication and mesh scheduling. `total` is the existing internal elapsed
counter; `call` additionally includes local cleanup before `update()` returns.
Each phase reports nearest-rank p95 and maximum, not an averaged frame rate.
`updates_over_2000us` counts whole calls over budget, repeated on each stage row;
on `region_write` it describes that separate edit. Small samples often have
p95 equal to maximum. Timings are observations, not portable pass/fail limits.

## Findings And Changes

The baseline exposed a main-thread copy of the entire regional policy map on
each large replan, resident-wide scans for every mesh neighborhood, a second
copy when storing neighbor snapshots, and full base-cell copies during unload
even though persistence consumes only sparse edits.

- RegionMap now shares value-semantic snapshots and detaches on mutation.
  Copy construction and assignment do not clone map entries. Destination
  assignment still invalidates its planning revision; unchanged `set` and
  missing `erase` operations do not invalidate it.
- Meshing performs at most 27 coordinate lookups for voxels and nine for height
  or hex chunks, rather than scanning all residents. Snapshots move into the
  work item. Lexicographic x/y/z order preserves shared height-edge precedence.
- Unload requests a locked, sorted edit-only snapshot, avoiding base-cell copies.
  Persistence format, sparse overlays and version checks are unchanged.
- Opt-in stage timings, a neighbor-query counter, the standalone benchmark and
  five regression cases cover these paths without requiring a timing threshold.

Retained baseline: `build/streaming-before.csv`. Three final runs:
`build/streaming-after-1.csv` through `build/streaming-after-3.csv`. These are
ignored local artifacts, not committed fixtures. The baseline lacks the later
`call` and region-edit rows; compare matching stage rows only. One baseline and
three subsequent runs cannot establish statistical significance or a universal
speedup; worker scheduling and CPU frequency vary.

| Observation | Before | Three final runs |
| --- | ---: | ---: |
| 50,000-region cold planning maximum | 14,013.8 us | 18.8-77.7 us |
| 50,000-region moved planning maximum | 12,949.7 us | 10.0-27.8 us |
| 128-resident height neighbor probes, cold + move | 29,161 across 272 jobs | 2,457 / 2,448 / 2,493 across 273 / 272 / 277 jobs |
| 50,000-region first edit with retained snapshot | not measured | 15,769.8-23,939.9 us |
| 128-resident height moved whole-call maximum | not measured | 3,170.0-4,146.9 us |

All five scenarios converged in every final run with zero failed work. The
fixed lookup bound is the reliable complexity improvement; it does not imply
every scene is faster (a sparse voxel neighborhood can have fewer residents
than the 27 hash lookups). Neighbor snapshot copying and whole-mesh publication
still take main-thread time, and cooperative progress can exceed 2 ms.

## Remaining Acceptance

M16 remains **partial**. Sharing removes repeated map copies during unchanged
camera traversal, but the first mutation with retained readers still copies
the entire map. Retiring a large old snapshot can also deallocate many nodes.
Do not perform large live map edits under a claimed hard frame deadline; a
bounded update/reclamation design would need separate implementation and tests.

Follow-up: the [integrated height-world demo](streaming-world-demo.md) now connects
streaming residency, GPU upload/retirement, camera motion, editing and fixed-fixture
disk/process reload. Its frame/allocation profiles expose additional cold-pipeline
and synchronous-operation spikes. The CPU harness alone did not establish this
gate, and the new height-only fixture still does not close M16 budget acceptance.

## Verification

- GCC Release: 7/7 CTest targets, 461 core cases / 105,766 assertions.
- Five new cases cover region copy/assignment isolation and concurrent snapshot
  reads, sorted independent edit-only snapshots and stale handles, exact bounded
  neighbor-query counts for all policies, and optional timing accounting.
- World, terrain and streaming golden determinism gates remain unchanged.
- All 156 static captures repeated exactly and matched the prior reviewed
  `ef788b33a3fe` set byte-for-byte. Final gallery: `fd00e15c9945` under
  `build/release/static-demo-captures/`. No new visual behavior is claimed.
- WebGPU Debug rebuilt and browser smoke passed; it compiles shared world-data
  code but does not run the native WorldStreamer service.
- MSVC AddressSanitizer: 6/6 CTest targets, 412 core cases / 104,430 assertions.
  The existing driver-compatibility filter skips 17 Vulkan/DX12 cases; Release
  runs those native GPU gates separately. ASan does not establish race freedom.
- No fresh remote Linux/macOS execution or physical Metal acceptance is claimed.

## Research

The implementation follows the local HashMap, ChunkStore snapshot and streaming
publication patterns. RegionMap supports concurrent access to **distinct value
copies**, not mutation and reading of the same RegionMap object. Owning copies
remain alive for reads; no raw map reference or weak ownership escapes. Shared
ownership alone is not synchronization for mutable pointees.

- [C++ shared ownership](https://eel.is/c++draft/util.smartptr.shared)
- [C++ shared pointer observers](https://eel.is/c++draft/util.smartptr.shared.obs)
- [Microsoft standard-library thread-safety rules](https://learn.microsoft.com/en-us/cpp/standard-library/thread-safety-in-the-cpp-standard-library?view=msvc-170)
- [Microsoft steady clock](https://learn.microsoft.com/en-us/cpp/standard-library/steady-clock-struct?view=msvc-170)
- [CMake explicit executable targets](https://cmake.org/cmake/help/latest/command/add_executable.html)
