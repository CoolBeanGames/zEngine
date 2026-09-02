#pragma once

#include "Render2D.h"

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

// A lazily-built glyph atlas for the 2D text path. Rendered once from the system
// UI font with GDI into an RGBA bitmap (white, alpha = coverage); the renderer
// uploads it as an ordinary texture and expands TextDraws into sprites through
// Layout(). No Direct3D dependency, so the layout maths is unit-testable.
class FontAtlas
{
public:
    struct Glyph
    {
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0; // atlas UVs
        float width = 0, height = 0;          // bitmap size, atlas pixels
        float advance = 0;                    // pen advance, atlas pixels
    };

    FontAtlas() = default;
    // Builds from "Segoe UI" (falls back to the default GUI font) at `pixelHeight`.
    static FontAtlas Build(int pixelHeight);

    bool Valid() const noexcept { return width_ > 0 && height_ > 0; }
    int Width() const noexcept { return width_; }
    int Height() const noexcept { return height_; }
    int PixelHeight() const noexcept { return pixelHeight_; }
    float LineHeight() const noexcept { return lineHeight_; }
    const std::vector<std::uint8_t>& Pixels() const noexcept { return pixels_; } // RGBA

    bool Has(char32_t codepoint) const;
    const Glyph& GlyphFor(char32_t codepoint) const; // missing -> '?' -> space

    // Advance width of a single line of UTF-8 text at the requested pixel height.
    float Measure(std::string_view utf8, float pixelHeight) const;

    // Expands one UTF-8 run into sprites against `texture`. The pen starts at the
    // top-left cell corner (x, y); '\n' returns the pen and advances a line.
    std::vector<SpriteDraw> Layout(std::string_view utf8, float x, float y,
                                   float pixelHeight, Float4 color,
                                   TextureHandle texture) const;

private:
    int width_ = 0, height_ = 0, pixelHeight_ = 0;
    float lineHeight_ = 0;
    std::vector<std::uint8_t> pixels_;
    std::unordered_map<char32_t, Glyph> glyphs_;
};
