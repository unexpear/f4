# M16 Completion Checklist (2026-09-12)

This audit tracks the user's instruction to continue until M16 is complete.
The architecture plan's M16 section, status table and Current Next Work remain
authoritative. Passing a new fixture does not close unrelated acceptance gaps.

## Established Baseline

- Camera-driven policy residency, priority workers, versioned publication,
  sparse edit replay, stale-result rejection, palettes and typed resource lifetime.
- Native height demo with save/reload, static/mixed-policy diagnostics, regional
  clipping, hex cut walls and cavity-preserving voxel cuts.
- Compatible endpoint Height/Hex composition, immutable dependency capture,
  asynchronous requested-set rebuilding, staged render/query publication and
  incremental upload/retirement admission. See the linked increment reports in
  the architecture plan for tested subsets and limitations.

## Required Follow-Through

1. Boundary lifecycle: explicit retirement/retargeting is verified by Release,
   ASan and native endpoint gates. Old-view retention, cancellation, failed
   uploads, distinct world identities and immediate region changes are covered.
2. Automatic bounded discovery: region-map enumeration and caller-window
   selection are implemented. A scoped Height/Hex path now derives its active
   window from current camera-selected residency and clipped published meshes;
   full-world policy coverage and caller-independent multi-policy route
   selection remain open. Derive required boundaries from actual region
   ownership and residency, split endpoints/crossings consistently, and
   distinguish no boundaries from unsupported, incomplete or failed discovery.
   Never silently drop a required transition on failure or truncate a boundary
   set.
3. Shared world publication/query contract: an engine-level immutable
   `StreamingSurfaceView` now packages accepted resident geometry, source stamps
   and optional transition composition; the demo's displayed render/query path
   consumes that same package and its freshness gate. A process-wide accepted
   view slot and transition-aware query now exist in `WorldStreamer`; successful
   current plain/composed demo commits publish into that slot, while partial
   stale incremental states retain the prior publication. Renderer consumers
   still maintain a parallel local surface owner instead of consuming the
   engine slot directly. Callers of the general current-CPU query can still
   observe current meshes rather than a retained render view. Preserve the
   existing voxel-DDA and bilinear API contracts.
4. Solid ownership: a reference column contract now specifies and tests policy
   boundary/corner behavior, including nonplanar triangle tops, finite hex floors,
   precision limits, discontinuous breakpoints and unsupported intersections.
   Voxel cavities and overhangs are named and fail closed; a bottom-anchored voxel
   stack is still unsupported.    Surface rays remain a different API and do not
   establish this volume contract. `WorldStreamer::classifyPublishedSolid` now
   installs that classifier on the streaming collision path for published
   Height/Hex columns, including boundary planes and corners. Voxel samples
   fail closed with `UnsupportedVoxel` and never return an owner. A general
   voxel solid query remains open.
5. Moving coverage: test continuous camera/residency changes, edits, region changes,
   stale jobs and source arrivals/unloads. The resident-scope controller now has a
   focused queued-retarget regression across three outbound and two return
   positions under Release and MSVC ASan; it rejects the superseded result and
   recovers at the original corner. The native policy gate now also performs an
   eight-step outbound and inbound Height/Hex residency sweep, records accepted
   surface freshness and job state, retains the last accepted composed view when
   source coverage is temporarily unavailable, stages plain terrain before
   departure, and restores the corner exactly. The two-process gate passed with
   byte-identical movement evidence and 112 captures per run. Continuous
   frame-by-frame native coverage, edits during travel, and region changes during
   travel remain open.
6. Budget acceptance: profile integrated snapshot/planning, copy-on-write map edits,
   publication, conversion/upload and retirement. Count limits do not bound the
   cost of one operation. Large shared-map edits and continuous-movement planning
   need explicit progress and budget evidence. Do not claim a universal hard 2 ms
   deadline from a cooperative scheduler or one favorable local run.
7. Final regression/plan audit: rerun applicable native build, sanitizer,
   determinism and visual gates; check existing fixtures and assets; reconcile
   every remaining M16 claim with actual code and evidence. Hardware/toolchain
   validation must be reported as executed or unavailable, never inferred.

M17 physical Metal acceptance is separate and still requires Apple hardware.
No completion box is checked merely because this checklist or a fixture exists.

## Current Increment

[Solid-column ownership](solid-ownership.md) is now the streaming collision
query. `classifyPublishedSolid` classifies published Height/Hex interiors from
the upward triangle, and boundary or corner samples through the reference
classifiers. A cooperative frame can report `budgetClosed` when admission is
off and `budgetDeferred` when a later candidate waits; neither is a hard
deadline, and the admitted operation is not preempted. Unscoped discovery still
fails closed when a resident window contains an unsupported transition. Scoped
Height/Hex discovery still omits those edges. Focused tests cover triangle-top
disagreement with bilinear sampling, an edit that stays on the retained mesh
until republication, camera travel that keeps then drops a resident column, a
region change that does not keep the previous owner, hex floor closure, voxel
fail-closed samples, and Height/Hex boundary agreement with the reference
classifier. M16 remains partial.

[Bounded boundary discovery](boundary-discovery.md) adds ownership-driven request
selection within caller-supplied windows. [Resident reuse](boundary-resident-reuse.md)
reduces staged GPU work without changing accepted visibility or weakening
freshness checks. [Resident-coverage discovery](resident-coverage-discovery.md)
now derives a conservative window from the current camera-selected resident
set and fails closed when coverage is absent or includes unsupported required
transitions. The explicit Height/Hex scope uses clipped published mesh bounds
and passes the native corner departure/return route; broad unscoped coverage
still fails closed when it includes an unsupported required transition. The
native mixed-policy corner route uses that explicit scope because its full
resident union includes an unsupported Voxel/Hex edge.
The queued-retarget regression passes 1 case / 31 assertions in both Release and
MSVC ASan. The native stepped-residency route is covered by
`corner_motion_residency.csv` in the repeated policy gate. Subsequent increments
must update this checklist and the architecture plan without weakening the
acceptance requirements.

## 2026-09-23 Verification And Reload Fix

The movement/edit regression now verifies a valid Height edit at the settled
far anchor, eviction on return home, persisted delta replay, and byte-identical
mesh streams after reloading the far anchor. It explicitly checks rejection
with `IncompleteProfile` at the intermediate 128-metre anchor; this is evidence
of incomplete resident boundary coverage, not successful continuous coverage.

Reload now clears the displayed surface query when terrain entities detach.
The world-aware reload entry point also clears the engine's accepted view, and
native callers use it. The regression first reproduced rays hitting invisible
terrain after reload, then verifies no hit from either displayed or accepted
queries while GPU retirement remains incremental.

Use doctest's `--test-case=<pattern>` option for focused verification. A bare
test name does not filter tests; previous attempts using it ran the whole suite,
including unrelated GPU tests, so their sanitizer failures did not establish a
focused streaming-test blocker.

Verification: the combined `Streaming*` and `Composed jobs:*` selection passes
32 cases / 10,007 assertions in Release and MSVC ASan. Release CTest passes all
nine gates, including native streaming/policy captures and determinism. M16
remains partial, including the incomplete intermediate boundary above.
