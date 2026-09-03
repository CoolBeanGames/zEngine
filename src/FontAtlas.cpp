#include "FontAtlas.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace
{
    constexpr char32_t kFirst = 32;   // space
    constexpr char32_t kLast = 0xFF;  // ZE-122: ASCII + Latin-1 Supplement
    constexpr int kAtlasWidth = 512;
    constexpr int kPadding = 1;

    // ZE-122: common non-Latin-1 symbols used by the UI / editor chrome so they do
    // not fall back to '?'. (Full dynamic coverage comes with TTF support, ZE-123.)
    constexpr char32_t kExtraGlyphs[] = {
        0x2010, 0x2011, 0x2012, 0x2013, 0x2014, // hyphen / dashes (– —)
        0x2018, 0x2019, 0x201C, 0x201D,         // curly quotes
        0x2022, 0x2026,                         // bullet, ellipsis (• …)
        0x2190, 0x2191, 0x2192, 0x2193,         // arrows
        0x25B8, 0x25BE, 0x25C0, 0x25B4,         // triangles (tree carets ▸ ▾)
        0x2610, 0x2611, 0x2612,                 // ballot boxes (☐ ☑ ☒)
        0x2713, 0x2717, 0x2716,                 // check / cross (✓ ✗ ✖)
        0x00D7, 0x00F7,                         // × ÷  (also in Latin-1, harmless dup guard below)
    };

    // Minimal UTF-8 decode; malformed bytes and non-BMP code points collapse to
    // U+FFFD, which the atlas maps to '?'.
    std::vector<char32_t> DecodeUtf8(std::string_view text)
    {
        std::vector<char32_t> out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size();)
        {
            const auto byte = static_cast<unsigned char>(text[i]);
            char32_t cp = 0xFFFD;
            int len = 1;
            if (byte < 0x80) { cp = byte; len = 1; }
            else if ((byte >> 5) == 0x6 && i + 1 < text.size()) { cp = byte & 0x1F; len = 2; }
            else if ((byte >> 4) == 0xE && i + 2 < text.size()) { cp = byte & 0x0F; len = 3; }
            else if ((byte >> 3) == 0x1E && i + 3 < text.size()) { cp = byte & 0x07; len = 4; }
            for (int k = 1; k < len; ++k)
            {
                const auto cont = static_cast<unsigned char>(text[i + k]);
                if ((cont & 0xC0) != 0x80) { cp = 0xFFFD; len = 1; break; }
                cp = (cp << 6) | (cont & 0x3F);
            }
            out.push_back(len == 1 && byte >= 0x80 ? 0xFFFD : cp);
            i += len;
        }
        return out;
    }
}

