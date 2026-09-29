# Tiled Hex Streaming (2026-09-06)

This records the tiled-layout increment. The subsequent
[mixed-policy audit](mixed-policy-boundaries.md) expands the policy gate to
100 captures and supersedes the next-work list below; the layout results remain valid.
The later [boundary profile audit](height-hex-boundary-profiles.md) finds a
microscopic gap between translated published hex meshes. Lattice packing is
fixed, but numerical shared-endpoint closure is not established by this report.
The [canonical endpoint follow-up](canonical-hex-endpoints.md) fixes that measured
interval in opt-in regional mode; legacy local meshing remains unchanged.

## Findings And Design

The preceding [policy audit](streaming-policy-checks.md) found that a hex-shaped
chunk on a square-width axial stride left holes. Rendering alone could not fix
this: generation, neighbor lookup, camera ownership, region lookup and distance
priority all needed a consistent layout. Filling the holes with extra cells
would instead change the existing local shape and offset-row indices.

`HexLayout::Tiled` retains that shape and those indices. For radius R, chunk
X/Z are translated on the axial basis A=(2R+1,-R), B=(R,R+1). Its determinant
is 3R(R+1)+1, the number of cells in the radius-R hex. Neighbor centers tile the
cell lattice; they are no longer separated by unused cells. The six existing
chunk-neighbor directions still apply.

`hexChunkOrigin` is shared by generation, meshing, neighbor sampling and render
placement. `hexCellAt` uses inverse pointy-top coordinates and cube rounding.
`hexChunkAt` inverts the center lattice, then tests a bounded 3x3 neighborhood
with exact integer hex distance to determine cell ownership. This avoids using
approximate floating-point nearest-center ties as the ownership rule.
Inputs are checked before integer conversion; unsupported radius/layout,
invalid scale, nonfinite positions and coordinate overflow are rejected.

Streaming planning and its cache use that ownership calculation. Regional
selection floors actual geometric center coordinates into RegionMap, and
priority uses world-space center distance including `hexRadius`. Local mesh
translation uses the same axial lattice. Tiled boundary edits locate the actual
owning neighbors and deduplicate them so each gets one version bump per edit.
Height and voxel coordinate rules are unchanged.

## Compatibility

- Standalone `HexPolicyParams` defaults to `LegacySpaced`, retaining original
  generation, offset-row cell indices, legacy saves and golden fixtures.
- New `WorldStreamingConfig` defaults its hex parameters to `Tiled`. Assigning
  a separately constructed legacy `PolicyParams` explicitly selects legacy
  generation instead; it does not get silently upgraded. Legacy rendering is
  still spaced, but camera/region/distance calculations now match its geometry.
- Tiled `ChunkStore` delta frames use version 2. After the existing coordinate,
  overlay-version, count and policy fields, they carry worldSeed (u64), radius
  (u32), heightAmplitude (f32) and frequency (f32), then the existing cell deltas.
  Version 2 implies tiled hex layout and is accepted only by the same recipe.
- Legacy hex, height and voxel retain version 1. Nonempty cross-layout or
  mismatched tiled-recipe input is rejected before changing the live overlay.
  Empty input still explicitly clears an overlay, retaining the original
  zero-byte untouched-chunk convention; it contains no identity to migrate.
- There is no automatic remapping of old hex edits into a tiled world. Radius,
  seed, generation parameters and cell ownership cannot be inferred safely.
  The standalone height demo's save format and controls remain unchanged.
- This is a chunk-delta compatibility gate, not a general multi-region disk
  save. Geometry scale and region configuration remain external world settings.

## Verification Scope

Five new focused cases check unique cell ownership/coverage at radii 0, 1, 2,
3 and 16, signed coordinates, scales 0.5/1/3, supported radius-2048 boundaries,
invalid inputs, recipe/legacy/truncation rejection without partial mutation,
geometry-aligned nearest residency and region changes, exact shared corner
heights, exposed-wall index counts, and precise neighbor edit invalidation.
These contribute 219,502 assertions; the old world/terrain/streaming golden
fixtures are unchanged.

The native OpenGL policy gate now has 20 checkpoints repeated twice in each of
two processes: **80 BMPs**. It retains the fixed-anchor voxel/hex inspections,
empty-air check and three stationary region cycles, and adds a 240-step
camera-driven hex round trip. Home edits survive unload/return, all nine chunks
draw at the destination, and settled captures match within/across processes.
The fixture uses four asynchronous workers without `waitForIdle` in live frames.
Visual inspection confirms continuous hex tops and undersides rather than the
previous holes. This is not a universal moving-camera or cross-driver guarantee.

## Remaining Work

Mixed-policy regional seams, cancellation-heavy live region changes, large
copy-on-write maps, non-preemptible frame-budget overruns, general world saves,
and physical Metal acceptance remain open. The legacy spaced layout deliberately
retains its old geometry. User assets were not modified.

## Local Results

- GCC Release: **9/9** CTest gates, **484 core cases / 331,504 assertions**.
- MSVC AddressSanitizer: **6/6**, **434 core cases / 330,161 assertions**;
  the existing 17 driver-compatibility exclusions remain. Release runs the
  native driver checks separately. ASan does not establish race freedom.
- WebGPU rebuilt the shared world-data/terrain code and passed browser smoke
  **1/1**. This does not run the native WorldStreamer service in the browser.
- All five height checkpoints remain byte-identical to the preceding reviewed
  `eff972c4dd68` gallery. Existing world/terrain/streaming golden gates pass.
- Reviewed tiled hex top/underside and camera home/destination captures. The
  policy gate also verifies matching camera-return images and full GPU teardown.
- Policy gallery: `build/release/streaming-policy-captures/0340e8931197/index.html`.
  Height gallery: `build/release/streaming-demo-captures/26b0f8b5b164/index.html`.
  Static gallery: `build/release/static-demo-captures/e0f59a1cc54d/index.html`.
- `git diff --check` passed. Builds and tests used D: TEMP/TMP. No new Linux,
  macOS, physical Metal, or TSan result is claimed.

## Research

The layout is a reflected orientation of the mirror-center construction in
[Red Blob Games' original hex-grid guide](https://www.redblobgames.com/grids/hexagons/#wraparound).
The same author's [coordinate conversion and cube-rounding implementation](https://www.redblobgames.com/grids/hexagons/implementation.html)
informs the world-to-cell mapping; the bounded integer ownership check is
verified independently against explicit cell coverage in the project tests.
Range checks follow the [C++ floating-to-integer conversion contract](https://eel.is/c++draft/conv.fpint).
