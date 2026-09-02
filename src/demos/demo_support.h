#pragma once

#include "core/containers/array.h"
#include "core/containers/string.h"
#include "core/diagnostics/diagnostics.h"
#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/hud/hud_overlay.h"
#include "platform/window.h"

namespace demo {

// RAII window + OpenGL context + RHI device. Mirrors the f4_app setup so every
// demo focuses on its own behavior instead of boilerplate. Not owned by the
// diagnostics layer — the renderer only supplies metrics.
class DemoWindow {
public:
    static constexpr int kWidth = 900;
    static constexpr int kHeight = 600;

    explicit DemoWindow(const char* title, int width = kWidth, int height = kHeight, bool resizable = true);
    ~DemoWindow();

    DemoWindow(const DemoWindow&) = delete;
    DemoWindow& operator=(const DemoWindow&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] f4::platform::Window* window() const { return window_; }
    [[nodiscard]] f4::gfx::IDevice* device() const { return device_; }

private:
    f4::platform::Window* window_ = nullptr;
    f4::gfx::IDevice* device_ = nullptr;
    bool ok_ = false;
};

// Installs itself as a Diagnostics SampleSink: each sample snapshots the
// registry into HudLines for the overlay. The overlay is a graphics object; the
// Diagnostics layer stays renderer-independent and only hands us the metrics.
class Hud {
public:
    explicit Hud(f4::gfx::IDevice* dev) : overlay_(dev) {}
    ~Hud() = default;
    Hud(const Hud&) = delete;
    Hud& operator=(const Hud&) = delete;

    // Create overlay resources. Returns false on device failure.
    bool init(std::uint32_t maxGlyphs = 4096, float scale = 1.5f);

    // Diagnostics SampleSink entry point.
    static void onSample(const f4::diag::MetricRegistry& metrics, void* userData);

    // Emit the overlay pass into `cb` for the given drawable size.
    void render(f4::gfx::CommandBuffer& cb, std::uint32_t width, std::uint32_t height);

private:
    void rebuild(const f4::diag::MetricRegistry& metrics);

    f4::gfx::hud::Overlay overlay_;
    f4::containers::Array<f4::gfx::hud::HudLine> lines_;
    f4::containers::Array<f4::containers::String> texts_;
};

} // namespace demo
