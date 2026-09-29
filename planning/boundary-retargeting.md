# Boundary Retirement And Retargeting (2026-09-12)

## Findings And Plan

The incremental uploader previously had only a composed target. When travelling
away from a requested corner, missing source coverage caused the whole old view
to remain active. Calling synchronous removal would briefly restore outdated
originals instead of staging current plain terrain. Clearing a worker request
also did not express whether a displayed transition should be removed.

The fix adds an explicit no-boundaries target and reuses the staged whole-view
transaction. Retirement remains distinct from idle, invalid and failed requests.
The worker controller retains its existing release/acquire ticket publication
and generation-based rejection; no new backend synchronization API is introduced.
[Microsoft atomic ordering](https://learn.microsoft.com/en-us/cpp/standard-library/atomic-enums?view=msvc-170).

## Changes

- `BoundaryCompositionJob::requestRetirement` supersedes any queued composition
  and exposes `NoBoundaries`. The old callback drains without publishing and does
  not block plain-terrain staging. `clear()` still means stop requesting, not
  remove displayed geometry; empty `request()` remains invalid.
- The incremental uploader supports plain resident transactions without inventing
  an empty composition. It stages current resident meshes, keeps the accepted view
  visible, commits render/query ownership together and retires old meshes under
  the existing per-tick limits. A later explicit boundary request can replace that
  plain view through the same path.
- Transactions now retain an opaque WorldStreamer lifetime identity and region
  revision in addition to every resident stamp. A new world with coincidentally
  recycled handles/versions, or a region edit before update, invalidates staging.
  Accepted staged plain views also validate identity/revision in `caughtUp`.
  Ordinary per-resident synchronization retains its older independent contract.
- Retargeting during a partial upload cancels hidden work and drains its retirement
  queue before restarting. Build/upload failure does not silently select plain
  terrain. All targets here remain explicit caller decisions.

## Verification

- Two focused cases / 66 assertions cover retirement during a blocked queued job,
  stale completion rejection, idle-versus-removal semantics, retargeting while
  plain uploads are hidden, region-revision invalidation, failed plain upload,
  restore-to-composed publication, world identity replacement and resource teardown.
- The native route selects a plain target 512 m away in both X/Z, then requests
  the original corner on return. Pending departure retains the old corner image;
  pending return retains the accepted away image. The away surface query must hit
  plain Height geometry and the completed home image must exactly match its
  pre-departure composed view.
- Four new checkpoints add sixteen BMPs across two processes, raising the policy
  gallery to 224 images. Sources settle at route endpoints and the diagnostic
  waits for composition workers. This is lifecycle evidence, not automatic
  discovery or continuous-motion coverage/frame-time acceptance.

Release passed 9/9 gates (544 core cases / 471363 assertions). MSVC ASan passed
6/6 gates (494 core cases / 470020 assertions; 17 existing driver exclusions).
The WebGPU regression passed 1/1; it does not exercise this native controller.
Gallery: `build/release/streaming-policy-captures/6149f2ad5a88/index.html`.
The returned-corner and away-plain BMPs were visually inspected: the returned
wall is continuous in this view and the away view contains plain terrain.
Image equality gates, not a still image alone, establish pending-view retention.
All 208 prior policy and 156 static BMPs, and all 12 geometry/query reports,
remain byte-identical. Of 38 height BMPs, 32 match; six asynchronous inbound
captures differ. The known moving-coverage risk remains open, not waived.

## Remaining Work

[Bounded window discovery](boundary-discovery.md) follows this increment.
Residency-derived window selection is still open. Do not convert incomplete
or unsupported discovery into a no-boundaries target. Shared world publication,
solid ownership, continuous moving-coverage gates and budget acceptance remain
open; see [the full M16 checklist](m16-completion-checklist.md).
**M16 remains partial.** No assets were changed.
