# Static Screenshot Checks

The OpenGL demo bench now has a separate static inspection mode. Normal demos
still animate and accept input. Static mode uses 60 fixed 1/60-second updates,
zero keyboard input, fixed cameras, and then freezes simulation. It replaces
live timing HUD values with scene names and explicit inspection criteria.
Closing or resizing a static run before completion fails the capture.

## Run

From the repository root in PowerShell:

```powershell
cmake --build --preset build-release
ctest --preset test-release -R static_demo_capture
```

The gate captures 20 overview scenes plus six cube directions, three frames
each, in two separate processes per fixture: 156 BMPs total. It compares SHA-256
hashes both within each fixture and across processes, and tests invalid CLI
arguments. Output lives in `build/release/static-demo-captures/<run-id>/` with
an `index.html` gallery. New run directories prevent stale captures from passing.
The capture path drains the submitted OpenGL frame before `glReadPixels`, so
driver completion timing cannot create a one-pixel false failure in the HUD.
The gate needs an OpenGL-capable display; Linux CI runs it under Xvfb.

To capture one fixture or inspect a cube face:

```powershell
& ./build/release/src/demos/demo_bench.exe 14 --static --only-scene --capture-dir ./build/static-pbr
& ./build/release/src/demos/demo_bench.exe 7 --static --only-scene --static-view 2 --capture-dir ./build/static-cube-back
```

Cube view IDs: 0 overview, 1 front (+Z), 2 back (-Z), 3 left (-X), 4 right
(+X), 5 top (+Y), 6 bottom (-Y). Top/bottom cameras have a tiny tilt to avoid a
degenerate look-at up vector. Views 1-6 require cube scene 7 and `--only-scene`.
Static runs default to three frames; `--smoke N` overrides that count.

## Inspect

| Scene | What a capture confirms |
| --- | --- |
| Cube | Three connected faces in overview; one full square from each axial view. No half-face holes or diagonals leaking background. |
| Cornell | Fully framed room opening, floor, ceiling, back wall, colored side walls, and two boxes. This fixture is not a global-illumination reference. |
| PBR grid | 30 separated spheres, identical base hue, roughness increasing rightward and metallic increasing upward. Static-only layout removes overlap and camera skew. |
| Mipmapping | Intentionally aliased left panel versus filtered right panel. Some near-camera detail on the right is expected. |
| Z-fighting | Six increasing depth separations. Coplanar/near-coplanar left controls may conflict; the separated right panels should be coherent. |
| Alpha order | Left is back-to-front; right is intentionally reversed. The differing overlap colors are the test, not a regression. |
| Normal mapping | +Y on left, inverted Y on right; highlights must differ. The right-hand inverted bumps are intentional. |
| Hybrid | Deferred opaque geometry plus forward transparent layers, with opaque depth controlling occlusion. This is a demo path, not the production renderer's default. |
| Terrain | Voxel, height and hex surface coverage, borders, skirts, and seams. This is a mesher gallery, not an interactive WorldStreamer demo. |

The remaining scenes cover basic triangles, buffers/instancing, HUD glyphs,
collision snapshots, assets, batching/culling, particles, winding, axes, and
hierarchy transforms.

## Limits

- Identical pixels prove repeatability on this build/device, not correctness.
- Frozen scenes do not prove animation, camera-motion stability, resize behavior,
  streaming progress, resource exhaustion, or mirrored-transform correctness.
- Keep animated smoke tests, native GPU parity, and numerical/unit tests too.
- The static hook is opt-in. Existing WebGPU/Metal reference runners do not use
  it, so their established camera/layout fixtures are unchanged.
- Metal runtime and visual acceptance still require Apple hardware.

The audit follow-up repairs gizmo Y-face winding and validates every gizmo
triangle during initialization. Production reflected-instance correctness is
covered separately by native PBR/shadow readback and WebGPU browser regressions;
the ordinary positive-scale cube is still not its acceptance fixture. See the
[audit and fix record](personal-check-2026-09-05.md).
