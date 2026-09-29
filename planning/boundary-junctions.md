# Endpoint Boundary Junctions (2026-09-07)

Follow-up (2026-09-12): [displayed corner integration](displayed-boundary-corners.md)
now retains combined dependencies and displays this bounded Height/Hex fixture
with matching view queries. The CPU-only status below describes this earlier
increment. Automatic live transition rebuilding remains unfinished.

## Findings And Plan

The shared-source composer previously rejected all touching boundary requests.
Simply accepting them would not establish consistent sector heights or matching
vertical wall edges. A three-height junction can put one long wall edge against
two shorter ones, requiring subdivision without changing wall area or provenance.

Inspected the existing composition, top-profile, wall-generation and query paths.
CGAL's mesh-repair documentation describes matching reversed border endpoints
and opposite traversal of paired incident edges. That is a reference criterion
for the new geometry tests, not a new dependency or a claim that these unindexed
triangle copies form a validated halfedge mesh.
[CGAL combinatorial repair](https://doc.cgal.org/latest/PMP_Mesh_repair/group__PMP__combinatorial__repair__grp.html).
GPU position invariance still needs compatible shader inputs and computations;
CPU edge matches alone do not establish rendered seam stability.
[GLSL invariance](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html#variance-and-the-invariant-qualifier).

## Changes And Contract

- The pure composer now accepts compatible shared endpoints, including straight,
  L, three-arm and four-arm configurations. Positive-length overlaps and contacts
  in a request's interior still return `ConflictingBoundaries`. Callers must split
  interior crossings/T contacts into endpoint requests; discovery is not automatic.
- Incident profiles assign exact endpoint heights to NE/NW/SW/SE sectors.
  Shared sector values must agree. An absent arm requires matching endpoint column
  heights across its two sectors; inconsistent assignments return `JunctionRejected`.
  There is no tolerance weld or topmost-layer selection.
- Between successive sector heights, alternating occupied/empty sectors would
  produce four incident faces along a vertical edge. Such nonmanifold column
  configurations are rejected rather than accepted as a corner closure.
- Shared endpoint XZ positions must be exactly representable in the output float
  frame. Distinct incident height levels may not collapse to the same float.
  Existing per-boundary profile/completeness/precision validation remains active.
- Generated vertical wall edges are subdivided at all incident sector heights.
  A winding-preserving fan retains the old wall area and original source/request
  provenance. Generated wall normals/materials remain constant; UV stays equal
  to world tangent/height. Source tops and earlier trim/stitch mappings are retained.
- All limits remain active, including final triangle/byte checks after subdivision.
  Endpoint maps, repeated profiles and wall splitting are not hard time or aggregate
  working-memory bounds. The result remains a pure CPU value, not a dependency-
  stamped composed package or an installed rendering/query replacement.
- These checks concern endpoint column limits under the existing solid-column
  and replaceable-cap assumptions. They do not validate every surrounding surface,
  volume, cave, overhang, external boundary or arbitrary-world manifold condition.

## Verification

- Three new cases / 864 assertions cover T/four-arm edge subdivision with both
  positive- and negative-side ownership, exact opposite edge pairing, preserved
  area and top-triangle identity, culled front/back rays, material/UV retention,
  request-order geometry stability, and exact/one-short byte and triangle limits.
- Additional cases within those tests cover L/straight joins, equal-height sectors,
  walls tapering to a shared point, inconsistent missing-arm heights, nonmanifold
  saddles and unrepresentable endpoint positions. The old conflict test now retains
  overlap and unsplit-interior-contact rejection rather than rejecting all endpoints.
- The native Height/Hex fixture retains the disjoint 0..48 strips and adds complete
  X=64/Z=64 strips over 0..64. It uses the same 18 settled source meshes without
  any world update during extraction. Composed geometry is NOT displayed.
- Full-corner result: 118,099 triangles (306 walls), 354,297 vertices, 20 material
  ranges. All 306 wall-ray pairs and all 306 full source top-edge matches pass.
  The corner vertical edge has exactly two opposite traversals over the same
  endpoints. `height_hex_junction.csv` repeats across two native processes.
- GCC Release: 9/9 gates; 537 core cases / 470,639 assertions.
- MSVC AddressSanitizer: 6/6 gates; 487 core cases / 469,296 assertions.
  Existing 17 driver exclusions unchanged.
- WebGPU: 1/1 browser regression gate; native junction code is not compiled into
  that app. This does not validate browser rendering of the new geometry.
- All 342 prior BMPs and ten prior profile/wall/trim/stitch/package/composition
  evidence files are byte-identical. No new GPU seam or moving-camera acceptance
  is inferred from those unchanged images.
- Final Release galleries: policy `streaming-policy-captures/6ac027c78e05`, height
  `streaming-demo-captures/24fa920accda`, static `static-demo-captures/a4e625fe864f`,
  under `build/release/`, each with `index.html`.
- Assets untouched; TEMP/TMP stayed on D:. No new Linux/Clang, physical Metal,
  TSan, general volume-ownership or hard-budget acceptance.

## Next

Capture and retain the union of dependencies for composed packages, including
deduplicated source identity, revisions, versions and contributor membership.
Keep that freshness gate through worker construction and GPU staging, then swap
the complete rendering/query replacement together, retaining the prior version
on failure. Never overlay packages on their original source meshes.

Automatic boundary discovery/splitting/rebuild scheduling, global budgets, source
filtering, solid-volume ownership and in-world static/moving-camera seam acceptance
remain open. **M16 remains partial.**
