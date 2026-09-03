#include "Render2D.h"
#include "FontAtlas.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    bool Near(float a, float b, float epsilon = 0.01f) { return std::fabs(a - b) <= epsilon; }

    struct Bounds { float minX, minY, maxX, maxY, minU, minV, maxU, maxV; };

    Bounds Measure(const std::vector<SpriteVertex>& verts)
    {
        Bounds b{verts.front().position.x, verts.front().position.y, verts.front().position.x, verts.front().position.y,
                 verts.front().uv.x, verts.front().uv.y, verts.front().uv.x, verts.front().uv.y};
        for (const auto& v : verts)
        {
            b.minX = std::min(b.minX, v.position.x); b.maxX = std::max(b.maxX, v.position.x);
            b.minY = std::min(b.minY, v.position.y); b.maxY = std::max(b.maxY, v.position.y);
            b.minU = std::min(b.minU, v.uv.x); b.maxU = std::max(b.maxU, v.uv.x);
            b.minV = std::min(b.minV, v.uv.y); b.maxV = std::max(b.maxV, v.uv.y);
        }
        return b;
    }

    bool HasVertexNear(const std::vector<SpriteVertex>& verts, float x, float y)
    {
        return std::any_of(verts.begin(), verts.end(), [&](const SpriteVertex& v) {
            return Near(v.position.x, x) && Near(v.position.y, y);
        });
    }
}

void PlainQuad()
{
    SpriteDraw draw;
    draw.dest = {10, 20, 100, 50};
    draw.tint = {0.2f, 0.4f, 0.6f, 0.8f};
    std::vector<SpriteVertex> verts;
    AppendSprite(verts, draw, 64, 64);
    Check(verts.size() == 6, "A plain sprite must be two triangles");
    const auto b = Measure(verts);
    Check(Near(b.minX, 10) && Near(b.minY, 20) && Near(b.maxX, 110) && Near(b.maxY, 70), "Plain sprite covers the wrong rect");
    Check(Near(b.minU, 0) && Near(b.minV, 0) && Near(b.maxU, 1) && Near(b.maxV, 1), "Plain sprite must map the whole texture");
    for (const auto& v : verts)
        Check(Near(v.color.x, 0.2f) && Near(v.color.w, 0.8f), "Tint must reach every vertex");
}

void RegionCrop()
{
    SpriteDraw draw;
    draw.dest = {0, 0, 40, 40};
    draw.region = {0.25f, 0.0f, 0.5f, 1.0f};
    std::vector<SpriteVertex> verts;
    AppendSprite(verts, draw, 100, 100);
    const auto b = Measure(verts);
    Check(Near(b.minU, 0.25f) && Near(b.maxU, 0.5f) && Near(b.minV, 0) && Near(b.maxV, 1), "Region did not crop the UVs");
    Check(Near(b.minX, 0) && Near(b.maxX, 40), "Region must not change the destination rect");
}

void RotationAboutPivot()
{
    SpriteDraw draw;
    draw.dest = {10, 20, 100, 50};   // centre (60, 45)
    draw.rotationDegrees = 90;
    draw.pivot = {0.5f, 0.5f};
    std::vector<SpriteVertex> verts;
    AppendSprite(verts, draw, 1, 1);
    // The top-left corner (10, 20) rotates 90 deg clockwise about (60, 45) -> (85, -5).
    Check(HasVertexNear(verts, 85, -5), "90 deg rotation about the pivot is wrong");
    // A full rotation set still spans the same area, just transposed.
    const auto b = Measure(verts);
    Check(Near(b.maxX - b.minX, 50) && Near(b.maxY - b.minY, 100), "Rotated sprite bounds are wrong");
}

void NineSlice()
{
    SpriteDraw draw;
    draw.dest = {0, 0, 100, 100};
    draw.slice = {5, 5, 5, 5};
    std::vector<SpriteVertex> verts;
    AppendSprite(verts, draw, 20, 20);
    Check(verts.size() == 54, "A nine-slice sprite must emit nine quads");
    // Corners keep their source pixel size; the centre stretches.
    Check(HasVertexNear(verts, 5, 0) && HasVertexNear(verts, 0, 5), "Nine-slice corner did not stay 5px");
    Check(HasVertexNear(verts, 95, 100) && HasVertexNear(verts, 100, 95), "Nine-slice far corner is wrong");
    const auto b = Measure(verts);
    Check(Near(b.minX, 0) && Near(b.maxX, 100) && Near(b.minY, 0) && Near(b.maxY, 100), "Nine-slice must still fill the dest rect");
    // The border UV split sits at 5/20 = 0.25 of the texture.
    bool splitUv = std::any_of(verts.begin(), verts.end(), [](const SpriteVertex& v) { return Near(v.uv.x, 0.25f); });
    Check(splitUv, "Nine-slice UV split should land at the border pixel ratio");
}

