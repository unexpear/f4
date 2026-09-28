# Reusing Unchanged Displayed Terrain (2026-09-12)

## Findings And Plan

Every staged boundary/plain publication previously recopied and reuploaded all
residents. At two upload units per tick, 25 unchanged residents alone required
13 ticks. A moving camera could invalidate that work before it completed. This
increment removes unnecessary resident work without allowing partial visibility
or transferring ownership before the complete view is ready.

CPU geometry and the opaque world-lifetime token retain their existing shared
ownership. GPU handles remain explicitly owned by the accepted view during
borrowing, not by a new independent owner. Copying a shared pointer retains its
existing ownership rather than adopting the same raw pointer again.
[Microsoft shared_ptr guidance](https://learn.microsoft.com/en-us/cpp/cpp/how-to-create-and-use-shared-ptr-instances?view=msvc-170).

## Changes

- Each resident records its source-world identity. Reuse requires that identity,
  chunk handle, current published/cell version, region revision and mesh origin
  to match. Ordinary synchronization and caught-up checks use the same predicate,
  preventing coincidentally recycled handles from another world being accepted.
- Staging may borrow unchanged residents without copying CPU triangles, creating
  GPU buffers or allocating entities. Those entities retain their accepted
  visibility, including suppression by an old composition, throughout staging.
- Cancelled or failed staging releases only its newly owned resources. Borrowed
  handles stay in the accepted view. Commit switches visibility and query state,
  skips retirement of borrowed residents and transfers their ownership once.
  The existing whole-view freshness check remains mandatory before commit.
- `StreamingUploadStats::reused` counts borrowed residents. They do not consume
  upload units; admission still respects the deadline, and at most 25 residents
  can be examined in this view. New/empty resident and composed-policy groups
  retain their old upload-unit accounting. Count limits are not hard time limits.
- Reuse is enabled by default. `BoundaryUploadBudget::reuseUnchanged = false`
  retains the full-reupload reference path. Existing exhaustive allocation-fault
  and incremental-count tests explicitly use it; their stages/assertions were
  not removed. New default-path tests cover borrowed ownership and resource reuse.
- The native corner fixture writes `corner_reuse.csv`, checks exact expected
  reuse from before/after source stamps and compares counters across processes.
  All existing screenshot and geometry-report gates remain intact.

## Verification

The focused streaming-demo suite passes 23 cases / 9762 assertions. Release
passes all 9 gates, including capture, core, and the four determinism checks.
MSVC ASan passes all 6 registered gates; the aggregate core run passes after a
retry when the host had enough virtual-memory headroom, and the focused
streaming-demo run passes all 23 cases / 9762 assertions under ASan.

New tests cover borrowed-only staging failure/cancellation/destruction, exact
resource counts after composed/plain transfer, partial movement retaining current
residents, per-tick upload limits, another world refusing reuse, and restoration
of every device resource baseline. The full-reupload reference continues to test
stale non-corner source cancellation and failures at every original/policy stage.

## Remaining Work

This removes one source of unnecessary work; it does not by itself fix ordinary
height-demo travel coverage or make moving transactions immune to cancellation.
The ordinary demo still updates residents independently. Camera/residency-derived
boundary domains, a shared accepted world/query publication contract, continuous
moving coverage, solid ownership and integrated budget acceptance remain open.
M16 remains partial; no asset files were changed.
