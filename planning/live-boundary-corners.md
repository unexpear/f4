# Live Requested Boundary Corners (2026-09-12)

Follow-up: [incremental boundary uploads](incremental-boundary-uploads.md) now
spreads upload units and detached GPU retirement over ticks. The whole-batch
description below records the original implementation; explicit batch APIs remain.

## Findings And Plan

The prior corner adapter could only install once into an already synchronized
view. Its ordinary `sync` deliberately froze residents to prevent newer source
meshes from appearing under an old transition. Removing the corner before upload
would expose precisely that intermediate state. The next bounded step is a
worker-built replacement plus a complete staged resident/render/query transaction.

The existing `BoundaryProfileJob` supplied the immutable-ticket, generation and
stale-result pattern. Microsoft documents the acquire/release visibility ordering
used to publish worker results; callbacks own only their ticket, never the world
or controller. World capture, dependency checks, GPU work and view commit stay on
the main thread. No new external dependency or backend command API was added.
[Microsoft atomic ordering](https://learn.microsoft.com/en-us/cpp/standard-library/atomic-enums?view=msvc-170).

## Changes

- `BoundaryCompositionJob` follows an explicit caller-selected set of at most
  256 requests, further limited by composition limits. Main-thread ticks capture
  immutable dependencies and submit one worker build. Changed sources, membership
  or region revisions invalidate results; stale completions are discarded and
  subsequent ticks retry. Request replacement/clear cannot grow the worker queue.
- Workers release their completed payload through an atomic flag; the main thread
  acquires it before inspection. Destruction is safe for queued callbacks. The
  external job pool must outlive the controller. This is not a TSan/race-freedom
  acceptance claim.
- Snapshot mesh-payload accounting now also limits aggregate capture copies
  before worker submission. Failed unchanged build inputs are retained to prevent
  repeated rebuilds; a changed dependency or explicit re-request retries them.
  Capture failures and rejected submissions are reported. Capture retries can
  repeat synchronous work; there is no total CPU/working-memory budget guarantee.
- `replaceBoundaryComposition` stages every current original resident and the
  composed policy meshes before retiring the previous view. All world residents
  must be settled and the composition current. The final main-thread commit
  changes resident ownership, visible meshes and displayed-surface queries together.
  Old originals are not drawn below the new composition. Removing a successfully
  replaced corner restores the new originals, not the pre-edit originals.
- `syncBoundaryComposition` ticks the controller and publishes a current result
  when the view needs it. Pending work keeps the prior view; returned upload
  failures roll back staged resources and permit retry. Unchanged current views
  do not reupload. This API is used instead of ordinary sync for explicit requests;
  the default interactive height demo remains unchanged.
- At most two sets of 25 originals plus three policy meshes coexist, within the
  existing 64-slot arenas. Detached reload meshes exclude replacement. Uploads,
  CPU copies and retirement remain a single non-preemptible batch. This favors
  correctness over efficiency and does not close the frame-budget requirement.

## Verification

- Three focused cases / 152 assertions cover stale worker rejection, unchanged
  result caching, request replacement/clear queue bounds, aggregate capture limits,
  build rejection, explicit retry, shutdown rejection and controller destruction.
- View tests inject failure before/inside original uploads and before/inside/between
  composed policy uploads. Every returned failure preserves old query versions and
  distances, buffer counts and buffer bytes. Successful editing updates displayed
  height, eviction retains the old view when the corner becomes incomplete, and
  return rebuilds successfully with persisted edits. Final resource totals return
  to baseline.
- The native fixed-anchor diagnostic adds initial, pending, edited and restored
  captures. Pending CPU edits leave the old displayed image exactly intact. An
  accepted edit raises a source boundary vertex to Y=45, changes the image and is
  hit at Y=45 by a displayed-surface query. Restoration is byte-identical to the
  initial composed view. The controller accepts exactly three compositions.
- These four checkpoints add sixteen BMPs across two native processes; all 204
  policy BMPs repeat within/across processes. Checkpoint mesh CSVs still describe
  WorldStreamer CPU sources, which intentionally differ from the display while
  an edit is pending. They are not hashes of uploaded composed GPU buffers.
- Native convergence waits for workers in the diagnostic harness. This validates
  asynchronous construction and safe publication, not a nonblocking interactive
  frame-time or continuous-motion acceptance gate.
- Visually inspected the final elevated edit and restoration captures. The raised
  boundary vertex and its adjoining wall are fully in frame; restoration removes
  the peak and returns the joined corner. This is a narrow single-vertex edit,
  not a broad terrain sculpting or arbitrary-motion seam test.
- GCC Release: 9/9 gates; 541 core cases / 470,867 assertions.
- MSVC AddressSanitizer: 6/6 gates; 491 core cases / 469,524 assertions.
  Existing 17 driver exclusions unchanged.
- WebGPU: 1/1 browser regression gate. This app does not compile the native
  composition controller; it is not a browser rendering acceptance claim.
- All 382 prior BMPs and twelve prior CPU/display-query evidence files are
  byte-identical. The sixteen new live-request images are additional evidence.
- Final Release galleries under `build/release/`: policy
  `streaming-policy-captures/5468c7771ca6`, height
  `streaming-demo-captures/b8d24eb3ad75`, static
  `static-demo-captures/208832949b90`, each with `index.html`.
- TEMP/TMP stayed on D:. No new Linux/Clang, physical Metal, TSan or hard-budget
  acceptance is claimed.

## Next

Bound upload/retirement work across frames without exposing partially staged
versions. Add explicit boundary retargeting/retirement and moving-residency visual
gates: if a requested corner no longer has complete coverage, this implementation
retains the entire prior view rather than discovering another boundary or silently
dropping the transition. Continuous edits can defer publication indefinitely.

Automatic discovery/splitting, general WorldStreamer surface-query integration,
solid-volume ownership, hard budgets and broad backend seam acceptance remain
open. **M16 remains partial.** No assets were changed.
