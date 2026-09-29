# Incremental Boundary Uploads (2026-09-12)

Follow-up: [explicit retirement/retargeting](boundary-retargeting.md) now supports
staged plain-terrain targets and returning to composed terrain. The unavailable-
corner behavior below still applies when the caller has not selected a new target.

## Findings And Plan

Worker composition already avoided building transition geometry on the main
thread, but live publication converted/uploaded every resident and policy mesh,
then destroyed the prior view, in one call. Splitting that loop without changing
visibility would expose staged MeshRenderer entities before the replacement was
complete. Checking only boundary dependencies would also miss unrelated resident
edits during a multi-tick whole-view upload.

The scoped fix preserves the old view, stages invisible entities over several
ticks, validates all resident stamps, commits visibility/query ownership together,
and retires detached GPU meshes incrementally. Existing backend destruction is
reused; retirement counts describe calls to the renderer, not guaranteed physical
driver-memory reclamation. Khronos distinguishes object deletion from storage
still referenced by container objects or pending use.
[OpenGL object deletion](https://registry.khronos.org/OpenGL/specs/gl/glspec44.core.pdf).

## Changes

- `syncBoundaryComposition` accepts `BoundaryUploadBudget`, defaulting to two
  upload work units and two mesh retirements per tick. An upload unit processes
  one ordinary resident or one composed policy group, including empty units.
  Optional deadlines are checked before each unit/deletion; an already-expired
  deadline admits no GPU work. Zero counts pause the corresponding work.
- Pending state retains the immutable composition, the entire desired resident
  identity/version/revision set, staged handles and upload cursors. CPU resident
  copies are taken as their units are admitted. Policy grouping is also performed
  one policy at a time. Neither is a preemptible operation.
- Staged entities have invalid visible mesh handles. They cannot draw while the
  original render/query snapshot remains active. The final main-thread commit
  reveals only unrepresented ordinary sources and the composed policy groups,
  changes query ownership, destroys old entities and queues their GPU meshes.
- Every tick and final commit validate the entire resident set, not just corner
  contributors. Source changes, arrivals/unloads, job replacement/clear and stale
  results cancel pending staging. Old displayed terrain and queries remain intact.
- Cancellation and commit use a pre-reserved retirement queue. No new staging
  starts while that queue or the earlier reload queue is nonempty. Active plus
  staged/retiring terrain remains at most two 25+3 sets in the existing 64 slots.
  This is a slot bound, not an aggregate memory bound.
- `caughtUp` stays false during staging/retirement. `boundaryUploadPending` and
  `pendingRetirements` expose progress; upload stats count admitted work and
  retired meshes. Ordinary sync can drain boundary retirements but cannot expose
  ordinary updates through an active pending composition. Reload refuses to
  overlap pending boundary work.
- Explicit removal, clear and destruction remain synchronous teardown. The older
  explicit install/replace APIs remain whole-batch reference paths and reject
  overlap with incremental work. Existing fault tests select a large tick budget
  to retain their original single-call expectations.

## Verification

- One new focused case / 430 assertions tests zero upload/retirement counts,
  expired deadlines, invisible partial staging, every-step old query/render
  retention, an edit outside composed contributors, partial-stage cancellation,
  late upload failure, retry, exact work-unit counts, incremental post-commit
  retirement, lifecycle overlap rejection and clear during staging.
- A one-unit tick requires 25 units in the fixture: 22 residents and three policy
  groups. Every pre-commit render keeps the old vertex/candidate counts and every
  tested query keeps the old hit. At commit the query updates to the edited height;
  subsequent one-mesh retirements do not affect the new render.
- The native live fixture now uses the default two-unit budget and adds a
  single-unit partial-staging checkpoint. Its pixels must equal the old composed
  image exactly. Existing pending/edit/restored captures still pass, including
  image change after acceptance and exact restoration.
- Visually inspected the partial-stage and edited native screenshots. Partial
  staging shows the intact old corner; the accepted edit shows the raised boundary
  vertex. These fixed-camera checks do not prove arbitrary-motion seam stability.
- The extra checkpoint adds four BMPs across two processes, for 208 policy BMPs.
- GCC Release: 9/9 gates; 542 core cases / 471,297 assertions.
- MSVC AddressSanitizer: 6/6 gates; 492 core cases / 469,954 assertions.
  Existing 17 driver exclusions unchanged.
- WebGPU: 1/1 regression gate. The browser app does not compile the native
  corner adapter; this is not browser acceptance of incremental corner rendering.
- Baseline comparison: all 204 prior policy BMPs, all 156 static-demo BMPs and
  all 22 settled/load height BMPs are byte-identical. Twelve of sixteen moving
  height BMPs are identical; four differ: run-1 inbound 80/120/160 and run-2
  inbound 120. All twelve prior geometry/display-query evidence files match.
- The changed inbound-120 view shows a temporary far-edge loading gap. Adjacent
  frame logs show 23 GPU chunks before that capture and 25 after, while the
  baseline was already at 25. Captures themselves are excluded from timing CSVs.
  The route uses asynchronous camera-driven residency and is explicitly not an
  exact-image motion gate. No stable checkpoint changed, and no timing/coverage
  threshold was loosened. This remains evidence that moving coverage acceptance
  is unfinished, not a claim that all 398 prior screenshots match.
- Release galleries under `build/release/`: policy
  `streaming-policy-captures/93809ae05edf`, height
  `streaming-demo-captures/056d52a596ca`, static
  `static-demo-captures/3c464ca0b040`, each with `index.html`.
- TEMP/TMP remained on D:. No new Linux/Clang, physical Metal, TSan or hard-budget
  acceptance is claimed.

## Remaining Work

Boundary retargeting/removal and moving-residency visual gates are next. An
unavailable requested corner still retains the old view rather than discovering
a replacement or silently dropping its transition.

Per-unit conversion, policy scans, GPU operations, snapshot capture, CPU cleanup
and final entity/query commit remain non-preemptible. Continuous edits or expired
deadlines can prevent progress. Profiling and finer scheduling are still needed
before any hard frame-time claim. Automatic boundary discovery/splitting, general
world-query integration, solid-volume ownership and broader backend seam gates
remain open. **M16 remains partial.** No assets were changed.
