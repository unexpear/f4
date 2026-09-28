# Asynchronous Boundary Profiles (2026-09-06)

Follow-up: [isolated wall geometry](boundary-wall-geometry.md) now consumes complete
profiles in the native audit. The controller below still schedules profiles only;
wall publication and coplanar replacement are not implemented by that follow-up.

## Findings And Plan

Immutable boundary snapshots and freshness checks were available, but callers
had no job lifecycle for replacement, stale completion or retry. This increment
adds that lifecycle before wall geometry. **M16 remains partial.** No transition
triangles are generated, and no existing rendering or regular meshing policy is
changed.

Inspected `JobSystem`, the snapshot/freshness API, profile extraction and the
native audit. Workers receive immutable snapshots rather than a WorldStreamer
pointer. One worker writes the ticket payload and publishes completion with
release ordering; the main thread reads it only after an acquire load observes
completion. This follows Microsoft's documented
[atomic memory-order contract](https://learn.microsoft.com/en-us/cpp/standard-library/atomic-enums?view=msvc-170).
It does not establish general race freedom or imply that shared ownership alone
makes mutable data thread-safe.

## Changes And Contract

- `BoundaryProfileJob` tracks one explicitly requested boundary using an external
  `JobSystem`. Call `update(world)` on the main thread to collect/retry work.
  There is no background world access or automatic boundary discovery.
- At most one outstanding ticket per controller, including completed work not
  yet collected. Repeated replacement and `clear()` retain the slot until the
  old callback drains. Clearing is logical cancellation, not worker preemption.
  Destruction does not wait; callbacks own their tickets, not the controller or
  world. The caller owns the pool and must keep it alive for controller updates.
- Completed snapshots must still match world identity, region revision, source
  membership, handles, cell/mesh versions and source readiness. Superseded or
  stale completions are discarded; subsequent ticks capture the latest ready
  sources. Pending resident sources report `WaitingSources` without submission.
- Cached results are revalidated both on update and on `result(world)` access.
  A world edit can therefore hide a stale result before the next controller tick.
  Returned pointers are main-thread views, invalidated by controller mutation;
  callers must not mutate the world between checking and consuming a result.
- `Ready` means every span has exactly one source on each side. Missing or
  overlapping coverage is retained as `Incomplete`, never accepted as wall input.
  Unchanged incomplete input is not rebuilt repeatedly. Contributor arrivals
  invalidate the snapshot and permit retry.
- Profile errors, worker exceptions and rejected submissions report `Failed`.
  Failed captured input is cached until it changes or `request()` explicitly
  retries it. Capture errors remain visible and are checked again on each tick.
  Worker exceptions are caught; main-thread allocation failures are not converted
  into status values. No allocation-failure injection coverage is claimed.
- Existing snapshot copy limits and the profile segment limit are preserved.
  The one-ticket bound is per controller, not a global queue/memory cap. Capture
  still copies meshes and scans resident footprints on the main thread; this is
  not hard frame-budget acceptance.
- The native offline audit now submits each profile to the controller and checks
  the accepted result against the synchronous live-mesh reference, including
  exact endpoints, counts, heights and provenance. Its deliberate pool wait is
  verification code, not a real-time scheduling pattern.

## Verification

- Three new tests / 217 assertions: blocked-worker stale completion after an
  edit, retry after remeshing, immediate region invalidation, unload/return,
  incomplete-input caching, twenty request replacements without queue growth,
  clear, controller/world destruction before callback completion, segment and
  copy limits, invalid boundaries and rejected submission after pool shutdown.
- One- and four-worker profile pools are exercised. Tests use a condition-variable
  blocker rather than timing sleeps for pending-work assertions.
- GCC Release: 9/9 gates; 518 core cases / 467,582 assertions.
- MSVC AddressSanitizer: 6/6 gates; 468 core cases / 466,239 assertions. The
  existing 17 driver exclusions remain unchanged. Its initial build caught an
  integer literal assigned to `optional<float>` in the new fixture under
  warnings-as-errors. Using `96.0f` fixes the fixture type without relaxing
  warning settings.
- WebGPU browser smoke: 1/1; build up to date. This native controller is not
  compiled into the browser app, so this is regression coverage only.
- All 342 prior BMPs are byte-identical by SHA-256: 148 policy, 38 height and
  156 static. Profile CSV, summary and SVG are also byte-identical. Baselines:
  `5dd08deef083`, `7aa7efc8918e`, `a309c5772722`, respectively.
- Final galleries under `build/release/`: policy
  `streaming-policy-captures/dfdc30d5eff4/index.html`, height
  `streaming-demo-captures/6fbc40abbc2d/index.html`, static
  `static-demo-captures/a919633658a5/index.html`.
- Assets untouched; build/test TEMP/TMP stayed on D:. No new Linux, Apple hardware,
  TSan, hard-budget or moving-camera transition-closure acceptance.

## Next

Build uniquely owned height/hex connecting walls from complete paired profiles.
Define crossing splits, winding/material ownership and replacement of existing
coplanar skirts/caps; publish accepted geometry to both rendering and shared
triangle queries. Integrate boundary discovery and scheduling with the world,
including global budgets and invalidation during moving-camera tests. The
audited top heights still differ by approximately 25-26 m: complete profile
coverage is input validation, not a connected terrain surface.
