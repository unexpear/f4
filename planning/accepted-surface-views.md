# Accepted Surface Views

## Finding

The displayed streaming adapter previously owned one resident mesh map while
its transition-aware raycast duplicated the selection logic locally. The
ordinary `WorldStreamer::raycastPublishedTerrain()` query, meanwhile, only
described the world's current CPU publication and could not describe a staged
or retained render/query view.

## Changes

`world::StreamingSurfaceView` is an immutable engine-level package containing:

- the exact resident mesh pointers, transforms, handles, mesh versions and
  regional revision stamps used by an accepted view;
- an optional immutable `StreamingBoundaryComposition`, whose source surfaces
  are suppressed when its composed geometry is queried;
- deterministic source or boundary provenance for ray hits; and
- a `current(WorldStreamer)` gate that checks owner identity, resident handles,
  cell/mesh versions, mesh state and regional revisions.

`StreamingDemoView` builds this package at each accepted plain or composed
commit and routes `raycastDisplayedTerrain()` through it. GPU assembly still
uses the same resident geometry and composition inputs, so a retained view can
remain queryable while newer world publication is pending without silently
switching query versions.

## Verification

The direct surface-view test confirms that an accepted view's raycast matches
the current `WorldStreamer` surface hit, rejects duplicate source keys, and
invalidates immediately after a source edit. The full streaming-demo suite
passes 24 cases / 9,787 assertions in Release and MSVC ASan. The native policy
capture gate passes two processes with 112 captures per process and byte-
identical `corner_motion_residency.csv` evidence for the outbound/inbound
residency sweep.

The 2026-09-23 reload regression reproduced a displayed query hitting terrain
after its render entities were detached. Reload now clears the local surface
package; the world-aware overload also clears the accepted world publication.
Native reload callers use that overload. The combined streaming/composed-job
selection passes 32 cases / 10,007 assertions in Release and MSVC ASan, and all
nine Release CTest gates pass.

## Remaining Contract Boundary

`WorldStreamer` now provides an explicit accepted-view slot with publish,
replace, clear and transition-aware query operations. Publication rejects a
view whose complete source stamp set is not current; an accepted view remains
queryable as retained geometry until explicitly replaced or cleared. This does
not change the legacy voxel-DDA, bilinear or current-CPU
`raycastPublishedTerrain()` APIs. The demo publishes successful current plain
and composed commits, while partial stale incremental states retain the prior
engine publication. The remaining integration work is to make every renderer
consumer use the engine slot directly rather than maintaining a parallel local
surface owner.

M16 remains partial.