void OversizedSliceClamps()
{
    SpriteDraw draw;
    draw.dest = {0, 0, 10, 10};
    draw.slice = {40, 40, 40, 40};  // far larger than the dest / source
    std::vector<SpriteVertex> verts;
    AppendSprite(verts, draw, 8, 8);
    const auto b = Measure(verts);
    Check(Near(b.minX, 0) && Near(b.maxX, 10) && Near(b.minY, 0) && Near(b.maxY, 10), "Oversized nine-slice must clamp to the dest rect");
}

void ClipRect()
{
    // A plain quad clipped to a smaller window: geometry and UVs both trimmed.
    SpriteDraw draw;
    draw.dest = {0, 0, 100, 100};
    draw.clip = {25, 40, 50, 50}; // -> x 25..75, y 40..90
    std::vector<SpriteVertex> verts;
    AppendSprite(verts, draw, 1, 1);
    const auto b = Measure(verts);
    Check(Near(b.minX, 25) && Near(b.maxX, 75) && Near(b.minY, 40) && Near(b.maxY, 90), "Clip did not trim the quad");
    Check(Near(b.minU, 0.25f) && Near(b.maxU, 0.75f) && Near(b.minV, 0.40f) && Near(b.maxV, 0.90f), "Clip did not remap the UVs");

    // Fully outside the clip -> nothing emitted.
    verts.clear();
    SpriteDraw off = draw;
    off.clip = {500, 500, 10, 10};
    AppendSprite(verts, off, 1, 1);
    Check(verts.empty(), "A fully clipped sprite emits no geometry");

    // A rotated sprite ignores the clip (documented limitation).
    verts.clear();
    SpriteDraw rot;
    rot.dest = {0, 0, 100, 100};
    rot.clip = {0, 0, 10, 10};
    rot.rotationDegrees = 45;
    AppendSprite(verts, rot, 1, 1);
    Check(!verts.empty(), "Rotated sprite is not clipped");
}

void FontAtlasBasics()
{
    const FontAtlas atlas = FontAtlas::Build(24);
    Check(atlas.Valid() && atlas.Width() > 0 && atlas.Height() > 0, "Font atlas did not build");
    Check(atlas.PixelHeight() == 24 && atlas.LineHeight() > 0, "Font atlas metrics missing");
    Check(atlas.Pixels().size() == static_cast<std::size_t>(atlas.Width()) * atlas.Height() * 4, "Atlas pixel buffer size mismatch");
    Check(atlas.Has(U'M') && atlas.Has(U' ') && atlas.Has(U'?'), "Atlas is missing printable ASCII");
    Check(atlas.GlyphFor(U'M').advance > atlas.GlyphFor(U'i').advance, "Proportional font advances look wrong");

    bool anyCoverage = std::any_of(atlas.Pixels().begin(), atlas.Pixels().end(), [](std::uint8_t b) { return b != 0; });
    Check(anyCoverage, "Rendered atlas is entirely blank");

    const float wide = atlas.Measure("WWWW", 24);
    const float narrow = atlas.Measure("iiii", 24);
    Check(wide > narrow && narrow > 0, "Measure() is not proportional");
    Check(Near(atlas.Measure("Hi", 48), atlas.Measure("Hi", 24) * 2, 0.5f), "Measure() must scale linearly with pixel height");
}

void TextLayout()
{
    const FontAtlas atlas = FontAtlas::Build(20);
    const auto sprites = atlas.Layout("Hi", 5, 7, 20, Float4{1, 0, 0, 1}, nullptr);
    Check(sprites.size() == 2, "Layout should emit one sprite per visible glyph");
    Check(Near(sprites[0].dest.x, 5) && Near(sprites[0].dest.y, 7), "First glyph is not at the pen origin");
    Check(sprites[1].dest.x > sprites[0].dest.x, "Glyphs must advance along X");
    for (const auto& s : sprites)
        Check(Near(s.tint.x, 1) && Near(s.tint.y, 0), "Text colour must reach the glyph sprites");

    const auto spaced = atlas.Layout("A B", 0, 0, 20, Float4{1, 1, 1, 1}, nullptr);
    Check(spaced.size() == 2, "Spaces should not emit sprites");

    const auto wrapped = atlas.Layout("A\nB", 0, 0, 20, Float4{1, 1, 1, 1}, nullptr);
    Check(wrapped.size() == 2 && wrapped[1].dest.y > wrapped[0].dest.y && Near(wrapped[1].dest.x, 0),
          "A newline must return the pen and advance a line");
}

int main()
{
    try
    {
        PlainQuad();
        RegionCrop();
        RotationAboutPivot();
        NineSlice();
        OversizedSliceClamps();
        ClipRect();
        FontAtlasBasics();
        TextLayout();
        std::cout << "PASS: 2D sprite tessellation, regions, rotation, nine-slice, clip, font atlas, text layout\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
