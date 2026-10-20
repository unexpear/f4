# Engine Architecture Plan

Single source of truth for engine design decisions. Reference material: [awesome3dengine](https://github.com/jcoder58/awesome3dengine).

## 1. Goals & Constraints

- Build a 3D game engine **from scratch** in C++ — custom math library, no engine frameworks.
- Target platforms: **Windows + Linux + macOS + Web (WebAssembly)**. Windows,
  Linux, and Web are implemented; macOS is the remaining expansion target.
- Graphics backends: **OpenGL, Vulkan, native DirectX 12, native Metal, and
  browser WebGPU** behind one abstraction. DirectX 12 must remain direct
  D3D12/DXGI, Metal must not route through MoltenVK, and WebGPU must not route
  through WebGL.
- No deadline. Slow, correct foundation over fast results.
- The goal is **not** "make five APIs work." The goal is to build an
  **API-independent rendering model with conforming native implementations.**
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
| Shaders | Per-backend native | GLSL (GL), HLSL/DXIL (DX12), GLSL→SPIR-V via glslang (Vulkan), MSL/metallib (Metal), WGSL (WebGPU) |
| ECS | Data-oriented | No `Component` base class; components are data, systems operate on them |
| Component storage | `ComponentStorage<T>` sparse-set default | Interface swappable; semantics over layout |
| Render graph | Declarations-only version at M8 | Before Vulkan |
| Math | Fully custom | Vec/Mat/Quat written by hand, every op tested |
| Asset formats | **Full-spectrum ladder (§12)** | BMP/TGA/OBJ hand-written seed codecs, then every major format: stb_image, glTF/GLB, OpenEXR, DDS+BCn, KTX2+BasisU, ASTC, FBX, USD, Alembic — renderer track paused until complete |
| Third-party deps | Pinned FetchContent | SYSTEM includes (warning-isolated from -Werror), license recorded next to each pin, six-preset gate still applies |

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
   +-------+-------+-------+-------+
   v       v       v       v       v
 OpenGL  Vulkan   DX12   Metal  WebGPU
```

## 5. Folder Layout

```text
src/
├── core/                  # M0–M2, M13: math, containers, memory, ids, logging, noise
│   ├── math/
│   ├── containers/
│   ├── memory/
│   ├── ids/
│   ├── logging/
│   ├── noise/
│   └── resources/         # M5+: resource cache, asset records, format codecs (headless)
├── ecs/                   # (empty scaffold — ECS actually lives in src/core/ecs,
│                          #  per M3 precedent that headless layers ship under core/)
├── engine/                # M4–M5, M8
│   ├── scene/
│   ├── transform/
│   ├── assets/            # high-level asset pipeline sits here once it exists;
│                          # low-level cache/codecs are core/resources
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
    ├── dx12/              # M10
    ├── metal/             # M17
    └── webgpu/            # M18
tests/                     # doctest suites, mirrors src/ structure
planning/                  # this folder
```

## 6. Conventions (locked once, documented forever)

- **Matrices:** column-major, right-handed world space.
- **Clip space:** depth range **0..1** (D3D-style), forward-Z (near 0, far 1).
  One projection matrix feeds every backend; OpenGL requires
  `GL_ARB_clip_control` and selects `GL_ZERO_TO_ONE` when its device is created.
  Reversed-Z is deferred until clear values and depth comparisons can change
  atomically on every backend.
- **Handles everywhere:** engine code never touches raw API objects or raw resource pointers — opaque handles only (`BufferHandle`, `TextureHandle`, ...).
- Generational IDs for entities and resources (stale-handle detection).
- Warnings-as-errors on configured compilers; sanitizers (ASan/UBSan) run wherever the toolchain provides them. MSVC ASan is active via `EnableASAN` plus a test-time DLL path prepend. MSYS2 GCC ships no sanitizer runtimes, so that preset builds without sanitizers. Linux/Clang debug and ASan+UBSan configure-build-test gates are defined in CMake presets and GitHub Actions; both were also verified locally with Clang 18 on Ubuntu 24.04.

## 7. Shader Strategy (per-backend native)

Chosen trade-off: effects use native backend languages. The implemented desktop
set is maintained in GLSL + HLSL; M17 adds MSL and M18 adds WGSL. Mitigations
baked into milestones:

- Identical **vertex input layouts** and **uniform block structures** across backends, enforced via shared layout-constant headers per material.
- **Shader parity infrastructure** starts in M8. Each real backend must add the
  same seeded float probes and reference-scene captures when it lands. OpenGL,
  Vulkan, and DX12 parity is established; M17 and M18 own Metal and WebGPU
  admission to that conformance set.
- Escape hatch: migrating to HLSL+DXC single-source later touches only the asset pipeline — no engine changes, because M6 keeps shader sources as opaque blobs behind handles.
- Vulkan path: GLSL → SPIR-V via glslang at build time.

The M13 hash's paired-`u32` implementation is the canonical cross-backend
algorithm, not a workaround for a 32-bit host. Host/process bitness does not
define shader-language integer support: OpenGL exposes 64-bit shader integers
through `GL_ARB_gpu_shader_int64`, Vulkan gates them behind the optional
`shaderInt64` feature, HLSL adds them at Shader Model 6, Metal support varies by
GPU family, and WGSL has only concrete `i32`/`u32` runtime integer scalars.
Backends may add a native-`u64` fast path only after it produces the same locked
vectors and is measurably beneficial; the paired-`u32` path remains required so
parity does not depend on an optional capability.

## 8. Development Roadmap

### Implementation audit (2026-09-03)

Status is based on code and tests in the working tree, not on historical status
notes later in this document.

Status meanings are strict: ✅ means the written milestone acceptance is
implemented and tested; ⚠ means useful implementation exists but at least one
written acceptance gate is still open; ❌ means no milestone implementation.

| Milestone | Status | Implemented evidence / remaining gap |
|-----------|--------|--------------------------------------|
| M0 Infrastructure | ✅ Complete | Six Windows presets exercise UCRT64 GCC and MSVC; Linux Clang debug and ASan+UBSan presets are CI gates. Warnings-as-errors, doctest, and an explicit determinism label run across the applicable matrix. The Linux OpenGL build and tests also run under Xvfb. |
| M1 Math | ✅ Complete | Vector/matrix/quaternion, transforms, bounds, planes, frusta, projections, and direct unit/property coverage are present. No numeric coverage percentage is claimed. |
| M1.5 Quaternion certification | ✅ Complete | Hardened conversions/slerp and deterministic property fixtures are present. Its M8 carry-forwards are closed: orthographic projection, a locked forward-Z policy, 512-level hierarchy drift calibration, adversarial zero-to-one frustum coverage, and plane/frustum reconstruction roundtrips. |
| M2 Memory & containers | ✅ Complete | Arena/pool, engine `Array`/`String`, generational handles, and chunk serialization are implemented and tested. The heap seam covers engine-owned raw/contiguous storage; `std::unordered_map`, `std::function`, smart-pointer control objects, and third-party internals remain explicit standard-library allocation exceptions. |
| M3 ECS | ✅ Complete | Registry, sparse component storage, views, ordered systems, stale-handle checks, and lifetime stress tests are present. |
| M4 Transform | ✅ Complete | ECS local/parent/world transforms, dirty propagation, reparent/orphan/cycle handling, and churn tests are present. |
| M5 Resources & asset IO | ✅ Complete | M5.0–M5.9 records, cache, typed `getOrLoad`, file/container IO, scoped format loaders, and tests exist. `THIRD_PARTY_NOTICES.md` records every FetchContent dependency's declared ref, resolved commit, upstream source, and applicable license text/notices. |
| M6 Graphics abstraction | ✅ Complete | API-neutral handles/descriptors, graphics and compute command recording, `IDevice`, NullDevice, validation, texture uploads, storage-buffer readback, and headless tests are present. |
| M7 OpenGL backend | ✅ Complete | SDL/OpenGL rendering, resources, framebuffers, draw/instancing, zero-to-one clipping, diagnostics, and pixel tests work. Scissor/depth-clamp state, independent attachment blend/write state, clear-mask handling, depth-only/depth-stencil framebuffers, and device-limit/layout validation now conform to the advertised RHI and have focused tests. |
| M8 Renderer + render graph v1 | ✅ Complete | `Renderer::render()` executes through the graph and graph lifetimes/rollback are covered. The constant ring grows geometrically instead of silently stopping at 64 slots; direct 130-slot and 65-object renderer regressions verify the former boundary. Cross-backend parity remains an M9/M10 acceptance item rather than an M8 requirement. |
| M9 Vulkan | ✅ Complete | Vulkan 1.3 now covers SDL surface/presentation, resources, descriptors, build-time SPIR-V, dynamic-rendering pipelines, synchronization2 barriers, fenced frames in flight, swapchain recreation, and deferred retirement. Windows and Linux validation/synchronization-validation runs are clean, and OpenGL/Vulkan float and RGBA8 parity tests pass. |
| M10 DX12 | ✅ Complete | Native D3D12 covers hardware/WARP device selection, queues and fenced frames in flight, resources, barriers, render targets, root signatures, dynamically sized descriptor heaps, DXC-built Shader Model 6 pipelines, deferred retirement, and SDL/DXGI presentation/recreation. Focused pixel, stability, 130-draw binding, swapchain, and seeded parity tests pass alongside the full suite. |
| M11 PBR | ✅ Complete | Engine materials implement glTF metallic-roughness factors and texture slots, strict glTF-to-renderer mapping, GGX direct lighting, equirectangular mip-chain IBL, derivative normal mapping, filtered directional shadows, ACES tone mapping, alpha modes, and unlit materials. Native OpenGL GLSL, Vulkan SPIR-V, and DX12 HLSL/DXIL paths pass full-frame parity and clean validation gates, including position-only mesh fallbacks. Clustered forward point lights (hundreds to thousands per frame) now shade on OpenGL/Vulkan/DX12/WebGPU with GL/VK/DX image parity; Metal point lights are written but unverified. |
| M12 Optimization | ⚠ Partial | The renderer now computes authoritative mesh bounds, rejects camera-frustum misses, sorts a deterministic complete opaque batch key, and collapses compatible opaque items into native instanced draws on OpenGL, Vulkan, and DX12. Transparent items remain depth-sorted and unbatched. M16's distance-priority job system is complete; occlusion culling, multithreaded recording, SIMD paths, graph scheduling, and async compute remain open. |
| M13 Deterministic noise | ✅ Complete | Complete for OpenGL/Vulkan/DX12 and the current WebGPU backend: seeded value/perlin/simplex, FBM, and domain warp APIs retain bit-exact GCC/MSVC/Clang CPU vectors. API-neutral compute pipelines, dispatch, synchronization, storage bindings, and readback run equivalent GLSL/SPIR-V, HLSL/DXIL, and paired-`u32` WGSL kernels. Every implemented CPU/GPU probe passes the centralized float tolerance with at least a 4x margin. Metal parity remains an M17 admission gate. |
| M14 World data model | ✅ Complete | Handle-keyed chunks, negative-safe coordinates, regional policy selection, synchronized lifecycle/version snapshots, sparse deterministic delta saves, and monomorphic voxel/height/hex policies implement §11. Golden generation and persistence digests match GCC, MSVC, and Clang; focused topology, seam, DDA, stale-work, corruption, and concurrency tests pass under Clang ASan+UBSan. |
| M15 Terrain meshing | ✅ Complete | All policies emit one `MeshData` record and convert through the canonical M11 mesh/RHI path. Voxel greedy culling includes material ranges, a 34³ border, baked AO, AO-aware diagonals, and deterministic transparent-interface ownership; height tiles include neighbor-fed normals, fixed tessellation, four-way biome color splatting, skirts, and a transition floor; hex tiles emit colored closed prisms from the shared corner lattice. Version-safe snapshot builds, live-neighbor border invalidation/mirroring, cross-toolchain golden dumps, and a visually captured gallery are present. |
| M16 World streaming | ✅ Complete | Policy-aware camera rings use distance/stage-priority workers and bounded main-thread topology/publication. Versioned layout/cell/mesh results reject unloaded or edited work; sparse edits survive unload, including absent shared height neighbors. Resident voxel DDA and height sampling provide collision hooks. M16 also closes the deferred shared-resource reference contract and stores normal voxel bases in deterministic 4-bit/8-bit palettes. |
| M17 Native Metal | ⚠ Partial | The direct Objective-C++ Metal device, SDL `CAMetalLayer`, queue/replay, resources, pipelines, metallib build rules, presentation, readback, retirement, M11/M13 shader families, 130-draw regression, locked M11 image gate, and nine-scene repeated capture runner are implemented. Hosted Apple Clang Debug/Release builds and CPU/build conformance pass, but M17 cannot be accepted until Metal-capable Apple hardware passes diagnostics, parity, presentation/resize, and visual review of all 27 captures. |
| M18 Browser WebGPU | ✅ Complete | Emscripten 6.0.9 and Emdawnwebgpu provide a browser-native WebGPU RHI with asynchronous setup/pipeline admission, device-loss reporting, negotiated limits/features/formats, canvas resize/presentation, complete resource/binding/render/compute command paths, readback, and deferred reclamation. Native WGSL covers M11, M13, Cornell, all six focused rendering validations, and terrain. Chromium gates zero errors, 130 draws, indexed-strip format conformance, M13 CPU/GPU parity, byte-exact M11 full-frame parity, resize stability, and three identical captures from each of the seven validation scenes plus terrain; every capture was visually inspected. |

### Milestone 0 — Infrastructure ✅
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
- Public operations receive direct unit/property coverage; do not claim a
  coverage percentage until coverage instrumentation is part of the gate

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
- ✅ Forward-Z is locked for the shared zero-to-one projection convention;
  reversed-Z remains a future synchronized backend policy change
- ✅ A deterministic 512-level hierarchy study measured `1.528e-5` maximum
  absolute drift on GCC and MSVC; the regression ceiling is `6.2e-5`
- ✅ Orthographic projection, all-six-plane adversarial frustum cases, and
  plane/intersection/frustum-corner reconstruction roundtrips are covered
- ✅ `kParityTolerance{1e-5,1e-4}` is centralized. The seeded OpenGL compute
  probe measured `5.96046e-8` maximum absolute error and `1.1761e-7` maximum
  relative error; the absolute tolerance exceeds the measured delta by more
  than the required 4x margin

### Milestone 2 — Memory & Containers
- Generational IDs, handles (`src/core/ids/`)
- Containers: dynamic array, string, hash map wrappers (`src/core/containers/`)
- Allocators: arena (linear), pool (fixed-chunk) (`src/core/memory/`)
- Resource handle types (tag-typed `Handle<Tag>`, concrete tags arrive M5/M6)
- POD/chunk serialization helpers only

Design decisions locked at M2 start:
- **Heap backing seam**: engine-owned raw/contiguous storage (`Array`, `String`,
  arenas, pools) flows through `f4::memory::heapAllocate/heapFree`, implemented
  with aligned `::operator new/delete`. `HashMap`'s `std::unordered_map`,
  `std::function`, smart-pointer internals, diagnostic strings/streams, and
  third-party libraries allocate through the standard library. A future custom
  allocator therefore requires allocator-aware replacements/adapters for those
  explicit exceptions; it is not a one-function global swap.
- **Containers are NOT allocator-templated yet.** Allocator polymorphism and
  interception of the exceptions above are deferred until profiling proves a
  need (M12 job system / frame arenas).
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

### Milestone 5 — Resources & Asset IO (expanded 2026-08-25) ✅

Core layer first, then the full format ladder (§12) as sub-milestones
M5.1–M5.9. **Renderer track (M6+) pauses until the ladder completes** —
explicit product call: no rush to rendering.

M5.0 core (ships before any codec):
- `src/core/resources/` — headless, six-preset testable like core/ecs, core/noise
- Unified model: Mesh, Texture, Shader, Buffer, Pipeline (Material defers to M8)
- Typed handles (`Handle<MeshTag>` …) over per-type `GenerationalArena<T>`
  inside one type-erased `ResourceCache` — stale-handle safety inherited from
  M2, mirroring how M3's Registry reuses it
- Path lookup via StringHash; `find(path)` handle-or-null + typed `getOrLoad`
- File↔buffer binary IO (the M2 deferral lands here); `.f4asset` container
  reusing M2 chunk framing {id, version, payloadSize}
- Mesh record: flexible vertex-layout streams, u16/u32 index type, submeshes,
  AABB bounds computed at load
- Texture record: format enum incl. block-compressed (BCn/ASTC), mip chain,
  decoded-RGBA vs GPU-native payload distinction ("support" ≠ "decode")
- Buffer/Pipeline: desc structs only — device owns instances from M6
- Lifetime: M16 adds typed retain/release counts; repeated `getOrLoad` calls
  acquire shared ownership and final release removes the record/path while
  preserving stale-handle safety. `destroy` remains a release-compatible name.
- Reflection-based serialization NOT started here — M14 saves keep using
  explicit chunk writes; reflection waits for editor era

Sub-milestones M5.1–M5.9: see §12 ladder table with per-rung strategy.
M5 codec closeout completed 2026-09-02: cache load-on-miss preserves loader errors;
glTF primitives now carry submesh/material mappings and core metallic-roughness
material records, and accessor conversion handles normalized/strided data.
The format implementations are complete for their documented subsets.
`THIRD_PARTY_NOTICES.md` closes the parent process gate by recording every
FetchContent dependency's declared ref, resolved commit, upstream source, and
applicable license text/notices.

Design decisions locked at M5 start:
- **Loader seam**: every format is a free function
  `(Span<const u8> bytes) → Result<Record, Error>`; callers provide a typed
  loader callback to `ResourceCache::getOrLoad`, keeping single-record and
  scene-aggregate codecs type-safe and user-extensible. No virtual loader
  interface or cache-owned extension switch.
- **Error model**: error codes; corrupt input = log + reject, never exceptions.
- **Third-party discipline**: pinned-tag FetchContent, SYSTEM includes so
  -Werror never fires inside vendored code, license noted beside each pin.
- **Tests per codec**: known-answer decodes for hand-written parsers;
  synthetic malformed inputs rejected (bad magic, truncation, out-of-range
  dims); library-backed formats validated against scenes authored in-test via
  the library's own writer APIs where available (no binary fixtures in repo).

### Milestone 6 — Graphics Abstraction (RHI) ✅
- Device, Buffer, Texture, Sampler, Shader, PipelineState, CommandBuffer, Swapchain, Framebuffer
- Command-recording-first API (see §3 amendment)
- Zero OpenGL/Vulkan/DX12-specific types above this line
- **M6.0 core layer**: `f4_graphics` library (links `f4_core`); tags `Gpu{Buffer,Texture,Sampler,Shader,Pipeline,Swapchain,Framebuffer}Tag` with generational handles via `ObjectStore`; `GfxError` enum (no exceptions); `Backend`-tagged device types.
- **M6.1 binding model + pipeline state**: `PipelineLayoutDesc` (BindGroup → BindingSlot → BindingType), `VertexInputLayout` with a local `VertexFormat` resolved to bindings/locations, `DepthStencilState`, `RasterState`, `ColorAttachmentState`, `PipelineStateDesc`.
- **M6.2 record-only command buffer**: `CommandKind` (BeginRenderPass … DrawIndexed), typed `Command` list; engine records, backend executes at submit.
- **M6.3 `IDevice` abstract surface**: create*/destroy*/writeBuffer/submit/waitIdle, all `Result<*,GfxError>` or `GfxError`; `createDevice()` factory + `deleteDevice()`.
- **M6.4 NullDevice software backend**: `F4_GRAPHICS_BACKEND` build selector (`null|opengl|vulkan|dx12`, default `null`); generational `ObjectStore` per family (staleness/invalid → error, never crash); writeBuffer bounds checks; `submit()` replays the command log with a state machine (renderpass-active / pipeline-bound / index-bound → `GfxError::BadDescriptor` on violation). Headless-tested on the current six Windows presets.
- **M6.5 headless tests**: `tests/graphics/test_rhi.cpp` — create/destroy cycles for every kind, stale-handle rejection, zero-size buffer, writeBuffer bounds, pipeline-with-dangling-shader rejection, scripted command-record→submit success, and each submit-failure path.
- **Rationale (NullDevice)**: OpenGL/Vulkan need a window/context that the headless presets lack; the software backend exercises the whole `IDevice` contract (handles, validation, command conformance) deterministically in CI before any real GPU backend lands in M7/M9.

### Milestone 7 — OpenGL Backend ✅

- **M7.0 tooling + platform layer**: SDL2 (v2.32.10), Mesa (v26.1.8, llvmpipe
  software GL), and libepoxy (v1.5.10) installed via MSYS2 pacman (ucrt64);
  CA bundle fixed (188,900-byte `cacert.pem` downloaded from curl.se). `src/platform/`
  added: `Window` (SDL2 wrapper, visible/hidden), `GlContext` (libepoxy init,
  version parsed from `glGetString(GL_VERSION)` — `epoxy_gl_version()` unreliable
  on some drivers). GL 4.3 core context for vertex-attrib-binding API.
- **M7.0 CMake plumbing**: `find_package(SDL2 CONFIG QUIET)` — `F4_GRAPHICS_HAS_OPENGL`
  defined PUBLIC on `f4_graphics` only when SDL2::SDL2 target exists; MSVC presets
  stay NullDevice (298 tests). `f4_platform` library (window + gl_context,
  links f4_core + SDL2::SDL2 + epoxy). `f4_app` executable (windowed demo).
  GL test gated behind `F4_GRAPHICS_HAS_OPENGL AND TARGET SDL2::SDL2`.
- **M7.0 OpenGL backend**: `OpenGLDevice : IDevice` — per-family `ObjectStore`
  records (`GlBuffer/GlTexture/GlSampler/GlShader/GlPipeline/GlSwapchain/
  GlFramebuffer`) with GL object ids; mapping helpers (`glTextureInternal`,
  `glTopology`, `glIndexType`, `glCompare`, `glCull`, `glVertexFormat*`,
  `compileShader`, `bufferTarget`); full create/destroy/submit surface;
  `submit()` replays the command log with real GL calls (VAO setup via 4.3
  `glBindVertexBuffer`/`glVertexAttribFormat`, UBO binding via
  `glBindBufferBase`, indexed draw via `glDrawElements`).
- **M7.1 triangle**: windowed demo draws a rotating triangle; headless
  conformance test (`test_gl_device.cpp`) renders via hidden window
  (llvmpipe or real GPU driver) and verifies pixel output via
  `glReadPixels`. `initGl` hardened: parses `GL_VERSION` string directly
  instead of relying on `epoxy_gl_version()` which may return 0 on some
  drivers.
- **M7.2 indexed cube**: 24 vertices (4 per face, per-face colors), 36
  indices, depth buffer (`AttachmentType::Depth`), MVP uniform buffer via
  `writeBuffer` + `bindResource`, back-face culling, perspective projection
  (60° FOV), rotation around Y/X axes. `BindResources` wired in GL backend
  to resolve buffer handles and call `glBindBufferBase`.
- **Visual verification**: triangle and cube rendered to PPM/screenshot and
  visually confirmed correct (shape, color, depth, perspective).

Conformance debt found by the 2026-09-02 full-plan audit is closed:
- `SetScissor` now establishes `GL_SCISSOR_TEST` state and render-pass reset
- `RasterState::depthClampEnable` maps to `GL_DEPTH_CLAMP`
- each color attachment receives indexed blend factors, equations, enable state,
  and write masks; stale higher attachment state is reset
- color/depth clears temporarily override write masks as required and restore
  pipeline state afterward
- framebuffer attachment counts, dimensions, formats, depth-only and
  depth-stencil layouts, pipeline attachments/layouts, binding slots, and queried
  device limits are validated before issuing GL work
- focused state/pixel tests pass on Windows Mesa and Linux Mesa under Xvfb, so
  OpenGL is the reference backend for Vulkan parity

#### Performance optimizations (M7.2+)

Six fixes landed before scaling to complex scenes:

- **Fix #1 (SBO resources)**: `Array<BoundResource>` replaced with inline
  `BoundResource resources[8]` + `resourceCount` in `Command` — zero heap
  allocations for ≤8 resources per draw. `bindResource()` coalesces adjacent
  calls into a single Command.
- **Fix #2 (move semantics)**: `pushCommand(Command&&)` overload added; all
  recording methods use `std::move`. Eliminates deep copies of command
  payloads.
- **Fix #3 (writeBuffer)**: Removed unnecessary `glBindBuffer(target, 0)`
  unbind. Uniform buffers use orphaning (`glBufferData(nullptr)` +
  `glBufferSubData`) to avoid GPU pipeline stalls; vertex/index buffers
  keep plain `glBufferSubData`.
- **Fix #4 (FBO correctness)**: `GlFramebuffer` now has a real `GLuint fbo`.
  `createFramebuffer` creates GL FBOs for offscreen rendering; default
  framebuffer correctly uses FBO 0. `destroyFramebuffer` cleans up FBOs;
  destructor cleans up via `forEach`.
- **Fix #5 (conditional clears)**: `BeginRenderPass` builds
  `GLbitfield clearBits` from actual `clearOnLoad` flags; `glClear` only
  called when needed, color and depth cleared independently.
- **Fix #6 (GL state cache)**: Caches `glProgram_`, `glVao_`,
  `glDepthTest_`, `glCullFace_`, `glDepthFunc_`, `glDepthMask_`.
  Same-pipeline re-binds skip all state changes; individual values compared
  before issuing GL calls. Rule: engine-owned GL state only modified through
  `submit()`.

At that checkpoint, tests covered 314 cases / 85,622 assertions across five
working presets
(debug, release, asan, msvc-debug, msvc-release). 13 new tests (+165
assertions) for resource binding coalescing (0/1/4/8/9 resources, separated
by draw), move semantics, offscreen FBO, conditional clears, buffer upload
correctness, and GL state cache deduplication. The then-open msvc-asan blocker
was resolved during later work and is not a current limitation.

### Milestone 8 — Renderer + Render Graph v1 ✅
- ✅ Camera, Light, MeshRenderer components; Material concept
- ✅ RenderQueue sorting
- ✅ Render graph v1 declaration-order execution, imported framebuffers,
  persistent/transient realization, ownership-safe release, validation, and
  rollback; `Renderer::render()` records and submits through the graph
- ✅ Per-draw constant-ring allocation prevents deferred-command UBO aliasing;
  opaque and transparent queues use camera-space depth
- ✅ Deterministic shader-parity harness: tolerance-based float capture and
  exact RGBA8 frame comparison, including a real OpenGL compute probe. This is
  infrastructure/probe coverage, not yet cross-backend parity
- ✅ `ConstantRing` grows geometrically as a frame needs more slots. A direct
  130-slot test and a 65-object `Renderer` regression prove draws continue past
  the former fixed 64-slot boundary

### Milestone 9 — Vulkan Backend ✅
Completed delivery and acceptance gates:
1. Use the closed M0 Linux/Clang gate and M7/M8 reference contract as the
   baseline for Vulkan rather than preserving backend-specific OpenGL quirks.
2. Add Vulkan SDK/loader discovery, instance/device/queue selection, SDL surface,
   debug messenger, and `VK_LAYER_KHRONOS_validation` in development builds.
3. Implement buffers/images/samplers, memory allocation, descriptors, shader
   modules from build-time SPIR-V, graphics pipelines, and framebuffer/swapchain
   resize/present through `IDevice`.
4. Translate the existing command log and extend render-graph access metadata
   with stage/access/layout information. Use explicit barriers and queue
   synchronization; Vulkan provides few implicit ordering guarantees.
5. Replace M8's cross-frame constant/transient reuse assumptions with fenced
   frames-in-flight ownership. No unconditional `waitIdle()` in the steady-state
   frame path.
6. Run validation-layer and synchronization-validation gates with zero errors,
   then run the same seeded float probe and RGBA8 reference scenes through
   OpenGL and Vulkan. Cross-backend parity becomes real at this point.

### Milestone 10 — DX12 Backend ✅
Completed delivery and acceptance gates:
1. Added native D3D12 hardware-adapter selection with WARP fallback, debug-layer
   diagnostics, a direct queue, command allocators/lists, two fenced frames in
   flight, and deferred resource retirement.
2. Mapped the existing API-neutral binding model onto root signatures and
   dynamically sized CBV/SRV/sampler descriptor heaps. A 130-draw regression
   proves there is no hidden fixed descriptor/draw ceiling.
3. Implemented buffers, texture upload/readback, samplers, graphics PSOs,
   framebuffers, render targets, depth/stencil planes, explicit barriers, and
   SDL HWND to DXGI flip-discard presentation with recreation.
4. Added build-time DXC Shader Model 6 compilation and HLSL fixtures. Seeded
   float and exact RGBA8 probes enforce the same parity contract used by M9.
5. Full MinGW Debug/Release and MSVC Debug/Release suites pass. Repeated 12-frame
   captures are byte-identical, and the factory-selected DX12 smoke capture is
   visually complete with no missing or stale geometry.

### Milestone 11 — PBR ✅
- ✅ Core glTF 2.0 metallic-roughness factors and all five core texture slots
  map explicitly into renderer-owned materials; unsupported UV sets and invalid
  texture mappings fail instead of silently sampling the wrong data.
- ✅ GGX direct lighting, equirectangular prefiltered-mip IBL, derivative TBN
  normal mapping, occlusion/emissive inputs, alpha mask/blend, double-sided and
  unlit modes, and ACES tone mapping run through the engine renderer.
- ✅ A graph-routed directional shadow pass writes a persistent R16F depth map
  with a D32 depth attachment and applies manual 3x3 PCF in the main pass.
- ✅ Native OpenGL GLSL, Vulkan GLSL-to-SPIR-V, and DX12 HLSL-to-DXIL paths use
  one binding/constant contract. Full 64x64 opaque captures agree within two
  8-bit channel levels; filtered-shadow differences are confined to 67 edge
  pixels (1.64%) with a bounded 45-level maximum. Vulkan and DX12 validation
  report zero warnings and errors.
- ✅ Renderer-generated normal/UV/color fallback streams make position-only
  meshes valid on every backend without depending on API-specific missing-input
  behavior. Single-sided front faces remain visible in all three native paths.
- ✅ Clustered forward point lights (2026-10-20, branch `lights/point-lights`).
  `engine::PointLight` (world or `WorldMatrix`-local position, color, intensity,
  range) is no longer a stub. Each frame the CPU bins visible lights into a
  16x9-tile, 24-slice logarithmic froxel grid
  (`engine/lighting/clustered_lights.*`). Binning is conservative: a
  brute-force test over 600 lights and 40,000 samples finds no light missing
  from its cluster. Lights, cluster ranges, and indices pack into one read-only
  `u32` storage buffer (16-word header, 8-word light records, 2-word cluster
  records, 1-word indices; the index list is capped at 2^20 and overflow is
  counted in `FrameStats`). The PBR fragment shader on every backend looks up
  its cluster from `viewProj * worldPos` and adds GGX point lighting with a
  smooth range window `(1-(d/r)^4)^2 / (d^2+1)`. Directional light, shadow, IBL,
  emissive, and tone mapping are unchanged. Point-light shadows and HDR bloom
  were added afterwards (see below).
  The storage buffer uses RHI slot 7: GL SSBO binding 7, Vulkan/WebGPU flat
  binding 31, and DX12 `t7` (SRVs share `t` registers with textures 0-6). Metal
  uses slot 1 (`[[buffer(17)]]`) because it exposes only seven buffer slots.
  The buffer is a triple-buffered `DynamicBufferRing` with the new
  `BufferUsage::HostWrite`. On DX12, HostWrite storage writes go to an
  upload-heap twin and are copied at the start of the next submit instead of
  through a blocking `executeImmediate` per write. On OpenGL, HostWrite buffers
  are orphaned like uniforms.
- ✅ Evidence. `tests/engine/test_point_lights.cpp` covers:
  - CPU binning: empty header, culling, near-plane-crossing lights,
    conservativeness, and the index cap.
  - A NullDevice renderer run with 324 lights.
  - A 256-light night scene on a dark ground at 160x96, rendered on OpenGL,
    Vulkan, and DX12. GL/VK and GL/DX agree with 0 mismatching pixels (max
    channel delta 1).
  - Lights placed out of reach produce an image byte-identical to the lights-off
    frame, and validation reports zero warnings or errors.

  The Chromium WebGPU smoke binds the same buffer in its standalone PBR pipeline
  and requires a red point light to add red light through the engine renderer.
  `demo_bench` scene 20 ("Night Lights") shows 64-4096 animated colored lights.
- ✅ Frame time (`F4_POINT_LIGHT_BENCH=1`, 1280x720, RTX 3070 Ti Laptop on
  every backend after the adapter-selection fix below), 1,024 / 4,096 lights:
  - Range scaled to spacing: Vulkan 0.76 / 1.8 ms, OpenGL 1.0 / 2.2 ms,
    DX12 1.4 / 2.3 ms.
  - Fixed 2.5 m stress range: Vulkan 1.1 / 4.5 ms, OpenGL 1.3 / 5.2 ms,
    DX12 1.6 / 5.3 ms.
  - Before the fix, OpenGL and DX12 ran on the Intel Iris Xe iGPU (scaled
    4.2 / 8.2 ms and 8.0 / 17.4 ms; stress 4,096 lights at 27.5 and 67 ms).
- ✅ HDR bloom (2026-10-20, branch `lights/point-lights`). With
  `PbrRenderSettings::bloomEnabled` the PBR pass writes exposed linear HDR to an
  RGBA16F target instead of tonemapped sRGB. A threshold/soft-knee prefilter
  (`bloomThreshold` 1.0, `bloomKnee` 0.5) feeds a 13-tap downsample chain of
  `bloomLevels` half-resolution mips (default 5) and a tent-filter upsample
  chain. The composite adds `bloomIntensity` (default 0.6) times the glow, then
  applies ACES and sRGB encoding exactly as the non-bloom path does. The post
  shaders (`post_bloom_*`) derive UVs from `fragCoord / targetSize` so every
  backend samples the same texels. `Renderer::render(fb, camera, aspect, extent)`
  needs the output extent when bloom is on. Bloom is off by default, so all
  existing goldens are unchanged. Intensity 0 matches bloom-off within 1 level
  on GL, Vulkan, and DX12.
- ✅ Point-light shadows (same branch). `PointLight::castShadows` flags a light.
  Each frame, up to `pointShadowBudget` (0-16, default 8) flagged lights whose
  range reaches the view are kept, preferring the brightest and nearest.
  Each kept light gets six 256-texel tiles in an R16F cube-face atlas (plus
  D32F depth) that store `distance / range`. Casters are culled against the
  light sphere and each face frustum, then drawn instanced (the model matrix
  goes through vertex attributes 9-12 from a `DynamicBufferRing`) once per face.
  Each draw uses the new viewport/scissor origin in the RHI to target its tile.
  Clustered light records carry the shadow slot in words 10-14. The PBR fragment
  shaders pick the cube face, do a 3x3 PCF lookup in the atlas (VK/WebGPU
  binding 33/34, GL unit 8, DX12 `t8/s8`, Metal texture/sampler 7), and apply a
  bias of 0.02 x range. With no flagged lights the atlas pass is skipped. The
  sun shadow is untouched.
- ✅ RHI: `CommandBuffer::setViewport/setScissor` take an `(x, y)` origin in
  attachment pixels. OpenGL, Vulkan (flipped-height viewport), DX12, WebGPU, and
  Metal (unverified) all pass it through.
- ✅ Evidence (`tests/engine/test_point_lights.cpp`, 461/461 `f4_tests` in
  release and debug):
  - NullDevice tests cover face selection and matrices, atlas tiles, budget
    ordering, slot words, and the bloom mip chain.
  - A GPU night scene runs on GL, Vulkan, and DX12. Shadows darken 220 pixels
    and brighten none. Bloom at intensity 1.0 / threshold 0.6 lights 15,318
    more pixels, raising mean luma from 98.6 to 125.7.
  - Parity: GL/VK and GL/DX have 0 mismatching pixels for shadows (max delta
    1), bloom (0), and shadows+bloom (0). Validation is clean in debug.
  - WebGPU: the Chromium smoke renders an occluder in front of a point light and
    bloom at intensity 0 and 1.5. It compares both frames against locked
    fixtures (`tests/fixtures/renderer_{point_shadow,bloom}_reference.rgba`)
    that a native test renders from the same scene. Max delta is 1 against a
    tolerance of 2; native GL/VK/DX12 match the fixtures exactly. Regenerate
    the fixtures with `F4_UPDATE_EFFECT_FIXTURES=1`.
  - `demo_bench` scene 20 enables both. Keys: B toggles bloom, -/= change
    intensity, ;/' change threshold, K toggles point shadows, and ,/. change
    the shadow budget. Pillars cast the shadows.
- ✅ Frame cost at 1280x720 with 1,024 lights on the RTX 3070 Ti Laptop GPU.
  Ranges are over two runs:

  | Backend | base | +bloom (10 passes) | +32x32 pillars | 4 / 8 / 16 shadowed | 8 shadowed + bloom |
  |---|---|---|---|---|---|
  | Vulkan | 0.83-1.13 | 0.87-0.96 | 1.22-1.24 | 1.8-2.5 / 1.4-1.7 / 2.0 | 2.2-2.7 |
  | OpenGL | 1.06-1.18 | 1.06-1.13 | 1.38-1.40 | 2.0 / 1.8 / 2.2-2.3 | 2.4-2.8 |
  | DX12 | 1.26-1.50 | 1.31-1.32 | 1.50-1.58 | 2.0 / 1.8-1.9 / 2.5-2.6 | 2.2-2.3 |

  All times are ms/frame. Bloom is within noise at 720p. Point shadows add
  about 0.4-1.0 ms on top of the pillar scene for 8-16 lights (6 instanced
  draws per light). The 4-light case runs first and absorbs the atlas and
  pipeline warm-up. Before instancing, the per-entity, per-face draws exhausted
  the device buffer arena (crash at 16 lights x 1,024 pillars).
- ✅ Static point-shadow caching (2026-10-20, commits f0bf4ec, f59be27,
  0fb4662). `PbrRenderSettings::pointShadowCaching` (default on) keeps a
  shadowed light's six atlas tiles from the previous frame when nothing they
  depend on changed.
  - Slots are stable. `assignStablePointShadowSlots` keeps a surviving light
    in its old slot and gives freed slots to new lights, so a light entering or
    leaving the budget does not move the others.
  - Dirty tracking uses content signatures instead of ECS dirty flags, because
    `TransformDirty` is consumed before rendering and tests write `WorldMatrix`
    directly. Each slot hashes the light's position, range, and tile, plus every
    caster inside its sphere (entity, mesh, submesh, material, world matrix, and
    alpha inputs). A light is redrawn when its signature changes or the slot is
    new, and the whole atlas is cleared when the atlas is (re)created.
  - Partial redraw loads the atlas and resets only the dirty tiles with a
    full-tile triangle from a `tileReset` pipeline variant (depth compare
    Always, no depth test or write, still bound to the D32F attachment). It
    writes exactly what the clear writes, so the result is bit-identical. A
    zero-work "point-shadow-atlas-keep" graph pass declares the atlas as last
    frame's fragment-shader read, so the Vulkan barrier into the load pass
    orders after those reads and requests `ColorRead` for the load op.
  - `FrameStats::pointShadowLightsRedrawn` reports the redraw count. With
    nothing moving it is 0 on every frame after the first.
  - Evidence: NullDevice tests cover slot stability and reuse. A GPU test on
    GL, Vulkan, and DX12 renders an 8-step sequence with caching on and
    off: first frame, static, one caster moved, static, one light moved, camera
    moved (the shadow selection changes), static, and every caster moved. Every frame is pixel-identical (tolerance 0) and
    Vulkan/DX12 validation is clean in debug.
  - Savings (`F4_POINT_LIGHT_BENCH=cache`: 1,024 lights, 1,024 pillars, static
    scene, 7 interleaved rounds, min ms/frame, cached / uncached):

    | Backend | 4 shadowed | 8 shadowed | 16 shadowed |
    |---|---|---|---|
    | OpenGL | 1.39 / 1.46 | 1.54 / 1.71 | 1.68 / 1.94 |
    | Vulkan | 1.34 / 1.44 | 1.37 / 1.59 | 1.38 / 1.68 |
    | DX12 | 1.81 / 1.91 | 1.83 / 2.07 | 1.92 / 2.05 |

    With nothing dirty the atlas pass is skipped, so cached cost grows more
    slowly with the shadowed-light count (flat on Vulkan); uncached grows by about 0.01-0.04 ms per light. A later rerun on a busier
    machine (other apps using the CPU, everything ~1 ms slower) kept cached
    ahead in 7 of 9 mins, but medians were within noise.
- ✅ GPU light binning (2026-10-20, commits b66144a, 4316e62, d6e8949,
  850ba8e, ea6fc66). `PbrRenderSettings::gpuLightBinning` (default on) runs
  cluster assignment as a compute pass on OpenGL 4.3+, Vulkan, DX12, and
  WebGPU. CPU binning stays as the fallback and the reference.
  - The CPU still culls and writes light records with the same helpers as the
    CPU path (`gatherLightRanges`, `writeLightRecords`). It also appends three
    packed words per visible light with its tile/slice range
    (`buildClusteredLightBinningInput`). The cluster grid and index list are
    left for the GPU.
  - A "light-binning" graph pass runs three dispatches on the output buffer
    (64-thread groups, buffer barriers in between):
    1. Copy the header and records, and count each cluster's lights. Light
       ranges are staged in shared memory.
    2. One workgroup does a prefix scan with the 2^20 index cap.
    3. Each cluster writes its list in ascending light order.
    There are no atomics, so the output matches the CPU buffer word for word.
  - Shaders: `light_binning.comp` (GL, and Vulkan via `-DF4_VULKAN=1`),
    `light_binning_dx12.hlsl` (cs_6_0), and `light_binning_webgpu.wgsl`.
    WebGPU `BeginRenderPass` now ends an open compute pass.
  - Fallbacks: Null, Metal (no shader yet), callers that use `record()`
    directly, frames with no visible lights, and pipeline failures all use CPU
    binning. `FrameStats::pointLightBinningDispatches` reports which path ran.
  - Vulkan storage buffers that are neither HostWrite nor vertex/index/uniform
    are now DEVICE_LOCAL (with a host-visible fallback). `writeBuffer`/
    `readBuffer` on them go through a staging copy.
  - Evidence: a NullDevice emulation of the three passes reproduces the CPU
    buffer. A GPU test on GL, Vulkan, and DX12 compares CPU vs GPU binning for
    1,024 lights, 1,024 lights with an index cap of 20,000, and shadowed
    lights. All have 0 differing pixels (tolerance 0) and 0 differing buffer
    words. Cross-backend parity with GPU binning stays within max delta 1. The
    Chromium WebGPU smoke requires the 3 dispatches and still matches the
    point-light and point-shadow fixtures (max delta 1).
  - Frame time (`F4_POINT_LIGHT_BENCH=binning`, 1280x720, RTX 3070 Ti Laptop,
    median of 5 interleaved rounds, ms/frame with `render()` CPU time in
    parentheses, CPU binning → GPU binning):

    | Backend | stress 1,024 | stress 4,096 | scaled 1,024 | scaled 4,096 |
    |---|---|---|---|---|
    | OpenGL | 1.39 (1.36) → 1.51 (0.51) | 7.76 (5.74) → 8.44 (1.67) | 1.19 (1.18) → 1.37 (0.54) | 2.32 (2.29) → 2.51 (1.72) |
    | Vulkan | 1.31 → 1.81 | 7.58 → 8.57 | 1.00 → 1.34 | 2.04 → 2.71 |
    | DX12 | 2.01 → 1.66 | 8.67 → 9.43 | 1.54 → 1.23 | 2.58 → 2.29 |

    On Vulkan and DX12, `render()` CPU time tracks frame time because it waits
    for the frame in flight. DX12 gains about 0.3 ms except at 4,096 stress
    lights. GPU binning cuts OpenGL `render()` CPU time 2.6-3.4x, but frame time
    is 0.1-0.7 ms worse. Vulkan is 0.3-1.0 ms worse. The kernel is brute force:
    each cluster tests every visible light, which is 3,456 clusters x 4,096
    lights for each of two passes.
- Still open: Metal has every shader and binding written but was not compiled
  or run (no Apple hardware), and it bins on the CPU. GPU binning is not yet a
  frame-time win on Vulkan/OpenGL. Next steps are a per-tile or per-slice
  light pre-cull before the per-cluster loop, and GPU timestamp queries in the
  RHI (none today) to measure the pass itself. `FrameStats::maxLightsPerCluster`
  reads 0 in GPU mode. Bloom has no lens dirt or anamorphic options. The
  1,024-per-type object stores crash on overflow instead of returning an error,
  so two renderers cannot share one GL device.
- ✅ Adapter selection (2026-10-20). Every backend now picks the strongest
  GPU by default, using one ranking in `graphics/gpu_selection.*`: discrete,
  then integrated, then virtual/other, with software rasterizers (WARP /
  Microsoft Basic Render Driver, llvmpipe) used only when nothing else exists.
  Ties go to more dedicated memory, then to enumeration order.
  - DX12 ranks every FL11 DXGI adapter and uses `D3D12_FEATURE_ARCHITECTURE`
    UMA to mark integrated parts. It previously took the first hardware
    adapter, which DXGI lists as the iGPU on hybrid laptops.
  - Vulkan ranks eligible devices by `deviceType`, then by device-local heap
    size.
  - OpenGL cannot enumerate adapters. Every Windows executable (`f4_tests`,
    `demo_bench`, `f4_app`, `f4_vulkan_app`, `f4_dx12_app`) exports
    `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance` via
    `f4_prefer_high_performance_gpu()`.
  - WebGPU already requests `HighPerformance` (the browser decides).
  - `F4_GPU=auto|discrete|integrated|<n>` overrides the pick. `<n>` is the
    API enumeration index printed in the startup ranking log. OpenGL honours
    `integrated` by clearing the exports before the first GL context and
    ignores indices with a warning. An unmatched override falls back to auto
    with a warning.
  - Verified on the laptop: GL `GL_RENDERER` and the DX12 DXGI description
    changed from Intel Iris Xe to the RTX 3070 Ti. `F4_GPU=integrated` moves
    all three back to Iris Xe, and GL/VK/DX point-light parity holds on both
    GPUs.

### Milestone 12 — Optimization
- ✅ World-space AABB frustum culling with per-frame candidate/visible/culled
  counters; invalid bounds conservatively remain visible
- ✅ Deterministic opaque state batching and native per-instance model/normal
  vertex streams on OpenGL, Vulkan, and DX12; transparent draws stay isolated
- Occlusion culling
- ✅ Distance/stage-priority job system (delivered by M16); multithreaded command
  recording remains open
- SIMD math paths
- Render graph scheduling, async compute

### Milestone 13 — Deterministic Noise Library (procgen track) ✅
- ✅ Value/Perlin/Simplex noise, FBM, and domain warping; every function takes
  an explicit seed with no hidden global state
- ✅ GCC/MSVC/Clang CPU golden vectors are bit-identical under the locked FP
  flags on the Windows/Linux gate
- ✅ API-neutral compute pipeline/dispatch, compute-stage barriers,
  storage-buffer binding, and readback are implemented by Null, OpenGL,
  Vulkan, and DX12 backends
- ✅ Equivalent GLSL/SPIR-V and HLSL/DXIL kernels preserve the 64-bit integer
  hash through portable 32-bit pairs and cover every public M13 sampler
- ✅ Five focused cases / 109 assertions compare CPU/OpenGL/Vulkan/DX12 and
  OpenGL/Vulkan plus Vulkan/DX12 pairs. The largest measured absolute error is
  `1.78814e-7`; the gate requires measured error x4 to remain within the
  centralized `1e-5` absolute tolerance

### Milestone 14 — World Data Model (procgen track) ✅
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
- Completed with negative-coordinate hash/region coverage, explicit
  padding-free delta encoding, monotonic stale-work versions, immutable base
  snapshots with locked overlays, and compile-time policy facades.
- Voxel delivery includes exact 32³/u16 layout, empty scans, local and
  world-spanning DDA; height delivery includes bit-identical shared edges,
  normalized bilinear material sampling, and neighbor-fed seam normals; hex
  delivery includes dense axial indexing, shared corner elevations and center
  heights, rings, spirals, and line drawing.
- Ten focused cases / 411 assertions pass on GCC, MSVC, and Clang ASan+UBSan.
  The `world_determinism` CTest label locks generated voxel/height/hex data and
  serialized edit bytes across all three toolchains.

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
- Completed with a common `MeshData` vertex/index/surface record and canonical
  `resources::Mesh` conversion. Surface ranges preserve per-face material ids
  while position, normal, UV, and color use the existing M11 vertex contract.
- Voxel delivery caches the full 34³ input window, owns only center-chunk faces,
  greedily merges equal material/normal/AO masks, flips non-coplanar AO
  diagonals, and assigns unlike transparent interfaces to one block id so
  neighboring chunks cannot emit coplanar duplicates.
- Height delivery emits 65×65 top vertices, fixed-winding quads, neighbor-fed
  edge normals, four material weights plus their blended display color, and
  four skirts with an optional constant policy-transition floor. Live shared
  edge edits mirror into resident cardinal/diagonal tiles before remeshing.
- Hex delivery uses the M14 pointy-top axial/corner lattice for colored top,
  bottom, and exposed side faces; same-policy neighbors suppress shared walls.
- Twelve focused cases / 16,129 assertions pass on GCC, MSVC, and Clang 18
  ASan+UBSan. `terrain_determinism` locks quantized vertex/index/surface dumps
  across all three toolchains. Three back-face-culled gallery captures are
  pixel-identical with no missing faces, seam gaps, z-fighting, or blinking.

### Milestone 16 — World Streaming (procgen track) ✅
- ✅ Camera-centered rings select voxel/height/hex residency through RegionMap.
  Chunk topology stays main-thread-owned; pure cell generation and immutable
  snapshot meshing run on the pulled-forward first-class job system.
- ✅ Distance then stage priority, deterministic tie ordering, frame-time and
  operation caps, and `layout → cells → meshes` publication prevent generation
  or meshing spikes from becoming unbounded main-thread work.
- ✅ Handle generation plus monotonic chunk versions reject completions after
  unload/reuse, regeneration, border invalidation, or edits. Shutdown drains
  workers before result storage is destroyed.
- ✅ Sparse overlays persist across unload/reload. Height edge/corner edits are
  recorded for mirrored neighbors even while those chunks are absent.
- ✅ Resident terrain queries expose cross-chunk solid-block DDA and the same
  bilinear height/material sampler used by generation and rendering.
- ✅ Generated voxel bases with at most 16 ids store 4-bit indices; 17–256 ids
  use 8-bit indices; more diverse chunks retain the dense u16 representation.
  Packing order and round trips are deterministic.
- ✅ M5's deferred cache lifetime contract is closed with typed shared
  retain/release ownership and final-release path eviction.

Ten focused M16 cases cover 126 assertions on scheduling, stage/version gates,
palette packing, ring movement and caps, stale unloads, edit persistence,
collision, shared resource lifetime, and deterministic regional convergence.
GCC Debug/Release, MSVC Debug/Release/ASan, and Linux Clang Debug/ASan+UBSan
pass all six CTest targets. The 20-scene OpenGL bench completed 60 inspected captures
without missing/blinking faces or regressions in the seven validation scenes.

### Milestone 17 — Native Metal Backend

1. Add macOS Apple Clang Debug/Release presets and CI on Apple hardware. The
   milestone cannot be marked complete from a cross-compile-only or non-Apple
   validation run.
2. Isolate Objective-C++ below the RHI. Create an SDL Metal view, bind its
   `CAMetalLayer` to a native `MTLDevice`, and implement drawable resize/present
   without leaking Cocoa or Metal types above `src/backends/metal`.
3. Implement native command queues/buffers, frames in flight, buffers, textures,
   samplers, render targets, graphics and compute pipelines, resource bindings,
   barriers/synchronization, readback, and deferred resource retirement.
4. Compile MSL to a metallib as a build artifact. Port the M11 material/shadow
   contract and M13 paired-`u32` compute kernel without changing RHI semantics.
5. Require clean Metal diagnostics, focused resource/pixel/stability tests, the
   130-draw regression, M11 full-frame comparison, M13 CPU/GPU and cross-backend
   probes, and repeated demo captures inspected for missing or blinking faces,
   alpha-order faults, normal-map inversion, moire, and z-fighting.

Implementation is in progress as of the 2026-09-03 audit. Hosted Apple Clang
Debug/Release CI builds the native Objective-C++ backend and MSL libraries and
passes CPU/build conformance. A manually dispatched Metal-capable Apple runner
is the acceptance gate: it enables Metal API/shader validation, runs focused resource, 130-draw,
resize/present, M11 locked-image, and M13 CPU/GPU tests, then produces three
stability captures each for the cube, Cornell box, PBR sphere grid, mipmapping,
z-fighting, alpha order, normal-map orientation, hybrid rendering, and terrain.
The implementation and non-Metal regressions have been checked on Windows, but
no hardware-only result is claimed until that runner executes cleanly and all 27
captures are visually inspected.

### Milestone 18 — Browser WebGPU Backend ✅

1. Add an Emscripten/WebAssembly preset and browser application. Use
   Emdawnwebgpu's C API over the browser's WebGPU implementation; WebGL is not an
   acceptable fallback for this backend.
2. Integrate the browser event loop plus asynchronous adapter/device creation,
   device-loss handling, canvas configuration, resize, and presentation behind
   the existing synchronous RHI-facing ownership model.
3. Implement buffers, textures, samplers, bind groups, render and compute
   pipelines, render/compute passes, copies/readback, limit negotiation, and
   deferred destruction without backend types leaking upward.
4. Maintain WGSL shaders as a native family. M13 must use the canonical
   paired-`u32` mixer because WGSL has no concrete runtime `u64` scalar.
5. Require zero uncaptured WebGPU errors, focused resource/pixel/stability tests,
   the no-fixed-draw-ceiling regression, M11 full-frame comparison, M13 CPU/GPU
   and cross-backend probes, and automated repeated captures of every validation
   and terrain demo with visual inspection.

Recommended interleave: M13 right after M4 (no renderer required), M14 after
M5 (resources), M15 as soon as M7 draws its first mesh, M16 alongside M12.
M17 and M18 extend the renderer track only after the original three-backend RHI
contract is locked, so they conform to it instead of reshaping it around one API.

## 9. Key References

### Architecture
| Topic | Resource |
|-------|----------|
| Overall architecture | *Game Engine Architecture* — Jason Gregory (Ch. 3–6, 8) |
| Engine math paradigms | *Foundations of Game Engine Development Vol. 1* — Eric Lengyel |
| Modern rendering theory | *FGED Vol. 2: Rendering* — Lengyel (companion to our M6–M12) |
| Scene-graph/realtime structure | *3D Game Engine Architecture/Design* — David H. Eberly |
| Subsystem coupling analysis | **SyDRA** (2024): dependency study of 10 open-source engines — validates §4 layering and informs where resources sit between ECS and RHI |
| Data-driven rendering | Gabriel Sassone's data-driven rendering writings: render passes/shaders dictated by data files, not hardcoded — target model for M8 render graph v2+ |

### Rendering
| Topic | Resource |
|-------|----------|
| Renderer design | "Designing a Modern Rendering Engine" thesis; Data Driven Renderer PDF; Modern Renderer Design thread |
| Data-oriented rendering | "ECS and Rendering" (Our Machinery); Data Driven Rendering (Sassone) |
| PBR | Allegorithmic PBR Guide; UE4 PBR docs; Moving Frostbite to PBR |
| Light transport foundations | *Physically Based Rendering: From Theory to Implementation* — Pharr/Jakob/Humphreys (offline but the physics our real-time approximations approximate) |
| Lighting pipelines | Forward vs Deferred vs Forward+ comparisons (3DGEP); Forward+ paper; 2048 point lights @60fps — **mandatory reading before M6 device design / M8 pipeline choice** |
| Vulkan | Writing an Efficient Vulkan Renderer (zeux.io); BansheeEngine Vulkan blog/code |
| DirectX 12 | [Microsoft Direct3D 12 programming guide](https://learn.microsoft.com/en-us/windows/win32/direct3d12/directx-12-programming-guide) and [HLSL scalar types](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-scalar) |
| Metal | [Apple Metal resources and specifications](https://developer.apple.com/metal/resources/) and [SDL Metal view contract](https://wiki.libsdl.org/SDL2/SDL_Metal_CreateView) |
| WebGPU | [W3C WGSL specification](https://www.w3.org/TR/WGSL/) and [Emscripten WebGPU support](https://emscripten.org/docs/porting/multimedia_and_graphics/WebGPU-support.html) |
| Portable shader integers | [OpenGL `ARB_gpu_shader_int64`](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_gpu_shader_int64.txt), [Vulkan `shaderInt64`](https://registry.khronos.org/vulkan/specs/latest/man/html/VkPhysicalDeviceFeatures.html), and [Apple Metal feature tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf) |
| Optimization | Fast Hierarchical Culling (Cesium); Frostbite compute pipeline; Agner's optimization resources |
| Physics (later) | Introduction to Networked Physics (Gaffer on Games); Unified Particle Physics preprint |
| Code study | BansheeEngine (C++14/Vulkan), Tiny Renderers, EntityX |

### Procedural generation
| Topic | Resource |
|-------|----------|
| Papers catalog | [hzxie/Awesome-3D-Scene-Generation](https://github.com/hzxie/Awesome-3D-Scene-Generation) — terrain simulation, rule-based city generation, neural techniques; feeding M13–M16 |

### Hubs
| Topic | Resource |
|-------|----------|
| Curated rendering/GI/PBR | [jcoder58/awesome3dengine](https://github.com/jcoder58/awesome3dengine) |
| Full toolset scope | [stevinz/awesome-game-engine-dev](https://github.com/stevinz/awesome-game-engine-dev) — scene editors, asset pipelines (editor era) |
## 10. Implementation History

This section is a chronological log. Preset counts and “next” statements inside
older entries describe their checkpoint, not current status; the audit table in
§8 and the final “Current next work” subsection are authoritative.

M5.0 core layer shipped (resource cache, mesh/texture/shader records, handles,
file IO, .f4asset container). M5.1 BMP+TGA hand-written decoders shipped with
`Result<Texture, LoadError>` loader seam, `load_error.h` shared error codes,
and `std::span<const std::byte>` entry points. M5.2 stb_image vendor dep
shipped — PNG/JPG/HDR/BMP/TGA/PSD/GIF/PIC via `stbi_load_from_memory` +
`stbi_loadf_from_memory` wrappers, FETCHCONTENT pinned to commit
`2c980bb`, SYSTEM includes, no engine warnings. M5.3 cgltf vendor dep
shipped — glTF 2.0 / GLB parsing into Mesh records with position, normal,
texcoord, tangent, color attributes; indices; auto-computed bounds.
FETCHCONTENT pinned to commit `85cd623`, SYSTEM includes. M5.4 minimal
OpenEXR scanline loader hand-written — header/attribute parse, HALF+FLOAT
channels, uncompressed + ZIP (zlib v1.3.1 via FetchContent, cross-platform),
output RGBA32_FLOAT; tiled/deep rejected. M5.5 DDS container + BC1–BC7
software decode hand-written — BC1–BC5 decoders from published specs, BC6H
and BC7 bit-accurate decoders with partition/fixup/weight tables generated
from the MIT-licensed DirectXTex reference (bc67_tables.inc); DDS loader
parses legacy fourcc (DXT1/3/5, BC4U/BC5U, ATI1/ATI2) and DX10 headers
(DXGI BC1–BC7 incl. BC6H UF16/SF16), decodes full mip chains; output
RGBA8/R8/RG8 unorm for LDR, RGBA32_FLOAT for HDR per §12. M5.6 KTX2
container + BasisU vendor dep shipped — hand-written KTX2 container parser
(header, level index, raw Vulkan format path) + basis_universal v2_50
vendored via FetchContent; raw KTX2 path decodes
R8/RG8/RGBA8/BGRA8/R16F/RGBA16F/RGBA32F direct; Basis path delegates
ETC1S and UASTC payloads to ktx2_transcoder for RGBA8 or RGBA16F output;
zstd supercompression deferred. M5.7 ASTC decode shipped — all 14 standard
2D block sizes (4×4 through 12×12) in both LDR and HDR profiles;
16-byte `.astc` header parser + block→raster assembly via basisu's
fuzz-tested `basisu_astc::astc::decompress_ldr`/`decompress_hdr` from
`basisu_astc_helpers.h` (reuses existing basis_universal v2_50 vendor dep);
`BASISU_DISABLE_ANDROID_ASTC_DECOMP=1` enables the software decoder;
`astc_helpers::init_tables()` called lazily on first decode; LDR→RGBA8_UNORM,
HDR→RGBA32_FLOAT. 256 tests / 85,213 assertions across all six presets.
M5.8 OBJ+FBX shipped — OBJ hand-written parser (~370 lines) supports
`v`/`vt`/`vn` faces, groups, `f v/vt/vn` + `f v//vn` + `f v/vt` + `f v`
formats, fan-triangulation of quads/ngons, negative indices, vertex
deduplication, `usemtl`/`mtllib` parsed without error; FBX via ufbx v0.23.0
(single-file MIT, vendored via FetchContent, compiled via `ufbx_impl.cpp`
with `UFBX_STATIC_DEFINE`); `ufbx_mesh*` face iteration with
fan-triangulation, position/normal/UV extraction into engine Mesh records.
 277 tests / 85,300 assertions across all five working presets.
M5.9 USD+Alembic shipped — Alembic via official tree (Ogawa-only, no HDF5,
v1.8.12) + Imath v3.2.0; USD via tinyusdz v0.9.1 (zero-dep single-file reader).
Both integrated through the pinned FetchContent discipline. FetchContent
issues upgraded: Alembic's unconditional `install()`/`export()` overridden with
a local `macro(export)` no-op + `CMAKE_SKIP_INSTALL_RULES (no source patch
needed); tinyusdz GCC 16/MSYS2 patched (`cmake/patch_tinyusdz.cmake`): removed
clang-only `-Wa,-mbig-obj`, added `-Wno-unused-function
-Wno-unused-but-set-variable`, `#include <cstdint>` (GCC 16 dropped transitive
includes), and `auto fn = &TypeTraits<T>::type_id` pattern for dependent-name
lookup. Loaders wrap third-party exceptions into `Result<_, LoadError>` codes
(no exceptions leak past the seam). `loadAlembic(Span)->Result<AlembicScene>`
reads via `IFactory::getArchive` over an in-memory seekable streambuf, walks
`IPolyMesh` schema, fan-triangulates quads/ngons, extracts pos/normal/UV;
`loadUsd(Span)->Result<UsdScene>` via `LoadUSDFromMemory`, recurses the prim
tree, detects `GeomMesh`, fan-triangulates. Tests: Alembic cube authored
in-test via `OPolyMesh` writer API (round-trip through our memory loader); USD
cubes from tinyusdz's own `cube-indexed-primvar.usda` + `cube.usdc` fixtures.
285 tests / 85,352 assertions (+8 tests / +52 assertions, 4 Alembic + 4 USD)
across the five presets working at that checkpoint (debug, release, asan,
msvc-debug, msvc-release). The sixth preset, msvc-asan, was then blocked because
MSVC's `VS_GLOBAL_EnableASAN` injects STL-annotation defines
(`_ENABLE_STL_ANNOTATION`) into f4_core/f4_tests objects but not into the
FetchContent vendor static libs (tinyusdz, Alembic, Imath, basis, zlib),
producing LNK2038 `annotate_string`/`annotate_vector` mismatches at link
time. This M5.0-M5.9 checkpoint therefore recorded "five working presets";
the mismatch was resolved later and the current M8 gate passes all six.
The next step at that checkpoint was M6.