FontAtlas FontAtlas::Build(int pixelHeight)
{
    FontAtlas atlas;
    atlas.pixelHeight_ = pixelHeight = std::clamp(pixelHeight, 6, 256);

    const HDC screen = GetDC(nullptr);
    const HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc) throw std::runtime_error("FontAtlas: could not create a device context.");

    const HGDIOBJ stockFont = GetStockObject(DEFAULT_GUI_FONT);
    HFONT font = CreateFontW(-pixelHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!font) font = static_cast<HFONT>(stockFont);
    const bool ownsFont = static_cast<HGDIOBJ>(font) != stockFont;
    const HFONT previousFont = static_cast<HFONT>(SelectObject(dc, font));

    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int cellHeight = metrics.tmHeight + kPadding * 2;
    atlas.lineHeight_ = static_cast<float>(metrics.tmHeight + metrics.tmExternalLeading);

    // First pass: measure every glyph and lay out atlas cells.
    struct Cell { char32_t cp; int x, y, w, h; float advance; };
    std::vector<Cell> cells;
    int penX = 0, penY = 0, rowHeight = cellHeight;
    std::vector<char32_t> codepoints;
    for (char32_t cp = kFirst; cp <= kLast; ++cp) codepoints.push_back(cp);
    for (const char32_t cp : kExtraGlyphs) if (cp > kLast) codepoints.push_back(cp);
    for (const char32_t cp : codepoints)
    {
        const wchar_t ch = static_cast<wchar_t>(cp);
        SIZE extent{};
        GetTextExtentPoint32W(dc, &ch, 1, &extent);
        const int glyphWidth = std::max<int>(1, extent.cx) + kPadding * 2;
        if (penX + glyphWidth > kAtlasWidth) { penX = 0; penY += rowHeight; }
        cells.push_back({cp, penX, penY, glyphWidth, cellHeight, static_cast<float>(extent.cx)});
        penX += glyphWidth;
    }
    const int atlasHeight = penY + rowHeight;

    // Create a top-down 32-bit DIB and render the glyphs into it.
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = kAtlasWidth;
    info.bmiHeader.biHeight = -atlasHeight; // negative -> top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib)
    {
        SelectObject(dc, previousFont);
        if (ownsFont) DeleteObject(font);
        DeleteDC(dc);
        throw std::runtime_error("FontAtlas: could not allocate the atlas bitmap.");
    }
    const HBITMAP previousBitmap = static_cast<HBITMAP>(SelectObject(dc, dib));
    const RECT full{0, 0, kAtlasWidth, atlasHeight};
    FillRect(dc, &full, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SelectObject(dc, font);
    for (const auto& cell : cells)
    {
        const wchar_t ch = static_cast<wchar_t>(cell.cp);
        ExtTextOutW(dc, cell.x + kPadding, cell.y + kPadding, 0, nullptr, &ch, 1, nullptr);
    }
    GdiFlush();

    // Copy coverage (any channel; the glyphs are drawn white on black) into RGBA.
    atlas.width_ = kAtlasWidth;
    atlas.height_ = atlasHeight;
    atlas.pixels_.assign(static_cast<std::size_t>(kAtlasWidth) * atlasHeight * 4, 0);
    const auto* source = static_cast<const std::uint8_t*>(bits);
    for (int y = 0; y < atlasHeight; ++y)
    {
        for (int x = 0; x < kAtlasWidth; ++x)
        {
            const std::size_t si = (static_cast<std::size_t>(y) * kAtlasWidth + x) * 4;
            const std::uint8_t coverage = source[si]; // B == G == R for white-on-black
            const std::size_t di = si;
            atlas.pixels_[di + 0] = 255;
            atlas.pixels_[di + 1] = 255;
            atlas.pixels_[di + 2] = 255;
            atlas.pixels_[di + 3] = coverage;
        }
    }

    const float fw = static_cast<float>(kAtlasWidth), fh = static_cast<float>(atlasHeight);
    for (const auto& cell : cells)
    {
        Glyph glyph;
        glyph.u0 = cell.x / fw;
        glyph.v0 = cell.y / fh;
        glyph.u1 = (cell.x + cell.w) / fw;
        glyph.v1 = (cell.y + cell.h) / fh;
        glyph.width = static_cast<float>(cell.w);
        glyph.height = static_cast<float>(cell.h);
        glyph.advance = cell.advance;
        atlas.glyphs_.emplace(cell.cp, glyph);
    }

    SelectObject(dc, previousBitmap);
    SelectObject(dc, previousFont);
    DeleteObject(dib);
    if (ownsFont) DeleteObject(font);
    DeleteDC(dc);
    return atlas;
}

bool FontAtlas::Has(char32_t codepoint) const
{
    return glyphs_.find(codepoint) != glyphs_.end();
}

const FontAtlas::Glyph& FontAtlas::GlyphFor(char32_t codepoint) const
{
    if (const auto it = glyphs_.find(codepoint); it != glyphs_.end()) return it->second;
    if (const auto it = glyphs_.find(U'?'); it != glyphs_.end()) return it->second;
    static const Glyph empty{};
    if (const auto it = glyphs_.find(U' '); it != glyphs_.end()) return it->second;
    return empty;
}

float FontAtlas::Measure(std::string_view utf8, float pixelHeight) const
{
    if (!Valid()) return 0;
    const float scale = pixelHeight / static_cast<float>(pixelHeight_);
    float widest = 0, line = 0;
    for (const char32_t cp : DecodeUtf8(utf8))
    {
        if (cp == U'\n') { widest = std::max(widest, line); line = 0; continue; }
        line += GlyphFor(cp).advance * scale;
    }
    return std::max(widest, line);
}

std::vector<SpriteDraw> FontAtlas::Layout(std::string_view utf8, float x, float y,
                                          float pixelHeight, Float4 color,
                                          TextureHandle texture) const
{
    std::vector<SpriteDraw> sprites;
    if (!Valid()) return sprites;
    const float scale = pixelHeight / static_cast<float>(pixelHeight_);
    float penX = x, penY = y;
    for (const char32_t cp : DecodeUtf8(utf8))
    {
        if (cp == U'\n') { penX = x; penY += lineHeight_ * scale; continue; }
        const Glyph& glyph = GlyphFor(cp);
        if (cp != U' ' && glyph.width > 0 && glyph.height > 0)
        {
            SpriteDraw sprite;
            sprite.texture = texture;
            sprite.dest = {penX, penY, glyph.width * scale, glyph.height * scale};
            sprite.region = {glyph.u0, glyph.v0, glyph.u1, glyph.v1};
            sprite.tint = color;
            sprites.push_back(sprite);
        }
        penX += glyph.advance * scale;
    }
    return sprites;
}
