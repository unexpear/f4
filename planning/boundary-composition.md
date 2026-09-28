# Disjoint Shared-Source Boundary Composition (2026-09-07)

Follow-up: [endpoint junctions](boundary-junctions.md) now support compatible
touching endpoints with sector validation and vertical wall-edge subdivision.
The historical disjoint fixture and evidence below remain unchanged. Combined
dependency capture and coordinated GPU/query publication are still open.

## Findings And Plan

The single-boundary builder includes full contributing source meshes. Concatenating
two packages that share a source would duplicate it, and independently replacing
one package with another would discard earlier cuts. This increment composes
disjoint boundary strips before publishing anything. **M16 remains partial.**

Inspected the existing profile, trim, stitch, wall, replacement and query code.
The implementation reuses the single-boundary pipeline, mapping its triangle ids
back through each source's previous result instead of starting from originals.
Working mesh storage has fixed size while borrowed views are in use; replacement
happens only after the returned provenance has been consumed. This avoids vector
reallocation invalidating borrowed mesh pointers.
[Microsoft vector documentation](https://learn.microsoft.com/en-us/cpp/standard-library/vector-class?view=msvc-170).

CPU edge identity does not by itself guarantee matching GPU positions across
different programs. Shader input/computation invariance and actual rendered seam
acceptance remain separate work.
[GLSL invariance requirements](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html#variance-and-the-invariant-qualifier).

## Changes And Contract

- `composeTerrainBoundaryReplacements` takes one shared source table and requests
  whose negative/positive lists refer to that table. Every source must be used.
  Null sources, repeated request membership and duplicate mesh-pointer/origin
  identities are rejected. Callers remain responsible for unique logical sources;
  geometrically identical copies are not deduplicated by position.
- Closed request segments must be disjoint in XZ. Duplicate, overlapping,
  crossing and endpoint-touching segments return `ConflictingBoundaries`.
  Corner/junction closure is deliberately not inferred or approximated.
- Requests run in canonical axis/plane/begin/end order. Each pass works on the
  previous source result, preserving original triangle provenance through all
  trimming and stitching. Generated walls stay outside subsequent source trims.
- Final output contains each modified source once, followed by uniquely requested
  walls, in one zero-origin world-space mesh. Sources retain their attributes;
  material ranges never cross source identities. The caller resolves each source's
  policy-local material ids and suppresses every represented original source.
- Wall provenance includes its input request index, side and higher-side original
  source triangle. Source triangles use a sentinel request index, with no boundary
  side assignment. Request permutations preserve geometry; wall request indices
  intentionally follow the caller's reordered input, not canonical numbering.
- Defaults allow 64 requests (configurable 1..256), with the existing source,
  triangle and byte limits. Limits apply to the final logical payload and each
  single-boundary intermediate. Lower-level trim/stitch/wall limits still apply.
  They are not aggregate working-memory or execution-time bounds. No new allocator
  recovery, scheduling, dependency capture or publication behavior is added.
- The pure composer retains the existing solid-column/replaceable-cap assumptions
  and exact common-frame float conversion. It is not a generic mesh boolean or a
  solution for cave/overhang ownership.

## Verification

- Three new tests / 878 assertions cover a source shared by opposite boundaries,
  successive splits of the same original triangle, retained untouched cap strips,
  original provenance, no duplicate central top area, exact complete top-edge
  matches, wall/top rays, translated frames, request-order geometry stability,
  invalid source identity, unsupported junctions, incomplete coverage and limits.
- The native audit composes X=64 and Z=64 strips over tangent 0..48, leaving 16 m
  before their shared corner. Its table includes all 18 settled Height/Hex sources,
  including noncontributing mesh portions. No world update occurs during the audit.
  This is a borrowed-source CPU fixture, not a combined dependency-stamped package.
- Result: 118,068 triangles, including 228 walls; 354,204 vertices; 20 material
  ranges. All 228 culled wall-ray pairs retain original boundary/source identity;
  all 228 complete source top-edge matches survive both operations.
- `height_hex_composition.csv` repeats exactly across the native gate's two
  processes, including explicit render/metadata hashes (no struct padding).
- GCC Release: 9/9 gates; 534 core cases / 469,776 assertions.
- MSVC AddressSanitizer: 6/6 gates; 484 core cases / 468,433 assertions.
  Existing 17 driver exclusions unchanged.
- WebGPU: 1/1 browser regression check; the native module is not compiled
  into that app and the browser check does not validate composition rendering.
- All 342 previous BMPs and nine prior profile/wall/trim/stitch/replacement evidence
  files are byte-identical. Composed geometry is NOT installed in those screenshots.
- Final Release galleries: policy `streaming-policy-captures/b7b9343deeab`, height
  `streaming-demo-captures/f4784ac31ba1`, static `static-demo-captures/94ef6fbedd11`,
  under `build/release/`. Each contains `index.html`.
- Assets untouched; build/test TEMP/TMP stayed on D:. No new Linux/Clang, physical
  Metal, TSan, hard-budget or moving-camera transition acceptance.

## Next

Resolve touching boundary junctions, including compatible vertical wall edges at
corners and height crossings, without losing earlier source cuts. Capture the
union of source dependencies for composed packages and retain freshness checks
through construction, staging and installation. Then stage all GPU resources and
swap rendering/query geometry together, retaining the old version on failure.

Automatic discovery/rebuild scheduling, source filtering, global budgets, volume
ownership and static/moving-camera rendered transition acceptance remain open.
Do not install independent single-boundary packages over shared originals or
treat this disjoint-strip primitive as complete multi-boundary publication.
