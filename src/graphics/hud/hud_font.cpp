#include "graphics/hud/hud_font.h"

#include <cstring>
#include <vector>

#include "graphics/hud/hud_font_data.h"

namespace f4::gfx::hud {

std::uint32_t glyphIndex(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (uc < static_cast<unsigned char>(kFontFirst) ||
        uc > static_cast<unsigned char>(kFontLast)) {
        return glyphIndex('?');
    }
    return uc - static_cast<unsigned char>(kFontFirst);
}

Font::~Font() {
    if (dev_ != nullptr && tex_.index != 0) {
        dev_->destroyTexture(tex_);
        tex_ = TextureHandle{};
    }
}

Font::Font(Font&& o) noexcept {
    dev_ = o.dev_;
    tex_ = o.tex_;
    for (std::uint32_t i = 0; i < 95; ++i) {
        glyphs_[i] = o.glyphs_[i];
    }
    o.dev_ = nullptr;
    o.tex_ = TextureHandle{};
}

Font& Font::operator=(Font&& o) noexcept {
    if (this != &o) {
        if (dev_ != nullptr && tex_.index != 0) {
            dev_->destroyTexture(tex_);
        }
        dev_ = o.dev_;
        tex_ = o.tex_;
        for (std::uint32_t i = 0; i < 95; ++i) {
            glyphs_[i] = o.glyphs_[i];
        }
        o.dev_ = nullptr;
        o.tex_ = TextureHandle{};
    }
    return *this;
}

GfxError Font::init() {
    if (dev_ == nullptr) {
        return GfxError::BadDescriptor;
    }

    // Precompute per-glyph UV rects + advances from the fixed cell layout.
    for (std::uint32_t g = 0; g < 95; ++g) {
        const std::uint32_t col = g % kCols;
        const std::uint32_t row = g / kCols;
        const float u0 = static_cast<float>(col * kCellW) / static_cast<float>(kAtlasW);
        const float v0 = static_cast<float>(row * kCellH) / static_cast<float>(kAtlasH);
        // Cover only the 5x7 lit area, not the padded cell.
        glyphs_[g].u0 = u0;
        glyphs_[g].v0 = v0;
        glyphs_[g].u1 = u0 + static_cast<float>(kFontGlyphWidth) / static_cast<float>(kAtlasW);
        glyphs_[g].v1 = v0 + static_cast<float>(kFontGlyphHeight) / static_cast<float>(kAtlasH);
        glyphs_[g].advance = static_cast<float>(kCellW);
    }

    // Rasterize the 1-bit font into RGBA coverage (alpha only; text is tinted
    // at draw time). 96 x 48 x 4 bytes.
    std::vector<std::byte> pixels(kAtlasW * kAtlasH * 4, std::byte(0));
    for (std::uint32_t g = 0; g < 95; ++g) {
        const std::uint32_t col = g % kCols;
        const std::uint32_t row = g / kCols;
        for (std::uint32_t r = 0; r < kFontGlyphHeight; ++r) {
            const std::uint8_t bits = kFontRows[g][r];
            for (std::uint32_t c = 0; c < kFontGlyphWidth; ++c) {
                const bool lit = ((bits >> (4 - c)) & 0x1u) != 0u;
                if (!lit) {
                    continue;
                }
                const std::uint32_t px = col * kCellW + c;
                const std::uint32_t py = row * kCellH + r;
                const std::size_t i = (static_cast<std::size_t>(py) * kAtlasW + px) * 4;
                pixels[i + 0] = std::byte(0xFF);
                pixels[i + 1] = std::byte(0xFF);
                pixels[i + 2] = std::byte(0xFF);
                pixels[i + 3] = std::byte(0xFF);
            }
        }
    }

    TextureDesc td;
    td.width = kAtlasW;
    td.height = kAtlasH;
    td.format = TextureFormat::RGBA8_UNORM;
    td.mipLevels = 1;
    auto res = dev_->createTexture(td);
    if (!res.hasValue()) {
        return res.error();
    }
    tex_ = res.value();
    if (!pixels.empty() &&
        dev_->writeTexture(tex_, 0, std::span(pixels.data(), pixels.size())) != GfxError::None) {
        dev_->destroyTexture(tex_);
        tex_ = TextureHandle{};
        return GfxError::BadDescriptor;
    }
    return GfxError::None;
}

const Glyph& Font::glyph(char c) const {
    return glyphs_[glyphIndex(c)];
}

} // namespace f4::gfx::hud
