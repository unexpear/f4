# Boundary Snapshot Dependencies (2026-09-06)

Follow-up: [boundary profile jobs](boundary-profile-jobs.md) now consume these
snapshots with explicit ticks, stale-result retry and bounded outstanding work.
This report preserves the scope and verification of the earlier capture API.

## Findings And Scope

The existing meshing workers still snapshot same-policy neighbors. Future
height/hex transition work needs immutable geometry from both policies and a
gate that rejects results after either source changes. Checking only a chunk
version misses contributor arrivals, unload/reload handle replacement and
region mutations before the next update.

This increment adds explicit `captureBoundary` and `boundaryCurrent` APIs,
used by the native profile audit. It does not schedule transition jobs, cache
their results, subscribe to invalidations or generate any walls. **M16 remains
partial.** The normal renderer and meshing scheduler are unchanged.

Inspected immutable mesh storage, source versions, handle lifetime, neighbor
dirty propagation and region publication gates before coding. Microsoft's
[C++ library thread-safety guidance](https://learn.microsoft.com/en-us/cpp/standard-library/thread-safety-in-the-cpp-standard-library?view=msvc-170)
allows concurrent reads but requires protection for concurrent modification.
The new worker input therefore owns immutable copies, not pointers to mutable
WorldStreamer meshes. Shared ownership alone would not protect mutable geometry.

## Implementation

- A request identifies one X/Z region plane, a tangent interval and ordered
  Height/Hex policies. Capture validates a uniform policy pair across the whole
  interval, signed int32 world bounds and at most 4,096 region tiles. Unsupported
  legacy mode, invalid boundaries, pending resident sources and exhausted copy
  budgets have distinct errors.
- Footprint contributors are selected conservatively, including empty meshes,
  and sorted by policy/chunk key. Both sides must have current resident meshes:
  no in-flight work, current cell/mesh versions and current region revision.
- Capture owns deep copies of published CPU meshes and transforms, not raw-cell
  or newly regenerated approximations. A const shared snapshot can be copied
  to a worker; profile results preserve source/triangle provenance. Source spans
  remain valid while a snapshot owning their data remains alive.
- Copy admission defaults to 256 sources and 64 MiB of logical mesh payload;
  caller-specified limits are checked before copying any meshes. This bounds
  payload, not allocator overhead, transient metadata or total process memory.
- The freshness gate checks world identity, global region revision, contributor
  membership, generation-bearing handles, cell versions, mesh versions and source
  readiness. Region changes invalidate immediately, before update. A remote
  interior edit outside the dependency set does not invalidate a capture.
- Missing nonresident contributors are not invented or loaded. A snapshot with
  no sources can be current while its profile has zero coverage. Any later
  contributor arrival changes the membership set and invalidates it. **Current
  does not mean complete:** future wall generation must also require unique
  coverage, handle profile crossings and define solid/material ownership.
- Snapshots survive source edits, unloads and destruction of the source world.
  The private owner token prevents another world from accepting their results.
  Freshness checks are main-thread, pull-based operations; callers must check
  immediately before publication without an intervening mutation.
- Capture/validation scan resident footprints. Capture copies meshes on the
  main thread. No acceleration index, cooperative deadline or performance
  acceptance is claimed; automatic invalidation/rescheduling remains future work.

## Verification

- Three new cases / 1,390 assertions cover one/four-worker worlds, edits on both
  sides, an unchanged remote dependency, profile computation on a separate
  thread while the main thread remeshes, stale-result rejection, source arrivals,
  unload/return with replacement handles, owner mismatch and post-destruction
  snapshot reads. These tests do not establish general race freedom.
- Validation cases include unsupported/default snapshots, nonuniform/reversed
  policy boundaries, signed coordinates, tiny negative intervals, invalid
  planes, the tile cap and exact/one-byte-short/source-count copy budgets.
- The native demo compares each captured profile against the prior live-mesh
  reference, including every span, coverage count, endpoint height, source key,
  triangle and mesh version; it checks freshness before accepting the audit.
- GCC Release: 9/9 gates, 515 core cases / 467,365 assertions.
- MSVC AddressSanitizer: 6/6 gates, 465 core cases / 466,022 assertions.
  The existing 17 driver exclusions remain unchanged.
- WebGPU browser smoke: 1/1, build already up to date. This native WorldStreamer
  API is not in the browser app; that gate is regression coverage only.
- All 342 previous BMPs are byte-identical: 148 policy, 38 height, 156 static.
  Profile CSV/summary/SVG data is also unchanged. Baselines are respectively
  `4ac73583afd4`, `e0f6f3301fa9` and `98973945e830`.
- Final galleries under `build/release/`: policy
  `streaming-policy-captures/5dd08deef083/index.html`, height
  `streaming-demo-captures/7aa7efc8918e/index.html`, static
  `static-demo-captures/a309c5772722/index.html`.
- Assets untouched; TEMP/TMP stayed on D:. No new Linux, Apple hardware,
  physical Metal, TSan, moving-camera seam or hard-budget acceptance.

## Next

Use the immutable snapshot as transition-job input and reject stale completions
through `boundaryCurrent`, with explicit retry/rescheduling. Build uniquely
owned walls only on valid paired profiles. Define replacement of existing
coplanar skirts/caps, height crossings and query ownership; keep incomplete
profiles visible as missing rather than creating unsupported geometry.
