#include "Render2D.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    struct Corner { float x, y, u, v; };

    void EmitCell(std::vector<SpriteVertex>& out, const std::array<Corner, 4>& c, const Float4& tint)
    {
        // c: 0 = top-left, 1 = top-right, 2 = bottom-right, 3 = bottom-left.
        const auto push = [&](const Corner& corner) {
            out.push_back(SpriteVertex{{corner.x, corner.y}, {corner.u, corner.v}, tint});
        };
        push(c[0]); push(c[1]); push(c[2]);
        push(c[0]); push(c[2]); push(c[3]);
    }
}

void AppendSprite(std::vector<SpriteVertex>& out, const SpriteDraw& draw,
                  float textureWidth, float textureHeight)
{
    const SpriteRect& d = draw.dest;
    if (d.width == 0 || d.height == 0) return;

    // Region in normalised UV space, and its span in source pixels.
    const float ru0 = draw.region.u0, rv0 = draw.region.v0;
    const float ru1 = draw.region.u1, rv1 = draw.region.v1;
    const float regionPixelsX = std::max(1.0f, std::abs(ru1 - ru0) * std::max(1.0f, textureWidth));
    const float regionPixelsY = std::max(1.0f, std::abs(rv1 - rv0) * std::max(1.0f, textureHeight));

    // Clamp the nine-slice borders so opposing borders never overlap in either
    // the source or the destination.
    NineSlice s = draw.slice;
    s.left = std::max(0.0f, s.left); s.right = std::max(0.0f, s.right);
    s.top = std::max(0.0f, s.top);  s.bottom = std::max(0.0f, s.bottom);
    const float sourceScale = std::min(
        s.left + s.right > regionPixelsX ? regionPixelsX / (s.left + s.right) : 1.0f,
        s.top + s.bottom > regionPixelsY ? regionPixelsY / (s.top + s.bottom) : 1.0f);
    const float destScale = std::min(
        s.left + s.right > d.width ? d.width / (s.left + s.right) : 1.0f,
        s.top + s.bottom > d.height ? d.height / (s.top + s.bottom) : 1.0f);
    const float borderScale = std::min(sourceScale, destScale);
    s.left *= borderScale; s.right *= borderScale; s.top *= borderScale; s.bottom *= borderScale;

    // Column / row split points: destination X, then matching U.
    const float xs[4] = {d.x, d.x + s.left, d.x + d.width - s.right, d.x + d.width};
    const float ys[4] = {d.y, d.y + s.top, d.y + d.height - s.bottom, d.y + d.height};
    const float uSpan = ru1 - ru0, vSpan = rv1 - rv0;
    const float us[4] = {
        ru0,
        ru0 + uSpan * (s.left / regionPixelsX),
        ru1 - uSpan * (s.right / regionPixelsX),
        ru1};
    const float vs[4] = {
        rv0,
        rv0 + vSpan * (s.top / regionPixelsY),
        rv1 - vSpan * (s.bottom / regionPixelsY),
        rv1};

    const bool sliced = s.left > 0 || s.right > 0 || s.top > 0 || s.bottom > 0;
    const int columns = sliced ? 3 : 1;
    const int rows = sliced ? 3 : 1;
    const int colIndex[3] = {0, 1, 2};
    const int rowIndex[3] = {0, 1, 2};

    // Rotation about the pivot, applied to every emitted position.
    const float radians = draw.rotationDegrees * 3.14159265358979323846f / 180.0f;
    const float sinA = std::sin(radians), cosA = std::cos(radians);
    const float px = d.x + draw.pivot.x * d.width;
    const float py = d.y + draw.pivot.y * d.height;
    const auto place = [&](float x, float y) -> Float2 {
        const float dx = x - px, dy = y - py;
        return Float2{px + dx * cosA - dy * sinA, py + dx * sinA + dy * cosA};
    };

    for (int r = 0; r < rows; ++r)
    {
        const int ri = sliced ? rowIndex[r] : 0;
        const int riNext = sliced ? ri + 1 : 3;
        for (int cc = 0; cc < columns; ++cc)
        {
            const int ci = sliced ? colIndex[cc] : 0;
            const int ciNext = sliced ? ci + 1 : 3;
            const Float2 tl = place(xs[ci], ys[ri]);
            const Float2 tr = place(xs[ciNext], ys[ri]);
            const Float2 br = place(xs[ciNext], ys[riNext]);
            const Float2 bl = place(xs[ci], ys[riNext]);
            const std::array<Corner, 4> corners{
                Corner{tl.x, tl.y, us[ci], vs[ri]},
                Corner{tr.x, tr.y, us[ciNext], vs[ri]},
                Corner{br.x, br.y, us[ciNext], vs[riNext]},
                Corner{bl.x, bl.y, us[ci], vs[riNext]}};
            EmitCell(out, corners, draw.tint);
        }
    }
}
