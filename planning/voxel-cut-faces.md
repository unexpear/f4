# Regional Voxel Cut Faces (2026-09-06)

The subsequent [height/hex profile audit](height-hex-boundary-profiles.md)
measures the next transition inputs and records a microscopic hex boundary gap;
it does not yet generate bridges. The voxel evidence below is unchanged.

## Findings And Plan

Triangle clipping can remove an outer voxel face without exposing the solid
cells at the new region boundary. A top-to-floor cap would incorrectly seal
tunnels and overhangs. RegionMap boundaries are integer, cell-aligned X/Z planes,
so the existing voxel mesher can instead see unowned cells as air.

The opt-in `regionalVoxelCutFaces` mode requires `regionalSurfaceOwnership`.
Both remain off by default. This closes cell-aligned voxel cuts, not gaps
between unequal height/hex surfaces. **M16 remains partial.**

Local inspection confirmed that neighbor padding feeds both face culling and
ambient occlusion before greedy merging. The original author's
[voxel meshing explanation](https://0fps.net/2012/06/30/meshing-in-a-minecraft-game/)
describes neighbor-cell face elimination and merging compatible faces. Applying
ownership at this input stage reuses those rules rather than inventing a
triangle cap. No external code or dependency was added.

## Changes

- `VoxelMeshingOptions::cellOwned` masks center and padded neighbor cells before
  culling/AO. Original block IDs are validated even when masked; input snapshots
  are not modified. Existing material/transparency rules remain unchanged.
- Streaming jobs use their immutable region snapshot and exact integer world
  cell coordinates. Out-of-domain coordinates are unowned. The masked voxel
  result bypasses generic triangle clipping because its faces already follow
  the cell-aligned region planes. Arbitrary floating or rotated cuts are not
  supported by this contract.
- Existing versioned publication and region revision checks remain in use.
  Rendering and `raycastPublishedTerrain` consume the same CPU triangles;
  GPU uploads may lag. Legacy raw-cell DDA and bilinear sampling are unchanged
  and are not ownership-masked solid queries.
- Stored edits outside the current owned region remain intact and reappear
  when ownership expands. No generic cavity filling, CSG or volume query was
  introduced.

## Verification

- Two new cases / 191 assertions cover a manually masked meshing reference
  including AO, winding, cut-face rays, tunnel back walls, roof undersides,
  unchanged input, fully unowned output and invalid hidden block IDs.
- Streaming coverage uses negative/positive chunk coordinates with one/four
  workers, a region expansion, retained handles/current region provenance and
  hidden-edit restoration. Packed resource streams and indices match a fresh
  edited reference. Existing lifecycle/version tests remain in the full suite;
  this increment does not establish race freedom.
- Windows GCC Release: 9/9 gates, 504 core cases / 359,557 assertions.
- MSVC AddressSanitizer: 6/6 gates, 454 core cases / 358,214 assertions.
  The existing 17 driver exclusions remain unchanged.
- WebGPU rebuild and browser smoke: 1/1. The shared mesher compiles for the
  browser, but this is not native streaming acceptance on WebGPU.
- Native policy gate: 148 BMPs repeated within/across two independent processes,
  including before-cut, east-face and oblique views. Ninety horizontal probes
  distinguish exposed solid faces from the tunnel back wall; an additional
  vertical probe checks the roof underside. CSVs and mesh digests match exactly
  across processes. An analytical distance assertion uses a 1e-12 tolerance:
  a correct 2-meter hit evaluated as 1.9999999999999998 in double precision.
  Image/CSV equality was not relaxed.
- Before/after and oblique screenshots were visually inspected. The closed
  voxel face retains its tunnel opening, floor and roof. The distant height
  surface remains disconnected; no full-scene watertightness is claimed.
- All 330 prior BMPs are byte-identical: 136 policy, 38 height and 156 static.
  Baselines are respectively `ba35bb09b848`, `cd32753e2089`, `4f66fe62629c`.
  Assets were untouched and TEMP/TMP remained on D:.
- Final galleries under `build/release/`: policy
  `streaming-policy-captures/2eb0a75b8427/index.html`, height
  `streaming-demo-captures/78267ad21a26/index.html`, static
  `static-demo-captures/54bd4d5ffbd7/index.html`.
- No new Linux, physical Metal, Apple hardware or TSan acceptance is claimed.

## Next

Define dependency-aware boundary profiles spanning both height and hex policy
snapshots before connecting unequal surfaces. Current jobs snapshot same-policy
neighbors only. Resolve transition ownership, overlap and solid-query semantics;
then add near-boundary camera motion and measured scheduling budgets. Static
repeatability and local voxel cut faces do not complete these acceptance gates.
