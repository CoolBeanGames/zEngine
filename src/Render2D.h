#pragma once

#include "ModelData.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 2D / UI submission layer.
//
// This is the flexible front door the task (ZE-60) asks for: everything is
// expressed in screen pixels (origin top-left, +Y down), the renderer draws it
// after the 3D pass with depth testing off and alpha blending on, and nothing
// here is affected by any lighting system.
//
// Textures are uploaded through Renderer::UploadTexture - a deliberately small,
// self-contained path. The renderer is free to back sprites with the material /
// material-instance systems internally later; callers never have to build a
// material just to show a UI image.
// ---------------------------------------------------------------------------

struct RenderTexture;
using TextureHandle = std::shared_ptr<const RenderTexture>;

// Destination rectangle on screen, in pixels.
struct SpriteRect { float x = 0, y = 0, width = 0, height = 0; };

// Source sub-rectangle of the texture, normalised 0..1. The default is the whole
// texture; a smaller region crops / picks an atlas cell.
struct SpriteRegion { float u0 = 0, v0 = 0, u1 = 1, v1 = 1; };

// Nine-slice border sizes, in source-texture pixels measured inside the region.
// All zero (the default) means a plain stretched quad.
struct NineSlice { float left = 0, top = 0, right = 0, bottom = 0; };

struct SpriteDraw
{
    TextureHandle texture;           // null -> renderer substitutes a 1x1 white texture
    SpriteRect dest;
    SpriteRegion region;
    NineSlice slice;
    Float4 tint{1, 1, 1, 1};         // straight-alpha multiply
    float rotationDegrees = 0;       // clockwise, about the pivot
    Float2 pivot{0.5f, 0.5f};        // 0..1 within dest; the rotation centre
    // Optional screen-space clip rectangle (pixels). A zero / negative width or
    // height means "no clip". Only honoured for unrotated sprites; the UI layer
    // uses it for the contents of a scrolling container.
    SpriteRect clip{0, 0, 0, 0};
};

// A run of text. The renderer lays this out against its font atlas (built lazily
// from the system UI font) and expands it into sprites.
struct TextDraw
{
    std::string text;                // UTF-8
    float x = 0, y = 0;              // top-left of the first glyph cell, pixels
    float pixelHeight = 16;          // requested cap-to-baseline-ish cell height
    Float4 color{1, 1, 1, 1};
    SpriteRect clip{0, 0, 0, 0};     // optional screen-space clip (see SpriteDraw::clip)
};

struct SpriteVertex
{
    Float2 position;                 // screen pixels
    Float2 uv;
    Float4 color;
};

// Appends two triangles per cell (6 verts for a plain quad, 54 for a nine-slice)
// in screen-pixel space, with the region mapped through and rotation applied
// about the pivot. `textureWidth` / `textureHeight` are the full texture size in
// pixels and are only needed to resolve nine-slice border pixels into UVs.
void AppendSprite(std::vector<SpriteVertex>& out, const SpriteDraw& draw,
                  float textureWidth, float textureHeight);
