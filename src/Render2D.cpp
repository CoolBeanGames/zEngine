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

    // Axis-aligned clip: only applied when the sprite is not rotated.
    const bool doClip = draw.clip.width > 0 && draw.clip.height > 0 &&
                        std::abs(sinA) < 1e-4f && cosA > 0;
    const float clipX0 = draw.clip.x, clipY0 = draw.clip.y;
    const float clipX1 = draw.clip.x + draw.clip.width, clipY1 = draw.clip.y + draw.clip.height;

    for (int r = 0; r < rows; ++r)
    {
        const int ri = sliced ? rowIndex[r] : 0;
        const int riNext = sliced ? ri + 1 : 3;
        for (int cc = 0; cc < columns; ++cc)
        {
            const int ci = sliced ? colIndex[cc] : 0;
            const int ciNext = sliced ? ci + 1 : 3;

            float cx0 = xs[ci], cx1 = xs[ciNext], cy0 = ys[ri], cy1 = ys[riNext];
            float cu0 = us[ci], cu1 = us[ciNext], cv0 = vs[ri], cv1 = vs[riNext];
            if (doClip)
            {
                const float nx0 = std::max(cx0, clipX0), nx1 = std::min(cx1, clipX1);
                const float ny0 = std::max(cy0, clipY0), ny1 = std::min(cy1, clipY1);
                if (nx1 <= nx0 || ny1 <= ny0) continue; // fully clipped
                const float sx = (cx1 - cx0) != 0 ? 1.0f / (cx1 - cx0) : 0.0f;
                const float sy = (cy1 - cy0) != 0 ? 1.0f / (cy1 - cy0) : 0.0f;
                const float u0 = cu0 + (cu1 - cu0) * (nx0 - cx0) * sx;
                const float u1 = cu0 + (cu1 - cu0) * (nx1 - cx0) * sx;
                const float v0 = cv0 + (cv1 - cv0) * (ny0 - cy0) * sy;
                const float v1 = cv0 + (cv1 - cv0) * (ny1 - cy0) * sy;
                cx0 = nx0; cx1 = nx1; cy0 = ny0; cy1 = ny1;
                cu0 = u0; cu1 = u1; cv0 = v0; cv1 = v1;
            }
            const Float2 tl = place(cx0, cy0);
            const Float2 tr = place(cx1, cy0);
            const Float2 br = place(cx1, cy1);
            const Float2 bl = place(cx0, cy1);
            const std::array<Corner, 4> corners{
                Corner{tl.x, tl.y, cu0, cv0},
                Corner{tr.x, tr.y, cu1, cv0},
                Corner{br.x, br.y, cu1, cv1},
                Corner{bl.x, bl.y, cu0, cv1}};
            EmitCell(out, corners, draw.tint);
        }
    }
}