M6 Graphics Abstraction (RHI) shipped — new `f4_graphics` library (links
`f4_core`) introduces the full device-object surface behind generational
handles (`Gpu{Buffer,Texture,Sampler,Shader,Pipeline,Swapchain,Framebuffer}Tag`),
`GfxError` codes (no exceptions), and the record-only command model. Backend
is selected at build time via `F4_GRAPHICS_BACKEND` (`null|opengl|vulkan|dx12`,
default `null`); `src/backends/null/null_device.cpp` is the first concrete
backend — a software NullDevice that keeps a fake buffer/swapchain, validates
every create/destroy against a per-family `ObjectStore` (stale/invalid → error,
never crash), bounds-checks `writeBuffer`, and replays the submitted command
log through a state machine (render-pass-active / pipeline-bound /
index-bound), returning `GfxError::BadDescriptor` on a scripted violation.
Rationale: OpenGL/Vulkan need a window/context the headless gate lacks, so the
software backend exercises the whole `IDevice` contract deterministically in CI
before any real GPU backend lands (M7 OpenGL, M9 Vulkan). Tests
`tests/graphics/test_rhi.cpp` cover create/destroy cycles for every kind,
stale-handle rejection, zero-size buffer, writeBuffer bounds, pipeline-with-
dangling-shader rejection, scripted command-record→submit success, and each
submit-failure path (draw-without-pipeline, drawIndexed-without-ibuffer,
draw-outside-renderpass). 298 tests / 85,418 assertions (+13 tests / +66
assertions) across the five presets working at that checkpoint (debug, release,
asan, msvc-debug, msvc-release). The then-open msvc-asan LNK2038 blocker was
resolved during later work; it is not a current limitation.

