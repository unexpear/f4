# Engine Architecture Plan

Single source of truth for engine design decisions. Reference material: [awesome3dengine](https://github.com/jcoder58/awesome3dengine).

## 1. Goals & Constraints

- Build a 3D game engine **from scratch** in C++ — custom math library, no engine frameworks.
- Target platforms: **Windows + Linux**.
- Graphics backends: **OpenGL, Vulkan, DirectX 12** — all three, behind one abstraction.
- No deadline. Slow, correct foundation over fast results.
- The goal is **not** "make three APIs work." The goal is to build an **API-independent rendering model that happens to have three implementations.**
- **PRODUCT GOAL (primary):** this engine exists to ship real 3D games with
  **procedurally generated worlds** — Minecraft-style voxel terrain,
  Ark/Valheim-style streamed open worlds with biomes, and hex/grid-cell
  worlds. Every feature is judged by whether it serves building such games;
  the procgen track (M13+) is a peer of the rendering track, not an
  afterthought. Existing foundations already serve it directly: the
  deterministic RNG contract gives identical seeded worlds on every machine,
  chunk serialization gives world save/edit persistence, generational
  handles give stable chunk/entity ids, and the ECS carries world-scale
  entity counts.

## 2. Locked Decisions

| Decision | Choice | Notes |
|----------|--------|-------|
| Language | C++20 | Concepts/spans/constexpr; solid MSVC + GCC + Clang support |
| Platform layer | SDL2 | Window + input, wrapped in engine-owned abstraction |
| Testing | doctest | Header-only, fast compiles, fits template-heavy code |
| Shaders | Per-backend native | GLSL (GL), HLSL (DX12), GLSL→SPIR-V via glslang (Vulkan) |
| ECS | Data-oriented | No `Component` base class; components are data, systems operate on them |
| Component storage | `ComponentStorage<T>` sparse-set default | Interface swappable; semantics over layout |
| Render graph | Declarations-only version at M8 | Before Vulkan |
| Math | Fully custom | Vec/Mat/Quat written by hand, every op tested |

## 3. Architectural Corrections (adopted)

The plan deliberately avoids these traps:

1. **No inheritance-based `Component` interface.** No virtual base classes per component. Components are plain data in storage; systems are free functions or thin structs operating on batches of that data.
2. **Scene graph is NOT part of the ECS foundation.** ECS = entities + component storage + systems. Transform hierarchy = components (`Parent`, `LocalTransform`, world recomputation pass). Two separate concerns; hierarchy is *implemented with* ECS, never a second object model.
3. **Storage semantics before storage layout.** Engine code depends on `ComponentStorage<T>` behavior (add/remove/get/iterate), never on physical memory layout. Implementation may later become SoA, paged, chunked, or archetypal without rewriting systems.
4. **Math before ECS.** Fundamental types exist before transform-related components consume them.

### Amendments (decisions where this plan deviates from initial suggestions)

- **Initial storage impl: sparse sets**, not dense-array + hashmap. ~100 lines, O(1) add/remove/has, stable iteration order, production-proven (EnTT). Least likely to need replacing.
- **Command buffers from day one.** Engine interacts with the device ONLY by recording into `CommandBuffer` — even while the GL backend executes immediately. Retrofitting deferred recording onto an immediate-mode-shaped API is the classic trap. Backend selected at build/link time (one RHI implementation compiled per build) → virtual-call overhead irrelevant.
- **Render graph v1 = declarations only.** Passes + read/write declarations + resource sizes, executed in declaration order on GL. No auto-reordering/barrier synthesis until Vulkan demands it (M9) / scheduling optimization (M12).
- **M2 serialization trimmed.** POD/chunk save-load helpers only. Reflection-driven serialization waits until M5+ when resources need it.

## 4. Layer Architecture

Dependencies flow strictly downward.

```text
+------------------------------------------+
|              GAME / EDITOR               |
+------------------------------------------+
|          ENGINE SYSTEMS / ECS            |
+------------------------------------------+
|       ENGINE SERVICES / RESOURCES        |
+------------------------------------------+
|         RENDERING ABSTRACTION            |
+------------------------------------------+
|      PLATFORM + GRAPHICS BACKENDS        |
+------------------------------------------+
```

Renderer ↔ backends relationship (RHI):

```text
             Engine
                |
                v
       Graphics Abstraction (RHI)
                |
     +----------+-----------+
     v          v           v
   OpenGL     Vulkan      DX12
```

## 5. Folder Layout

```text
src/
├── core/                  # M0–M2: math, containers, memory, ids, logging
│   ├── math/
│   ├── containers/
│   ├── memory/
│   ├── ids/
│   └── logging/
├── ecs/                   # M3–M4
│   ├── entity/
│   ├── component/
│   ├── storage/
│   ├── query/
│   ├── world/
│   └── systems/
├── engine/                # M4–M5, M8
│   ├── scene/
│   ├── transform/
│   ├── resources/
│   ├── assets/
│   ├── cameras/
│   ├── lighting/
│   └── rendering/
├── graphics/              # M6: RHI — zero backend types leak upward
│   ├── device/
│   ├── buffer/
│   ├── texture/
│   ├── sampler/
│   ├── shader/
│   ├── pipeline/
│   ├── command/
│   ├── framebuffer/
│   └── render_graph/
├── platform/              # SDL2 wrapper (window, input, events)
└── backends/
    ├── opengl/            # M7
    ├── vulkan/            # M9
    └── dx12/              # M10
tests/                     # doctest suites, mirrors src/ structure
planning/                  # this folder
```

## 6. Conventions (locked once, documented forever)

- **Matrices:** column-major, right-handed world space.
- **Clip space:** depth range **0..1** (D3D-style). One projection matrix feeds all three backends; GL's native −1..1 handled inside the GL projection helper only.
- **Handles everywhere:** engine code never touches raw API objects or raw resource pointers — opaque handles only (`BufferHandle`, `TextureHandle`, ...).
- Generational IDs for entities and resources (stale-handle detection).
- Warnings-as-errors on all compilers; sanitizers (ASan/UBSan) run wherever the toolchain provides them: full on Linux/CI and on MSVC (via `EnableASAN` + test-time DLL path prepend). On Windows: MSYS2 GCC ships no sanitizer runtimes by upstream design; a stray `BuildTools` install without ASan libs is pinned around via `CMAKE_GENERATOR_INSTANCE`.

## 7. Shader Strategy (per-backend native)

Chosen trade-off: effects are maintained twice (GLSL + HLSL). Mitigations baked into milestones:

- Identical **vertex input layouts** and **uniform block structures** across backends, enforced via shared layout-constant headers per material.
- **Shader parity tests** from M8+: same scene through each backend must match a reference frame pixel-for-pixel.
- Escape hatch: migrating to HLSL+DXC single-source later touches only the asset pipeline — no engine changes, because M6 keeps shader sources as opaque blobs behind handles.
- Vulkan path: GLSL → SPIR-V via glslang at build time.

## 8. Development Roadmap

### Milestone 0 — Infrastructure
- `git init`; CMake presets (MSVC + GCC/Clang); warnings-as-errors
- Debug/Release configs; ASan/UBSan runs (availability auto-detected at configure time: full on Linux/CI/Clang64; skipped with warning on MSYS2 GCC, which ships no sanitizer runtimes)
- doctest wired up with one trivial passing test
- Logging + assert macros
- Folder skeleton per §5
- Optional GitHub Actions CI once a remote exists

### Milestone 1 — Math
- `Vec2/3/4`, `Mat3/4`, `Quaternion`
- Transform composition; AABB, Sphere, Plane, Frustum
- Numerical utilities (epsilon compare, deg/rad)
- Conventions doc (§6)
- **Every operation unit-tested**

### Milestone 1.5 — Quaternion Certification (DONE)
- `toQuat(Mat3)` hardened (Shepperd branches, sqrt args clamped for drifted
  inputs, sign contract documented) + `toQuat(Mat4)` overload (orthonormal
  upper-left block; translation ignored)
- `slerp` general branch now renormalizes output (endpoints land on a/b's
  rotation regardless of arc length; matches the nlerp fallback's behavior)
- Deterministic fixture generator (`tests/random.hpp`): seeded mt19937,
  canonical `u32>>8 / 2^24` float mapping - NO `uniform_real_distribution`
  (not portable across stdlibs). This generator is THE source of M8 parity
  fixtures: same seeds must feed CPU and GPU sides.
- Property battery (`test_quat_properties.cpp`, 15 cases / ~4600 assertions):
  composition homomorphism, norm preservation, normalization idempotence,
  inverse==conj (unit), slerp endpoints/constant angular velocity/shortest-path
  sign-fix, double cover, mat->quat->mat authoritative roundtrip,
  yaw*pitch*roll order pin, zero axis, gimbal +-90 deg stability, drift
  tolerance. Rotation-semantic assertions wherever double cover applies -
  NEVER componentwise quat equality across a possible sign flip.

### M8 carry-forwards (from M1/M1.5 lessons)
- kTightTolerance sits at the residue floor of exact-angle trig constants;
  chained rotations floor at kDefaultTolerance, never tighter
- Before any GPU-parity fixtures: reversed-Z decision, deep-hierarchy drift
  study, frustum adversarial tests, orthographic projection,
  plane/frustum reconstruction roundtrips
- kParityTolerance{1e-5,1e-4} reserved name; calibrate from measured deltas x4

### Milestone 2 — Memory & Containers
- Generational IDs, handles (`src/core/ids/`)
- Containers: dynamic array, string, hash map wrappers (`src/core/containers/`)
- Allocators: arena (linear), pool (fixed-chunk) (`src/core/memory/`)
- Resource handle types (tag-typed `Handle<Tag>`, concrete tags arrive M5/M6)
- POD/chunk serialization helpers only

Design decisions locked at M2 start:
- **Heap backing via one seam**: all dynamic memory flows through
  `f4::memory::heapAllocate/heapFree`, implemented with aligned
  `::operator new/delete` (portable GCC/MSVC/Clang). Future custom backing
  swaps exactly one place.
- **Containers are NOT allocator-templated yet.** Documented trade-off:
  allocator polymorphism deferred until a proven need (M12 job system /
  frame arenas); the single heap seam keeps that retrofit cheap.
- **Arena contract**: bump allocation, `reset()` between frames, NEVER runs
  destructors (placement-new'd types must be lifetime-trivial or externally
  torn down). Exhaustion returns nullptr - caller decides; sizing bugs are
  caught in tests, not hidden.
- **Pool contract**: fixed stride >= sizeof(void*) so the free list is
  intrusive; LIFO reuse; double-free is UB like operator delete (ASan
  presets are the net). TypedPool<T> adds ctor/dtor discipline on top.
- **Generational handles**: {u32 index, u32 generation}, null = index
  0xFFFFFFFF. GenerationalArena<T> proves stale-handle detection (ABA-safe)
  before M3 builds entity registries and ComponentStorage on it.
- **Serialization trimmed** (per §3 amendment): MemoryWriter/MemoryReader +
  chunk framing (magic/version/payload) over byte buffers; FILE IO plumbing
  defers to M5 resources.
- **String/StringView contracts**: StringView is strictly non-owning and
  read-only; caller guarantees the backing outlives it. Null view
  (data==nullptr) is distinct from empty view (size 0). String owns a heap
  buffer, always null-terminated for C interop; copy = deep copy, move =
  steal (source left valid-empty). NO small-string optimization - documented
  trade-off, revisit at M12 if profiling demands. Equal contents hash equal
  across String and StringView (shared FNV-1a).
- **HashMap contract**: thin engine API over std::unordered_map (per plan:
  "wrappers"). insert() never overwrites; set() upserts; find() returns
  pointer-or-null (matches GenerationalArena::get convention). Iteration
  order is UNSPECIFIED - any logic needing determinism must sort keys
  explicitly. Reference/pointer stability of values under rehash holds
  (node-based backing) and is tested. Handle<Tag> keys work via the
  std::hash specialization in handle.h.
- **Serialization contract**: byte image = raw in-memory representation
  copied verbatim (memcpy; NO alignment requirements on the stream). POD
  paths enforce std::is_trivially_copyable at compile time. Endian policy is
  EXPLICIT little-endian: streams begin with an endian marker that readers
  verify against std::endian::native - mismatched streams are rejected, not
  silently swapped. Chunk framing: {u32 id, u32 version, u32 payloadSize}
  header per chunk; reader validates declared size against remaining bytes
  and caller-provided capacity before copying.

### Milestone 3 — ECS
- Entity create/destroy with generational validation
- Component registration; `ComponentStorage<T>` (sparse set)
- add/remove/get; queries/views; system execution runner
- Entity lifetime stress tests

Design decisions locked at M3 start:
- **Entities are Handle<EntityTag>**: reuse the proven M2 handle type
  ({u32 index, u32 generation}, null = 0xFFFFFFFF). Registry owns allocation
  through a GenerationalArena of tiny slot records, so stale-entity rejection
  (ABA-safety), recycling and clear() semantics are inherited from tested
  code rather than reimplemented.
- **ComponentStorage<T> is a sparse set**: dense arrays (components +
  owning entities) plus a sparse index→dense-slot map keyed by entity INDEX.
  Swap-and-pop removal: component ORDER in dense storage changes on erase
  (documented; sort explicitly if order matters) and pointers/references into
  dense storage are invalidated by any removal. Lookup validates the FULL
  entity handle (index + generation match against the dense entities array),
  so a stale recycled id can never fetch a newer entity's component.
- **Registry type-erases storages** in a HashMap<std::type_index, Box<IStorage>>;
  storage<T>() creates on first use. destroy(entity) removes that entity from
  every registered storage before releasing the handle - components never
  outlive their entity.
- **View<Ts...> iterates the smallest storage** among the requested types and
  probes the others with O(1) contains checks. Iteration order follows the
  smallest storage's dense order (unspecified after removals).
- **SystemRunner is an ordered list** of named {name, fn(Registry&)} stages,
  executed in insertion order - deterministic by construction. No scheduling
  magic until M12 proves it necessary.
- All logic is platform-free and headless-testable across all six presets.

### Milestone 4 — Transform
- `LocalTransform` + parent links as components
- World-matrix recomputation pass, dirty propagation
- Reparent/orphan/cycle tests

### Milestone 5 — Resources
- Unified model: Mesh, Texture, Shader, Material, Pipeline, Buffer
- Central registry; opaque handles only; reflection-based serialization starts here if needed

### Milestone 6 — Graphics Abstraction (RHI)
- Device, Buffer, Texture, Sampler, Shader, PipelineState, CommandBuffer, Swapchain, Framebuffer
- Command-recording-first API (see §3 amendment)
- Zero OpenGL/Vulkan/DX12-specific types above this line

### Milestone 7 — OpenGL Backend
- Minimum to prove abstraction: window → device → shader → vertex/index buffers → pipeline → draw
- Triangle → cube → hierarchy of cubes
- SDL2 window/context integration

### Milestone 8 — Renderer + Render Graph v1
- Camera, Light, MeshRenderer components; Material concept
- RenderQueue sorting
- Render graph v1: declared passes (e.g., shadow → main), declared resource lifetimes/sizes, executed in declaration order
- Shader parity test harness introduced

### Milestone 9 — Vulkan Backend
- Same abstraction; render-graph metadata drives barriers/transitions/submission
- Expect and log every abstraction mistake — this milestone's purpose

### Milestone 10 — DX12 Backend
- Same abstraction; descriptor heaps mapped onto our binding model
- Validates that binding/descriptor design was API-neutral

### Milestone 11 — PBR
- BRDF (GGX), IBL, normal mapping, metallic/roughness workflow
- Shadow filtering; tone mapping
- Implemented natively in both GLSL and HLSL under §7 parity rules

### Milestone 12 — Optimization
- Frustum culling, occlusion culling
- Batching, instancing
- Job system, multithreaded command recording
- SIMD math paths
- Render graph scheduling, async compute

### Milestone 13 — Deterministic Noise Library (procgen track)
- Value/Perlin/Simplex noise, FBM, domain warping — every function takes an
  explicit seed; no hidden global state
- Extends the M1.5 RNG contract: identical seed → identical world bytes on
  every toolchain and backend (CPU reference vs GPU-sampled noise parity is
  an M11+ test under the §7 rules)
- Pure CPU + headless-testable; can execute immediately after M4

### Milestone 14 — World Data Model (procgen track)
- **All three cell styles are first-class by design decision** (voxel blocks,
  height tiles, hex cells) — none is a retrofit
- Full technical specification: **§11** (chunk frame, storage layouts,
  coordinate math, concurrency, float-parity rules)
- Chunked world storage keyed by Handle<Tag> chunk ids. The chunk is the
  uniform unit (id, seed-derived generation, edit deltas, dirty flags,
  streaming state); the CELL LAYOUT is a chunk-level policy, not a virtual
  per-cell interface: one compile-time CellPolicy per world kind keeps hot
  loops monomorphic, honoring the §3 "semantics over layout, no per-object
  inheritance" rule at world scale
  - VoxelPolicy: dense block-id array (u16 palette) per chunk
  - HeightPolicy: heightfield samples + biome/material layers per tile
  - HexPolicy: axial-coordinate cells with explicit neighbor topology
- Generation pipeline: seed → chunk cells via M13 noise; player edits stored
  as sparse deltas over the generated base
- Save/load of edits through the M2 chunk serialization format (endian
  marker + versioned chunks already proven)

### Milestone 15 — Terrain Meshing (procgen track)
- One mesher per cell policy, all emitting the SAME MeshData
  {vertices, indices} record consumed uniformly by the RHI:
  - Voxel: culled + greedy mesher, per-face texturing, baked ambient
    occlusion, DDA-ready geometry
  - Height: skirt-stitched tessellated tiles with biome splatting
  - Hex: prism meshes from cell topology, per-face coloring
- Border stitching between neighboring chunks within a policy; remesh on
  edit with dirty flags from M14
- Executes as soon as M7 proves window→device→draw; headless mesher tests
  run earlier against golden vertex dumps

### Milestone 16 — World Streaming (procgen track)
- Ring load/unload of chunks around the camera on worker threads — pulls the
  job system forward from M12 into first-class status
- Priority queues by distance, per-frame time budget so spikes never stall
  the frame, generation staged: layout → cells → meshes
- Collision hooks for terrain (voxel DDA raycast for the Minecraft case,
  heightfield sampling for the Ark/Valheim case)

Recommended interleave: M13 right after M4 (no renderer required), M14 after
M5 (resources), M15 as soon as M7 draws its first mesh, M16 alongside M12.
The renderer track stays exactly as written above.

## 9. Key References (from awesome3dengine)

| Topic | Resource |
|-------|----------|
| Overall architecture | *Game Engine Architecture* — Jason Gregory (Ch. 3–6, 8) |
| Renderer design | "Designing a Modern Rendering Engine" thesis; Data Driven Renderer PDF; Modern Renderer Design thread |
| Data-oriented rendering | "ECS and Rendering" (Our Machinery); Data Driven Rendering (Sassone) |
| PBR | Allegorithmic PBR Guide; UE4 PBR docs; Moving Frostbite to PBR |
| Lighting/pipelines | Forward+ paper; Forward vs Deferred vs Forward+ (3DGEP); 2048 point lights @60fps |
| Vulkan | Writing an Efficient Vulkan Renderer (zeux.io); BansheeEngine Vulkan blog/code |
| Optimization | Fast Hierarchical Culling (Cesium); Frostbite compute pipeline; Agner's optimization resources |
| Physics (later) | Introduction to Networked Physics (Gaffer on Games); Unified Particle Physics preprint |
| Code study | BansheeEngine (C++14/Vulkan), Tiny Renderers, EntityX |
## 10. Immediate Next Step

Milestone 4 (Transform) is complete and verified (155 cases / 30,781
assertions, all six presets green): LocalTransform/Parent/WorldMatrix
components on the M3 ECS, dirty-tag propagation with inherited-dirty DFS,
world-jumping trySetParent with cycle rejection, dead-parent pruning on the
next pass (orphans become roots), and an 8000-step mirror-verified churn
stress test.

Next: Milestone 13 (Deterministic Noise) under §11.6's float-parity rules,
in the hardened order - land -ffp-contract=off // /fp:precise FIRST, rebuild
all six presets green, and only then generate/freeze bit-exact golden
vectors for value/perlin (2D+3D), fbm, domain warp, simplex2D. No simplex3D.
After M13: M5 (Resources) on the renderer track.

## 11. Procgen Deep Dive: Chunk Frame & Cell Policies

Locked technical specification behind M14/M15. Numbers are initial values —
chosen simple and safe; each carries a documented upgrade path.

### 11.1 Common chunk frame (all policies)

- **Identity**: every chunk is a `Handle<ChunkTag>` (M2 machinery) plus a
  small POD `ChunkCoord{int32 x, y, z}` hashed into the resident-chunk map
  (`std::hash` specialization ships with it so M2's HashMap accepts it).
  world-space origin = coord * chunkExtent(policy-specific).
- **Lifecycle** (state byte written only by the main thread):
  `Invalid → Queued → LayoutDone → CellsReady → Meshed`, plus dirty bits
  (`cellsDirty`, `meshDirty`, `bordersDirty`). Workers advance stages; they
  never mutate shared buffers in place (see 11.5).
- **Purity rule**: layout AND cells are pure functions of
  `(worldSeed, coord, policyParams)`. Player edits live ONLY in a sparse
  delta overlay:
  `HashMap<u32 cellIndex, CellValue> deltas;`
  Regeneration + delta replay reproduces the chunk bit-exactly. Saves
  persist just the deltas through M2 chunk framing (id + version) — an
  untouched chunk costs zero save bytes.
- **Policy selection is regional**: a RegionMap divides the world into large
  tiles (default 512 m) each naming ONE active policy. A Valheim-style world
  becomes "heightfield everywhere except these cave regions which are
  voxel" with no special-casing downstream — everything consumes the
  policy-neutral chunk interface (`cells()`, `markDirty()`, `buildMesh()`).

### 11.2 VoxelPolicy

- **Geometry**: 32×32×32 cells (locked). Linearization locked as
  `index = x | (z << 5) | (y << 10)` — horizontal slices contiguous, so XZ
  scans and greedy-mesher sweeps stay cache-friendly; vertical runs stride
  1024.
- **Storage v1**: flat `Array<u16>` = 64 KiB/chunk. Air = 0 (memset-able;
  "trivially empty" check = scan first words for all-zero).
- **Storage v2 (upgrade path, M16)**: per-chunk palette + bit-packed
  indices — ≤256 distinct blocks pack to 8-bit indices (32 KiB), typical
  terrain palettes ≤16 blocks pack to 4-bit (16 KiB). Palette lives in the
  chunk header; serialization versioning already allows both layouts.
- **Block registry**: u16 id → `{solid, opaque, textureLayer, hardness}`.
  ids 0..255 engine-reserved (0 air, 1 bedrock, …); games own ≥256.
- **Meshing input**: padded fetch window of 34³ built once per remesh from
  the chunk plus the 6 face neighbors' 1-cell edge slabs. Face adjacency is
  sufficient for face culling; corners become relevant only when baked AO
  arrives (noted for M15 — window grows to neighbor corners then).
- **Raycast**: Amanatides–Woo DDA over the cell grid; returns
  `{cell, enteringFaceNormal, t}`. The world-level raycast walks chunk
  boundaries transparently.
- **Remesh granularity v1**: whole chunk. Upgrade path: eight 16³ sections
  with per-section dirty bits (documented now, built when profiling asks).
- **Memory envelope**: at radius-8 view distance (~2200 resident chunks)
  v1 storage ≈ 140 MB worst-case full chunks, far less with emptiness
  checks; the palette path halves or quarters that. Radius-16 is what makes
  v2 mandatory rather than optional.

### 11.3 HeightPolicy

- **Tile**: 65×65 samples (64 m quads at 1 m spacing) — the extra shared
  edge row/col overlaps neighbors so stitching never interpolates across a
  gap. Heights are f32 meters in v1.
- **Layers per sample**: height f32 + biomeId u16 (index into a global
  BiomeTable) + 4× u8 material weights normalized to Σ=255 (splat map).
- **Normals**: central differences; edge samples read NEIGHBOR TILES' rows
  so lighting never reveals tile seams.
- **Sampling contract**: `worldXZ → bilinear height/biome`. Gameplay
  (collision, spawning) uses the SAME sampler as rendering — one source of
  truth, divergence impossible.
- **LOD**: fixed 2-triangle quads v1, CDLOD-style skirts to hide level
  cracks (M15); no dynamic tessellation.
- **Overhangs/caves**: impossible inside a height tile BY DESIGN — such
  features are voxel-policy regions via the RegionMap (11.1). At a
  height↔voxel region border a skirt wall drops to a constant skirt floor.
- **Edits**: sparse deltas `{sampleIndex → newHeight/newWeights}`;
  flatten/smooth/ramp tools go through the same mechanism as gameplay
  edits.

### 11.4 HexPolicy

- **Topology**: pointy-top AXIAL coordinates (q, r). Locked neighbor table:
  `(+1,0) (+1,-1) (0,-1) (-1,0) (-1,+1) (0,+1)`;
  `distance(a,b) = (|dq| + |dr| + |dq+dr|) / 2`.
- **Storage**: rectangular dense backing, `w = mapWidthInHexes`:
  ```
  col  = q + floorDiv(r, 2)
  row  = r
  index = row * w + col        // floordiv, NOT trunc: C++ `/` truncates
                               // toward zero and breaks negative rows;
                               // M14 ships one shared floorDiv/mod pair
  ```
  Cells outside the hex-shaped region hold `kInvalidCell`. Irregular island
  maps may instead back onto `HashMap<AxialCoord, Cell>` behind the same
  interface.
- **Elevation is stored PER CORNER, not per cell**: a corner lattice where
  every hex shares its six corners with its neighbors guarantees watertight
  prisms — no T-junction cracks, ever. Corner heights come from M13 noise
  sampled at corner world positions; a cell's center height derives from
  its corners.
- **Content per cell**: terrainType u8 + owner slot (strategy-game state:
  entity Handle or faction id) + implicit corner references via lattice
  position.
- **Queries shipped with M14**: ring/spiral iteration (walk the direction
  table), hex line-draw (lerp + axial round); obstacle-aware range comes
  later with gameplay.

### 11.5 Concurrency model (all policies)

- Cell buffers are IMMUTABLE once published (`CellsReady`). Edits append to
  the delta overlay under a per-chunk lock and bump a version counter;
  remesh workers snapshot `(base, deltas@version)` so a running mesher never
  observes torn state. The main thread swaps in finished meshes.
- Until M16's job system exists, the same state machine runs synchronously
  on the main thread — swapping workers in later touches scheduling only,
  not the data model.

### 11.6 Determinism & float parity

M13 noise must produce BIT-IDENTICAL output across GCC/MSVC/Clang and
eventually GPU samplers (extends the M1.5 contract from fixtures to worlds).
Two rules make that achievable:

1. **Compile discipline**: `-ffp-contract=off` (GCC/Clang) and `/fp:precise`
   (MSVC) on ALL presets — FMA contraction differences are the #1 silent
   cross-toolchain divergence. Added to CompilerWarnings.cmake when M13
   lands.
2. **Kernel discipline**: hot-path noise avoids transcendental functions;
   gradient/permutation tables come from integer hashes, not sin/cos. Where
   exactness vs GPU sampling is impossible, comparison tolerance is
   calibrated exactly like M8: measured delta × 4, recorded in the test.

### 11.7 M14 delivery order

1. `floorDiv/floorMod` helpers, `ChunkCoord`, hashing
2. Chunk frame: lifecycle state machine + delta overlay + purity-based
   regeneration
3. VoxelPolicy end-to-end: placeholder-noise gen → edits → serialize →
   reload-equality test
4. HexPolicy topology + corner lattice
5. HeightPolicy tiles + shared-edge normals

(Order starts with the most demanding future consumer of M15 meshing.)
