# Bounded Boundary Discovery (2026-09-12)

## Findings And Plan

The composition worker accepted explicit lists only. Departure and return could
stage correct views, but callers still had to enumerate every transition. This
increment derives the complete Height/Hex edge set inside a supplied rectangle
from RegionMap, then feeds the existing transactional controller. It does not
claim that a caller's rectangle covers every resident or visible surface.

The implementation follows the existing regional-mask integer-domain and
half-open tile enumeration rules. Negative coordinates require floor, not
truncation; division rounding at an adjacent endpoint is corrected against the
exact integer tile plane. Floating coordinates are validated before narrowing.
[Microsoft floor reference](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/floor-floorf-floorl?view=msvc-170).

## Changes

- `discoverTerrainBoundaries` samples at most 4096 ownership tiles. It emits at
  most 256 requests (default 64) in deterministic Z/X tile order, X edge first.
  Both limits are checked; a failed result never exposes a truncated prefix.
- Edges are clipped to the selected rectangle and split at every region-grid
  vertex. Straight runs intentionally remain split: perpendicular arms cannot
  form an interior crossing. Geometry composition still checks sector heights
  and rejects nonmanifold junctions; discovery is not geometry acceptance.
- Only differing Height/Hex pairs are supported. Any required voxel transition
  fails the entire selection. Uniform voxel ownership has no inter-policy edge
  and may correctly return an empty set. Edges lying on the window perimeter
  are excluded because only one side is selected.
- `BoundaryCompositionJob::requestWindow` discovers and selects the target.
  An unchanged set preserves the worker ticket, accepted result and cached build
  failure. An empty success requests staged retirement. Invalid, unsupported or
  over-limit discovery clears the target and exposes a persistent Failed state,
  including while superseded work drains. It never falls back to plain terrain.
- The native corner route now selects departure/return requests through ownership
  discovery. Its home window is [0,96] in X/Z; the away window is [512,608].
  These intentionally reproduce the prior explicit fixture, not the full
  resident footprint. Existing image checkpoints and equality gates are retained.

## Verification

Nine focused discovery/composition cases passed 10996 assertions. Discovery
tests cover all 512 two-policy 3x3 neighborhoods, immutable map snapshots, exact
directed edges, clipped tangents, four-arm endpoints, insertion order, tile and
request limits, unsupported mixed pairs, signed-domain extrema, NaN/infinity and
the representable coordinate immediately below zero. Controller tests cover
unchanged in-flight and accepted selections, failure during a blocked worker,
stale completion discard, empty selection, and recovery after a failed selection.

Release passed 9/9 gates, including 550 core cases / 482276 assertions. WebGPU
passed 1/1 as a regression only; that app does not use this native controller.
MSVC ASan passed 6/6 gates (500 core cases / 480933 assertions; 17 existing
driver exclusions). Linux/Clang and physical Metal were not run in this increment.

Gallery: `build/release/streaming-policy-captures/8f3cd65359b5/index.html`.
All 224 policy and 156 static BMPs and all 12 geometry/query reports are
byte-identical to the retirement increment. The returned-corner BMP was visually
inspected again and shows the same continuous wall in that view. Of 38 height
BMPs, 36 match; run-2 inbound frames 40 and 80 differ. These asynchronous motion
differences remain part of the open coverage risk, not an exact-image gate.
Side-by-side inspection of run-2 inbound frame 40 shows the preceding run's
missing far-left section filled in this run, despite no ordinary height-upload
code change in this increment. This is asynchronous loading variability, not a
coverage fix. Frame telemetry reaches only 20 displayed chunks during both travel
directions; counts alone do not establish coverage of any required spatial domain.
No assets were modified.

## Remaining Work

The caller must still select a coverage window and re-evaluate it after movement
or ownership changes. Discovery intentionally ignores mesh readiness: the
capture/profile/composition gates still reject missing source coverage. Deriving
a safe active domain from camera/residency, continuous-motion selection and
source-unload behavior remain open. So do the shared world publication/query
contract, solid ownership and integrated budgets in the
[M16 completion checklist](m16-completion-checklist.md). M16 remains partial.