M7 OpenGL backend shipped — SDL2 windowing (v2.32.10), Mesa llvmpipe (v26.1.8),
and libepoxy (v1.5.10) installed via pacman (ucrt64). `OpenGLDevice : IDevice`
with full create/destroy/submit; real GL 4.3 core context via libepoxy; triangle
verified (hidden-window conformance + screenshot); indexed cube with per-face
colors, depth buffer, MVP uniform buffer, perspective projection, back-face
culling — all visually confirmed. Six performance optimizations before scaling:
(1) SBO inline resources (≤8 resources/command, zero heap alloc), (2) move
semantics on pushCommand, (3) writeBuffer orphaning for Uniform buffers, (4)
real GL FBOs for offscreen framebuffers, (5) conditional clears from
clearOnLoad flags, (6) GL state cache (same-pipeline re-binds deduplicated).
314 tests / 85,622 assertions across the five presets working at that
checkpoint. 13 new tests
(+165 assertions) covering resource coalescing, move semantics, FBO, clears,
buffer upload, and state cache. The msvc-asan blocker was still open at this
checkpoint and was resolved later. The next step then was M8 render graph.

M8 renderer and render-graph v1 shipped — camera/light/mesh-renderer components,
material and GPU-mesh ownership, camera-depth queue sorting, graph-routed
renderer recording/submission, imported framebuffers, operational persistent and
transient resource lifetimes, ownership-safe cleanup, rollback/error coverage,
per-draw constant-ring allocation, world-grid helpers, and deterministic
float/RGBA8 shader-parity comparisons. The seeded OpenGL compute probe stays
inside the calibrated tolerance, and OpenGL now explicitly selects the shared
zero-to-one clip-depth convention. All M1.5 carry-forwards are closed. Debug,
release, asan, msvc-debug, msvc-release, and msvc-asan build and pass (365 test
cases / 86,731 assertions in the debug binary on 2026-09-02).

