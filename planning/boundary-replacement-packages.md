# Immutable Boundary Replacement Packages (2026-09-07)

Follow-up: [disjoint shared-source composition](boundary-composition.md) now
preserves successive cuts across separated boundaries, and [endpoint junctions](boundary-junctions.md)
now reconcile compatible touching corners. Combined dependency capture and coordinated publication remain open. The original
single-boundary package contract and evidence below are unchanged.

## Findings And Plan

Trimmed source meshes, stitched tops and walls were verified separately, but did
not form one immutable result carrying source dependency stamps. The demo still
uploads and retires terrain independently per chunk; that is not a transaction
for replacing several sources and their connecting wall together.

This increment builds a single-boundary package before changing publication.
**M16 remains partial. No package is installed into the renderer or WorldStreamer.**
Inspected snapshot ownership, per-chunk upload synchronization, geometry builders
and query provenance. Standard-library shared ownership does not protect mutable
data; the package exposes const geometry and retains an immutable source snapshot.
[Microsoft thread-safety guidance](https://learn.microsoft.com/en-us/cpp/standard-library/thread-safety-in-the-cpp-standard-library?view=msvc-170).
Khronos also requires compatible inputs and computations for invariant shader
outputs. Common world-space vertices remove differing mesh-origin arithmetic
from the package, but GPU seam acceptance still requires a consistent draw path.
[GLSL invariance](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html).

## Changes And Contract

- `buildTerrainBoundaryReplacement` handles one complete ordered Height/Hex-style
  top boundary under explicit solid-column and replaceable-skirt/cap assumptions.
  It composes profile extraction, coplanar trimming, canonical edge stitching and
  wall generation, without mutating any input mesh.
- The result includes full contributing source meshes, not just narrow seam
  strips. Consumers must suppress all originals represented by a package before
  displaying/querying it. Two independent packages sharing source chunks must
  not be installed together; multi-boundary composition is not implemented.
- Every referenced source vertex is converted to the common world-space float
  frame. The conversion must be exact, otherwise the build returns PrecisionLoss.
  Output uses zero origin, three vertices per triangle and sequential indices.
  This deliberately favors unambiguous geometry/provenance over compact storage.
- The original triangle id survives both trim and stitch mappings. Each output
  triangle identifies source/wall kind, policy side, source index and original
  source triangle. Wall provenance identifies its higher-side source top.
  Source attributes are retained; material ranges do not merge across policy
  sides. Policy-local material resolution remains the consumer's responsibility.
- Explicit limits default to 256 sources, 262,144 output triangles and 128 MiB
  final logical payload, including vertices, indices, surface ranges and provenance.
  The triangle option accepts 1..1,000,000; existing per-builder limits also apply.
  These are not bounds on snapshot storage, intermediates, allocator overhead,
  elapsed time or total process memory. Allocation exceptions are not recovered.
- `StreamingBoundarySnapshot::buildReplacement` produces an opaque immutable
  `StreamingBoundaryReplacement`. It retains the captured owner, region revision,
  membership, source handles and cell/mesh versions alongside the geometry.
  It is safe to build on a worker using only the immutable snapshot.
- `WorldStreamer::boundaryCurrent(replacement)` reuses the snapshot freshness
  gate. Geometry remains readable after edits or source destruction, but is not
  automatically current. A stale snapshot can build a package that must then be
  rejected. Neither geometry access nor shared ownership bypasses that gate.
- The native audit builds packages from accepted profile snapshots, validates
  freshness, compares wall counts and casts rays into the combined mesh. Those
  rays must hit generated walls with original source provenance, not old caps.
  Render-layout and explicit metadata hashes repeat across two processes; hashing
  does not include C++ struct padding or scheduling-dependent version counters.

## Verification

- Three new tests / 56 assertions cover common-frame geometry, removed old walls,
  top/wall query provenance, policy-separated material ranges, unchanged inputs,
  exact/one-byte-short payload limits, triangle/source caps, incomplete/duplicate
  coverage, precision rejection, default handles, worker construction, stale
  rebuild rejection, owner mismatch and reads after world destruction.
- Native east package: 8,823 triangles (160 walls), 26,469 vertices, three material
  ranges, 160 combined wall ray pairs. North: 8,850 triangles (146 walls), 26,550
  vertices, three ranges, 146 pairs. `height_hex_replacement.csv` records counts
  and repeatable render/provenance hashes. This is CPU package acceptance only.
- GCC Release: 9/9 gates; 531 core cases / 468,898 assertions.
- MSVC AddressSanitizer: 6/6 gates; 481 core cases / 467,555 assertions.
  Existing 17 driver exclusions unchanged.
- WebGPU: 1/1 browser smoke, build up to date. The native boundary module is not
  compiled into that app; this is regression coverage, not package rendering.
- All 342 prior BMPs are byte-identical: 148 policy, 38 height, 156 static.
  All eight previous profile/wall/trim/stitch CSV and SVG files are unchanged.
  Existing in-world pixels therefore do not show the newly built package.
- Baselines: policy `d92b8e9bc8ee`, height `fdd2383ad52f`, static `f50e9df879ca`.
  Final galleries under `build/release/`: policy
  `streaming-policy-captures/d4ae6428a91d/index.html`, height
  `streaming-demo-captures/38f13230536e/index.html`, static
  `static-demo-captures/a6991bc99292/index.html`.
- Assets untouched; build/test TEMP/TMP stayed on D:. No new Linux/Clang parity,
  physical Metal, TSan, global-budget or moving-camera seam acceptance.

## Next

Compose multiple boundaries over shared source meshes while preserving every
earlier trim/stitch and unique wall ownership. Then stage all GPU resources
without changing the displayed/queryable result; revalidate dependencies and
swap the complete replacement, rolling back failed uploads. Queries must use
the same accepted geometry/version as rendering, with original source identity.

Use the common transform path for in-world static and moving-camera seam tests.
Boundary discovery, asynchronous geometry scheduling, global budgets, corner
composition, solid-column validation and unsupported precision cases remain
acceptance work. Immutable package construction is not atomic world publication.
