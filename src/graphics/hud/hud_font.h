#pragma once

#include <cstdint>

#include "graphics/device/device.h"
#include "graphics/gfx_error.h"
#include "graphics/handle.h"
#include "graphics/texture/texture_desc.h"

namespace f4::gfx::hud {

// Normalized atlas UVs plus pixel advance for a single glyph.
struct Glyph {
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    float advance = 0.0f; // advance in texture pixels (glyph width + 1 pad)
};

// Immutable 1-bit 5x7 embedded font (hud_font_data.h). On init() it builds a
// tiny RGBA8 atlas texture on the device — the glyphs' coverage lives in the
// alpha channel so text is tinted at draw time. The 95 printable ASCII glyphs
// are laid out 16x6 in cells of 6x8 px (5x7 glyph + 1 px right/bottom pad).
class Font {
public:
    static constexpr std::uint32_t kCols = 16;
    static constexpr std::uint32_t kRows = 6;
    static constexpr std::uint32_t kCellW = 6; // 5 glyph + 1 pad
    static constexpr std::uint32_t kCellH = 8; // 7 glyph + 1 pad
    static constexpr std::uint32_t kAtlasW = kCols * kCellW; // 96
    static constexpr std::uint32_t kAtlasH = kRows * kCellH; // 48

    explicit Font(IDevice* dev) : dev_(dev) {}
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    Font(Font&&) noexcept;
    Font& operator=(Font&&) noexcept;

    GfxError init();
    [[nodiscard]] TextureHandle texture() const { return tex_; }
    [[nodiscard]] std::uint32_t textureWidth() const { return kAtlasW; }
    [[nodiscard]] std::uint32_t textureHeight() const { return kAtlasH; }
    [[nodiscard]] const Glyph& glyph(char c) const;

private:
    IDevice* dev_ = nullptr;
    TextureHandle tex_{};
    Glyph glyphs_[95];
};

// Maps an arbitrary char to a glyph index [0,95) in the embedded font.
[[nodiscard]] std::uint32_t glyphIndex(char c);

} // namespace f4::gfx::hud