The OpenGL demo bench now has 20 smoke-testable scenes. Rendering validation
includes a Cornell box, a GGX metallic/roughness sphere grid, mipmapping versus
moire, z-fighting separation, sorted versus reversed alpha blending, normal-map
Y orientation, and deferred opaque geometry composited with forward transparent
geometry. These scenes validate RHI/backend behavior but do not complete M11:
production material integration, IBL, shadow filtering, tone mapping, and GLSL /
HLSL parity remain planned there.

Visual sampling of the cube and seven rendering-validation scenes across three
frames found no blinking or missing cube faces. Cornell geometry, the complete
PBR sphere grid, mip/moire contrast, z-fighting stress bands, alpha-order
contrast, normal-map Y orientation, and hybrid deferred-opaque/forward-
transparent composition were all present.

M0/M5/M7/M8/M9 audit closeout completed 2026-09-02. Linux Clang 18 debug and
ASan+UBSan gates configure, build, and pass on Ubuntu 24.04; the OpenGL suite and
all 20 pre-M9 demo scenes also pass under Xvfb/Mesa. The six Windows GCC/MSVC
presets pass their CTest gates; MSVC ASan excludes Vulkan because loading an
unsanitized vendor driver can exhaust its reserved shadow-address region, while
Vulkan remains covered by Debug, Release, Linux, and validation-layer gates. The
debug binary reports 381 test cases / 87,163 assertions. The dependency manifest
is present, OpenGL conformance debt has focused regression coverage, and renderer
constant allocation grows beyond the former 64-draw boundary. M9 adds a Vulkan
presentation/capture demo plus 10 focused cases / 211 assertions covering device
setup, resources, descriptors, barriers, retirement, rendering, swapchain
recreation, and OpenGL/Vulkan parity. Its Windows and Linux captures differ by at
most one 8-bit channel value. The supplied texture archives were extracted in
place under `assets free to use but ziped/` and remain untracked; deterministic
generated fixtures are still used for milestone gates.

