# Displayed Boundary Corners (2026-09-12)

Follow-up: [live requested corners](live-boundary-corners.md) adds worker rebuilds
and staged whole-view replacement for explicit boundary requests. The frozen-only
contract below describes the original installation path, which remains available.

## Findings And Plan

The endpoint composer already produced tested CPU geometry, but the demo uploaded
only original per-chunk meshes. Overlaying composed walls on those originals
would retain duplicate source faces and leave queries describing a different view.
The scoped integration therefore retains combined dependencies, stages the whole
replacement, suppresses its represented originals, and switches view queries with
the render snapshot. Automatic live rebuilding is a subsequent increment.

Inspected capture/freshness, composition/provenance, renderer upload/destruction,
demo residency and query code. Khronos documents buffer-allocation failure;
Microsoft documents shared ownership separately from synchronization of mutable
objects. This implementation reuses the existing device error path and publishes
immutable CPU values, with world validation and view changes on the main thread.
[OpenGL glBufferData](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glBufferData.xhtml),
[MSVC library thread safety](https://learn.microsoft.com/en-us/cpp/standard-library/thread-safety-in-the-cpp-standard-library).

## Changes And Contract

- `StreamingBoundaryComposition` builds from captured boundaries with a common
  owner and region revision. Shared source identities are deduplicated; conflicting
  versions or origins are rejected. Every original capture remains retained for
  version and contributor-membership freshness checks, including source arrivals
  and unloads. Construction uses immutable data and is tested off-thread.
- Retained capture mesh bytes have an aggregate limit (128 MiB default), including
  duplicate geometry stored in separate captures. This is logical source
  deduplication, not deduplicated snapshot storage or a total peak-memory bound.
  Existing composition geometry limits remain active.
- `StreamingDemoView::installBoundaryComposition` requires a current, caught-up
  view. It stages up to three complete policy meshes in a common zero-origin
  frame, prepares their pipelines, validates again, then suppresses represented
  originals. Originals remain allocated for rollback/removal rather than being
  drawn under the replacements. Returned upload failures release staged resources.
  This does not add general allocator-exception recovery or GPU fence management.
- `raycastDisplayedTerrain` uses the installed composition plus immutable CPU
  counterparts of the remaining uploaded meshes. Hits retain original source and
  triangle provenance. It does not read newer WorldStreamer geometry behind an
  older GPU view. Existing world published-triangle, voxel-DDA and bilinear APIs
  retain their previous behavior and do not gain automatic composed surfaces.
- While installed, all demo residency synchronization is explicitly frozen.
  Source changes make `caughtUp` false but do not mix old corners with new chunks.
  Remove the composition to restore originals and resume ordinary synchronization.
  Reload and clear remove it first. This is inspection mode, not live streaming.
- Native `--verify-policies` now captures before/after, an elevated close-up,
  east/north views, four camera positions and exact restoration. Residency stays
  anchored during camera movement. These ten additional checkpoints produce 40
  new BMPs across two processes; the policy gallery now contains 188 BMPs.

## Verification

The new focused case has 76 assertions covering worker construction, source
deduplication, invalid/mixed-owner inputs, limits, staged buffer failures after
zero through three successful allocations, original suppression, display-query
provenance, north-only source invalidation, rebuilding a fresh union, frozen
render/query retention and resource teardown. The two-buffer-per-policy fixture
exercises failure after a complete first policy mesh has already been staged.

The native gate requires more than 1,000 changed non-HUD pixels between before
and after, checks 30 wall-ray hits against the displayed snapshot, compares the
query CSV across processes and requires exact before/restored image equality.
Checkpoint mesh-digest CSVs still describe WorldStreamer source publication;
they are not hashes of the newly uploaded composed meshes.

Visual inspection of the elevated, east, north and offset-camera captures shows
the formerly open gap filled by walls meeting at the corner, with no obvious
open seam in those views. The walls use existing policy materials and white wall
vertex colors. Sparse camera samples and image repeatability do not prove
flicker-free arbitrary motion or general watertightness.

- GCC Release: 9/9 gates; 538 core cases / 470,715 assertions.
- MSVC AddressSanitizer: 6/6 gates; 488 core cases / 469,372 assertions;
  the existing 17 driver exclusions are unchanged. Initial MSVC compilation
  exposed a missing explicit `<algorithm>` include and an integer literal assigned
  to `optional<float>` in the test; both were corrected without relaxing warnings.
- WebGPU: 1/1 browser regression gate. The browser app does not compile this
  native world integration; this is not browser acceptance of composed corners.
- All 342 prior BMPs and eleven prior CPU profile/wall/trim/stitch/composition/
  junction evidence files remain byte-identical. Forty new corner BMPs were added.
- Final Release galleries under `build/release/`: policy
  `streaming-policy-captures/46a9b3841ecc`, height
  `streaming-demo-captures/347d0647d9f3`, static
  `static-demo-captures/43265b93c18c`, each with `index.html`.
- TEMP/TMP remained on D:. No new Linux/Clang, physical Metal, TSan or hard-budget
  acceptance is claimed.

## Remaining Work

Add automatic asynchronous composed rebuilds and atomic live render/query
republication while edits, region changes, arrivals and unloads occur. Preserve
the prior accepted view on failure without indefinitely freezing the whole demo.
Then gate moving residency, stale job rejection and repeated replacements with
visual and query evidence. Boundary discovery/splitting, total memory/time
budgets, solid-volume ownership and general shared-world query integration remain
open. **M16 remains partial.** Assets were not modified.
