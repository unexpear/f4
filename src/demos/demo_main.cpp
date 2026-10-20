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
#include "demos/demo_night_lights.h"

using namespace f4::demo;

int main(int argc, char** argv) {
    DemoBench bench("f4 Demo Bench", 1280, 720);
    int initialScene = 0;
    std::size_t smokeFrames = 0;
    const char* captureDirectory = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            smokeFrames = 3;
            if (i + 1 < argc) {
                char* end = nullptr;
                const long requestedFrames = std::strtol(argv[i + 1], &end, 10);
                if (end != argv[i + 1] && *end == '\0') {
                    if (requestedFrames <= 0) {
                        std::fprintf(stderr, "Smoke frame count must be positive\n");
                        return 2;
                    }
                    smokeFrames = static_cast<std::size_t>(requestedFrames);
                    ++i;
                }
            }
        } else if (std::strcmp(argv[i], "--capture-dir") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--capture-dir requires a path\n");
                return 2;
            }
            captureDirectory = argv[++i];
        } else {
            char* end = nullptr;
            const long scene = std::strtol(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || scene < 0) {
                std::fprintf(stderr,
                             "Usage: demo_bench [scene] [--smoke [frames]] "
                             "[--capture-dir path]\n");
                return 2;
            }
            initialScene = static_cast<int>(scene);
        }
    }
    if (captureDirectory != nullptr && smokeFrames == 0) smokeFrames = 3;

    // Register all available scenes (in order: 0-8, then 9-17 with Shift)
    bench.addScene(new TriangleScene());      // 0 / 9
    bench.addScene(new InstancingScene());    // 1 / 10
    bench.addScene(new ResizeScene());        // 2 / 11
    bench.addScene(new CollisionScene());     // 3 / 12
    bench.addScene(new AssetsScene());        // 4 / 13
    bench.addScene(new HudScene());           // 5 / 14
    bench.addScene(new EcsScene());           // 6 / 15
    bench.addScene(new CubeScene());          // 7 / 16
    bench.addScene(new TerrainScene());       // 8 / 17
    bench.addScene(new ParticleScene());      // 9 / 18 (Shift+1)
    bench.addScene(new BackfaceScene());      // 10 / Shift+2 - 2 opposite triangles cull test, press C/R/Space, rotates
    bench.addScene(new GizmoScene());         // 11 / Shift+3 - X red Y green Z blue, H to toggle RH/LH, F is asymmetrical
    bench.addScene(new HierarchyScene());     // 12 / Shift+4 - 4 nested cubes local->world, N to toggle numbers, auto-rotates
    bench.addScene(new CornellBoxScene());    // 13 / Shift+5 - lit room with two interior boxes
    bench.addScene(new PbrSphereGridScene()); // 14 / Shift+6 - GGX metallic/roughness grid
    bench.addScene(new MipmappingScene());    // 15 / Shift+7 - level-zero vs trilinear minification
    bench.addScene(new ZFightingScene());     // 16 / Shift+8 - increasing coplanar separation
    bench.addScene(new AlphaOrderScene());    // 17 / Shift+9 - sorted vs reversed transparency
    bench.addScene(new NormalMapOrientationScene()); // 18 / Tab or CLI - tangent-space Y orientation
    bench.addScene(new HybridRenderingScene());      // 19 / Tab or CLI - deferred opaque + forward alpha
    bench.addScene(new NightLightsScene());          // 20 / Tab or CLI - clustered point lights, [ ] P M B K

    if (initialScene > 0 && initialScene < static_cast<int>(bench.sceneCount()) &&
        !bench.setScene(static_cast<std::size_t>(initialScene))) {
        std::fprintf(stderr, "Failed to initialize scene %d\n", initialScene);
        return 1;
    }
    return bench.run(smokeFrames, captureDirectory);
}