M10 and M14 completed on 2026-09-02. M10's native DX12 path passes hardware/WARP,
resource, binding, 130-draw, barrier, swapchain-recreation, pixel-stability, and
three-backend parity coverage; its selected-backend smoke capture is visually
complete. M14 adds 10 focused cases / 411 assertions. Its fixed generated-data
and padding-free delta-stream digests match GCC, MSVC, and Clang, and the full
Linux Clang ASan+UBSan suite passes under Xvfb.

M11 completed on 2026-09-02. The production renderer now owns the complete
metallic-roughness material path, semantic fallback textures and vertex inputs,
IBL, filtered directional shadows, tone mapping, and native shaders for all
three backends. Eleven focused M11 cases / 254 assertions cover CPU reference
math, glTF mapping, resource ownership, graph execution, complete-frame parity,
missing vertex channels, front-face visibility, and clean Vulkan/DX12 validation.
The demo bench also gained repeatable back-buffer capture; three captured frames
from each of all 20 scenes were inspected, with no blinking or missing cube
faces and all seven rendering-validation scenes intact.

The first M12 renderer slice completed on 2026-09-03. Mesh-local bounds are
recomputed from uploaded geometry, transformed conservatively into world-space
AABBs, and tested against the camera frustum before queue insertion. A complete
pipeline/material/mesh/submesh batch key now feeds dedicated native instanced
vertex paths on OpenGL, Vulkan, and DX12 through a growable triple-buffered
upload ring. Focused NullDevice and native pixel-parity regressions cover culling,
batch toggles, buffer growth/reuse, non-finite queue depths, and two-object
instanced output. All 60 captures from the 20-scene OpenGL bench were inspected
across three frames, including the cube and seven rendering-validation scenes,
with no missing or blinking faces.

