// f4 Demo Bench — unified demo runner with free camera and hot-swappable scenes.
// Controls:
//   1-9          : Switch to scene 0-8
//   Shift+1-9    : Switch to scene 9-17
//   Tab          : Cycle scenes
//   ESC          : Release mouse / quit (press twice)
//   LMB          : Capture mouse (free camera)
//   WASD + Mouse : Free camera movement
//   Shift        : Sprint (camera)
//   Space/Ctrl   : Up/Down (camera)
//   F11          : Toggle fullscreen

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>

#include "demos/demo_bench.h"
#include "demos/demo_triangle.h"
#include "demos/demo_instancing.h"
#include "demos/demo_resize.h"
#include "demos/demo_collision.h"
#include "demos/demo_assets.h"
#include "demos/demo_hud.h"
#include "demos/demo_ecs.h"
#include "demos/demo_cube.h"
#include "demos/demo_terrain.h"
#include "demos/demo_particles.h"
#include "demos/demo_backface.h"
#include "demos/demo_gizmo.h"
#include "demos/demo_hierarchy.h"
#include "demos/demo_cornell_box.h"
#include "demos/demo_render_validation.h"

using namespace f4::demo;

int main(int argc, char** argv) {
    DemoBench bench("f4 Demo Bench", 1280, 720);
    int initialScene = 0;
    DemoRunOptions options;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            options.smokeFramesPerScene = 3;
            if (i + 1 < argc) {
                char* end = nullptr;
                const long requestedFrames = std::strtol(argv[i + 1], &end, 10);
                if (end != argv[i + 1] && *end == '\0') {
                    if (requestedFrames <= 0) {
                        std::fprintf(stderr, "Smoke frame count must be positive\n");
                        return 2;
                    }
                    options.smokeFramesPerScene = static_cast<std::size_t>(requestedFrames);
                    ++i;
                }
            }
        } else if (std::strcmp(argv[i], "--capture-dir") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--capture-dir requires a path\n");
                return 2;
            }
            options.captureDirectory = argv[++i];
        } else if (std::strcmp(argv[i], "--static") == 0) {
            options.staticCapture = true;
        } else if (std::strcmp(argv[i], "--only-scene") == 0) {
            options.onlyScene = true;
        } else if (std::strcmp(argv[i], "--static-view") == 0) {
            if (i + 1 >= argc) return 2;
            char* end = nullptr;
            const long view = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || view < 0 || view > 6) return 2;
            options.staticView = static_cast<std::uint32_t>(view);
        } else {
            char* end = nullptr;
            const long scene = std::strtol(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || scene < 0 ||
                scene > std::numeric_limits<int>::max()) {
                std::fprintf(stderr,
                             "Usage: demo_bench [scene] [--smoke [frames]] "
                             "[--capture-dir path] [--static] [--only-scene] [--static-view 0..6]\n");
                return 2;
            }
            initialScene = static_cast<int>(scene);
        }
    }
    if ((options.captureDirectory != nullptr || options.staticCapture || options.onlyScene) &&
        options.smokeFramesPerScene == 0) options.smokeFramesPerScene = 3;
    if (options.staticView != 0 &&
        (!options.staticCapture || !options.onlyScene || initialScene != 7)) {
        std::fprintf(stderr, "--static-view requires cube scene 7, --static and --only-scene\n");
        return 2;
    }

    // Register all available scenes (in order: 0-8, then 9-17 with Shift)
    bench.addScene(new TriangleScene(), "One complete RGB triangle; smooth interpolation; no holes.");
    bench.addScene(new InstancingScene(), "Regular instance grid; no missing cells or shared-transform collapse.");
    bench.addScene(new ResizeScene(), "Right triangle anchored at screen center; equal legs. Resize is a separate gate.");
    bench.addScene(new CollisionScene(), "Separated colored bodies at the fixed simulation checkpoint.");
    bench.addScene(new AssetsScene(), "Texture and geometry intact. This is not full asset-format acceptance.");
    bench.addScene(new HudScene(), "Complete triangle and legible fixed text; no clipped glyphs.");
    bench.addScene(new EcsScene(), "Visible triangle grid; per-instance transforms retained; no missing in-frustum items.");
    bench.addScene(new CubeScene(), "Closed cube; overview shows three faces. Views 1-6 must each show a full face.");
    bench.addScene(new TerrainScene(), "Voxel, height and hex meshes: closed surfaces, clean seams and no holes.");
    bench.addScene(new ParticleScene(), "Fixed seeded particle cloud; overlapping billboards; no empty emitter.");
    bench.addScene(new BackfaceScene(), "Opposite winding controls: front-facing surface visible, rear-facing culled.");
    bench.addScene(new GizmoScene(), "X red, Y green, Z blue; asymmetric F distinguishes handedness.");
    bench.addScene(new HierarchyScene(), "Yellow sun, blue planet and gray moon; partial moon occlusion is expected.");
    bench.addScene(new CornellBoxScene(), "Closed room, red/green walls and two boxes; no missing wall or box faces.");
    bench.addScene(new PbrSphereGridScene(), "30 spheres, same base color. Roughness increases RIGHT; metallic increases UP.");
    bench.addScene(new MipmappingScene(), "LEFT: intentional moire. RIGHT: trilinear minification should reduce aliasing.");
    bench.addScene(new ZFightingScene(), "Separation increases LEFT to RIGHT; coplanar conflict on the left is intentional.");
    bench.addScene(new AlphaOrderScene(), "LEFT: back-to-front blending. RIGHT: intentionally reversed order.");
    bench.addScene(new NormalMapOrientationScene(), "LEFT: +Y normal map. RIGHT: deliberately flipped Y; lighting must differ.");
    bench.addScene(new HybridRenderingScene(), "Opaque depth retained; transparent geometry blended in front and occluded behind.");

    if (initialScene >= static_cast<int>(bench.sceneCount())) {
        std::fprintf(stderr, "Scene index out of range\n");
        return 2;
    }
    if (initialScene > 0 &&
        !bench.setScene(static_cast<std::size_t>(initialScene))) {
        std::fprintf(stderr, "Failed to initialize scene %d\n", initialScene);
        return 1;
    }
    return bench.run(options);
}
