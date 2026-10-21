# Resident-Coverage Boundary Discovery (2026-09-16)

## Findings And Plan

Boundary discovery previously required a caller-owned rectangle. That made it
possible for a moving route to request a domain unrelated to the current
camera-selected residency. `WorldStreamer` already owns the authoritative
resident chunk set and the policy-specific geographic footprint arithmetic, so
the safe next step is to derive one conservative XZ union from those residents
and reuse the existing discovery/controller path.

## Changes

- `WorldStreamer::residentTerrainFootprint()` returns the finite conservative
  geographic union of every current resident chunk footprint. The policy-pair
  overload uses published mesh bounds after ownership clipping, avoiding a
  pre-clip Hex envelope that can cross an unrelated neighboring region. It
  returns empty before usable residency exists and never invents coverage from
  configuration alone.
- `BoundaryCompositionJob::requestResidentWindow()` consumes that union and
  calls deterministic discovery. Empty resident coverage is a distinct
  `NoResidentCoverage` failure; source readiness and freshness remain checked
  by the existing `update()` gate.
- The derived path retains the existing no-partial-result behavior. Unsupported
  policy transitions and discovery limits fail closed rather than being omitted.
- Focused controller coverage verifies both empty-world failure and a settled
  Height/Hex world that submits and accepts a composition from the derived
  window.

## Verification

Release focused composed-job tests pass 6 cases / 151 assertions, including
camera/residency retargeting. A separate queued-retarget route passes 1 case /
31 assertions in Release and MSVC ASan, including three outbound and two return
positions while the initial worker remains blocked. The focused
discovery suite passes 6 cases / 10884 assertions. The earlier
resident-reuse focused suite passes 23 cases / 9762 assertions. Full Release
passes all 9 gates. MSVC ASan passes all 6 registered gates, including the
resident-reuse path. The focused composed-job coverage passes 6 cases / 151
assertions, discovery passes 6 cases / 10884 assertions, and the focused
streaming-demo coverage passes 23 cases / 9762
assertions under ASan.

The native scoped Height/Hex policy route passes and writes 112 BMPs. All 56
repeat pairs are byte-identical, and all 112 common BMPs match the latest
Release policy run. Representative corner, departure, edited-corner, Hex-wall
and voxel-cavity images were visually inspected; no missing or blinking faces
were observed in this settled route.

## Remaining Work

The unscoped resident union returns a Height/Voxel edge that lies inside it.
A sample inside one published face of that edge is owned by Height below the
voxel floor. A cell corner where adjacent tops disagree is `AmbiguousSurface`.
A resident Voxel/Hex pair window contains the shared edge. Discovery returns
it. On the hex floor the published column is Hex. Inside the taller voxel
span it is Voxel.
A Height/Hex scope fails a window that contains either voxel pair instead of
dropping the edge. Capture of an isolated Height/Voxel edge copies the
published meshes and emits no wall. A retained edge that shares an endpoint
is JunctionRejected and does not drop a still-current prior composition.
The native policy gate hashes `voxel_hex_column.csv` with
the existing residency CSV and 44 sweep presents. Continuous camera movement,
accepted world/query publication outside the demo adapter, and integrated
operation-time budgets remain open. No assets were modified.