M13 completed on 2026-09-03. The RHI now represents compute-only pipelines,
dispatch commands, compute synchronization, storage buffers, and deterministic
readback without leaking native APIs. OpenGL, Vulkan, and DX12 execute equivalent
noise kernels covering value, Perlin, simplex, FBM, and domain warp probes. Five
focused cases / 109 assertions pass with a maximum measured absolute error of
`1.78814e-7`, more than the required 4x margin inside the shared `1e-5`
tolerance. Windows GCC Debug/Release and MSVC Debug/Release/ASan configurations
pass all six CTest targets; Linux Clang Debug, determinism, Vulkan presentation,
and ASan+UBSan gates also pass. A fresh 60-frame OpenGL demo capture review
found no missing or blinking faces and no regressions in the seven validation
scenes.

M18 completed on 2026-09-03. The Emscripten 6.0.9 preset
uses Emdawnwebgpu's C API over `navigator.gpu`; there is no WebGL fallback. The
browser RHI covers adapter/device/canvas setup, negotiated limit
checks, buffers, textures, samplers, shaders, explicit bind-group and pipeline
layouts, render/compute passes, storage dispatch/readback, and animation-frame
presentation. The production M11 standard, instanced, fragment, and shadow WGSL
modules are selected by `MaterialCache`; shader diagnostics and asynchronous
pipeline completion are checked before handles are admitted. The device also
requires `float32-filterable`, matching M11's filtered RGBA32F environment-map
contract instead of deferring an unsupported-format error until drawing, and
negotiates depth-clip-control when available. Indexed triangle strips pin their
primitive-restart index format at pipeline creation and reject mismatched index
bindings before native encoding. Its
Chromium CTest gate attaches diagnostics before navigation, requires zero
console/runtime/WebGPU errors, verifies the full 36-value M13 paired-`u32`
CPU/GPU vector, proves the WebGPU deferred-reclamation contract by destroying a
submitted input immediately, and executes 130 resource-bound indexed PBR-cube
draws per frame. It then forces depth-backed canvas target recreation, audits
the visible cube for nonblank coverage, bounds, and color diversity, and
requires three byte-identical post-resize captures. The real `Renderer::render()`
M11 fixture matches the locked native image byte-for-byte in Chromium. Cornell,
PBR spheres, mipmapping, z-fighting, alpha order, normal-map orientation, hybrid
deferred-opaque/forward-transparent composition, and the voxel/height/hex terrain
gallery each pass three-frame stability and pixel-diversity gates; all captures
were visually inspected with no missing or blinking faces.

