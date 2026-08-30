#pragma once

#include <cstdint>
#include <span>

#include "graphics/command/command_buffer.h"
#include "graphics/device/device.h"
#include "graphics/gfx_error.h"
#include "graphics/hud/hud_font.h"

namespace f4::gfx::hud {

// One styled text row. (x, y) is the top-left of the first glyph in logical
// pixels; glyphs advance left-to-right by a fixed cell width.
struct HudLine {
    float x = 0.0f;
    float y = 0.0f;
    const char* text = nullptr;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

// Minimal HUD overlay: draws text as instanced quads sampling the embedded
// font atlas, with a fixed orthographic (pixel) projection and alpha blending.
// This is diagnostic infrastructure only — it owns its own fixed pipeline and
// resources and is deliberately NOT part of the M8 material/render-graph design.
//
// Usage: call init() once, then per frame call setLines() to record the text
// and render(&cb, w, h) inside the app's render pass to append the overlay pass.
class Overlay {
public:
    explicit Overlay(IDevice* dev) : dev_(dev), font_(dev) {}
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    Overlay(Overlay&&) = delete;
    Overlay& operator=(Overlay&&) = delete;

    // Build the font atlas + all overlay GPU resources. `maxGlyphs` sizes the
    // persistent per-instance buffer (no per-frame allocation); `scale` is the
    // uniform glyph multiplier (1.0 = 5x7 px glyphs).
    GfxError init(std::uint32_t maxGlyphs, float scale);

    // Buffer the glyph instances for `lines`, clamped to the glyph capacity.
    // Returns the number of glyphs actually stored. Call before render().
    std::uint32_t setLines(std::span<const HudLine> lines);

    // Append the overlay pass to `cb` using the given drawable size. Rebuilds
    // the ortho projection UBO for this size, then binds pipeline/resources and
    // issues one instanced draw for the recorded glyphs.
    void render(CommandBuffer& cb, std::uint32_t width, std::uint32_t height);

    [[nodiscard]] std::uint32_t maxGlyphs() const { return maxGlyphs_; }

private:
    struct Instance;

    IDevice* dev_ = nullptr;
    Font font_;
    float scale_ = 1.0f;
    std::uint32_t maxGlyphs_ = 0;
    std::uint32_t pendingGlyphs_ = 0;

    BufferHandle quadVb_{};
    BufferHandle quadIb_{};
    BufferHandle instBuf_{};
    BufferHandle ubo_{};
    SamplerHandle sampler_{};
    PipelineHandle pipeline_{};
    bool ready_ = false;
};

} // namespace f4::gfx::hud