### Current next work (2026-09-03 audit)

1. Register a Metal-capable Apple Silicon runner with the workflow's
   `self-hosted`, `macOS`, `ARM64`, and `metal` labels.
2. Dispatch the Metal-hardware acceptance job and admit M17 only after clean API
   and shader diagnostics, swapchain resize/presentation, the locked M11
   full-frame gate, M13 arithmetic parity, and visual inspection of all 27
   repeated captures.
3. Resume the remaining M12 profiling, occlusion, multithreaded recording, SIMD,
   render-graph scheduling, and async-compute work after backend conformance is
   closed, unless profiling identifies a release blocker first.

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
  (`cellsDirty`, `meshDirty`, `bordersDirty`). Workers compute immutable stage
  results; the main thread advances stages after version validation and never
  exposes an in-place worker mutation (see 11.5).
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
- **Storage v2 (completed in M16)**: per-chunk palette + bit-packed
  indices — ≤256 distinct blocks pack to 8-bit indices (32 KiB), typical
  terrain palettes ≤16 blocks pack to 4-bit (16 KiB). Chunks with more than
  256 ids retain dense u16 storage; sparse delta serialization is unchanged.
- **Block registry**: u16 id → `{solid, opaque, textureLayer, hardness}`.
  ids 0..255 engine-reserved (0 air, 1 bedrock, …); games own ≥256.
- **Meshing input**: padded fetch window of 34³ built once per remesh from
  the chunk plus one-cell slabs, edge lines, and corners from up to all 26
  adjacent chunks. Face samples drive culling; edge/corner samples drive the
  completed M15 baked-AO calculation and its neighbor dirty propagation.
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
- M16's scheduler runs pure cell generation and snapshot meshing on fixed
  workers. Chunk-map mutation, overlay replay, dirty propagation, and
  version-checked mesh publication remain on the main thread, so cancellation
  is stale-result rejection rather than shared-buffer mutation.

### 11.6 Determinism & float parity

M13 CPU noise must produce BIT-IDENTICAL output across GCC/MSVC/Clang. GPU
kernels must reproduce the integer hash exactly and match CPU floating-point
samples within the calibrated parity tolerance (extends the M1.5 contract from
fixtures to worlds). Two rules enforce that contract:

1. **Compile discipline**: `-ffp-contract=off` (GCC/Clang) and `/fp:precise`
   (MSVC) on every current engine/test target via `cmake/FloatPrecision.cmake`.
   FMA contraction differences are the #1 silent cross-toolchain divergence;
   the GCC/MSVC/Clang Windows/Linux execution matrix now passes as recorded in
   §8.
2. **Kernel discipline**: hot-path noise avoids transcendental functions;
   gradient/permutation tables come from integer hashes, not sin/cos. GPU
   kernels implement the 64-bit hash with portable 32-bit pairs. Float parity
   is calibrated exactly like M8: the `1.78814e-7` maximum measured delta x4
   remains inside the centralized `1e-5` tolerance and is enforced by M13 tests.

### 11.7 M14 delivery order

1. `floorDiv/floorMod` helpers, `ChunkCoord`, hashing
2. Chunk frame: lifecycle state machine + delta overlay + purity-based
   regeneration
3. VoxelPolicy end-to-end: placeholder-noise gen → edits → serialize →
   reload-equality test
4. HexPolicy topology + corner lattice
5. HeightPolicy tiles + shared-edge normals

(Order starts with the most demanding future consumer of M15 meshing.)

## 12. Asset IO Ladder (M5.1–M5.9)

Full-spectrum format support, dependency-ordered, one rung per sub-milestone.
Every rung: pinned deps per §2 discipline, six-preset gate, plan-doc status
update in the table below.

| Rung | Formats | Strategy | Codec status |
|------|---------|----------|--------|
| M5.1 | BMP, TGA | Hand-written decoders (~150 lines) — proves the loader seam zero-dep | ✅ |
| M5.2 | PNG, JPG, HDR | Vendor stb_image (public domain) | ✅ |
| M5.3 | glTF / GLB | Vendor cgltf (MIT): mesh primitives/submeshes, normalized vertex streams, core metallic-roughness materials, texture references, and primitive-material mappings; skins/animations deferred | ✅ |
| M5.4 | OpenEXR | Minimal scanline half-float subset by hand; vendor full OpenEXR only if tiled/deep needed | ✅ |
| M5.5 | DDS container + BC1–BC7 decode | Container by hand; block decoders by hand from published specs; decoded output = RGBA16F/32F where HDR | ✅ |
| M5.6 | KTX2 container + BasisU transcode | Container by hand; raw Vulkan formats copy/convert to CPU textures; ETC1S/UASTC transcode to RGBA; zstd and GPU-native output are deferred | ✅ |
| M5.7 | ASTC decode | Vendor reference decoder; note Vulkan can consume ASTC natively — decode path exists for CPU sampling | ✅ |
| M5.8 | OBJ (fuller subset), FBX | OBJ by hand (mtl-lite); FBX via ufbx (single-file MIT — no proprietary SDK) | ✅ |
| M5.9 | USD, Alembic | Alembic (Ogawa-only, official tree)+Imath; USD via tinyusdz (zero-dep single-file reader — full OpenUSD rejected: TBB+OpenSubdiv+Boost). Fixtures authored in-test via writer APIs / dep-tree fixtures. Heaviest rung by an order of magnitude — deliberately last so nothing blocks behind it | ✅ |

Cross-rung notes:
- A ✅ in this table means the documented codec subset is implemented and
  tested. M5's parent-level dependency-license requirement is separately closed
  by `THIRD_PARTY_NOTICES.md`, as recorded in §8.
- Compressed textures ride GPU-native through M9+ backends wherever hardware
  accepts them; software decode exists for CPU sampling regardless of backend.
- Every decoder feeds the SAME Texture/Mesh records from M5.0 — no format ever
  leaks above the loader seam.
- Renderer track (M6+) resumes only after this table is fully ✓.
