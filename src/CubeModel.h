#pragma once
// ZE-113: a canonical unit-cube ModelData that matches Renderer::CreateCube()'s
// geometry (positions, normals, winding). Used by the lightmap baker and by the
// static-mesh clone path so a "builtin:cube" can be baked and rendered from a
// lightmap. Vertex colours are white so a baked term multiplies in directly.

#include "ModelData.h"

inline ModelData UnitCubeModel()
{
    ModelData model;
    model.vertices = {
        {{-1, -1, -1}, {-0.577f, -0.577f, -0.577f}, {1, 1, 1}, {}},
        {{-1, +1, -1}, {-0.577f, +0.577f, -0.577f}, {1, 1, 1}, {}},
        {{+1, +1, -1}, {+0.577f, +0.577f, -0.577f}, {1, 1, 1}, {}},
        {{+1, -1, -1}, {+0.577f, -0.577f, -0.577f}, {1, 1, 1}, {}},
        {{-1, -1, +1}, {-0.577f, -0.577f, +0.577f}, {1, 1, 1}, {}},
        {{-1, +1, +1}, {-0.577f, +0.577f, +0.577f}, {1, 1, 1}, {}},
        {{+1, +1, +1}, {+0.577f, +0.577f, +0.577f}, {1, 1, 1}, {}},
        {{+1, -1, +1}, {+0.577f, -0.577f, +0.577f}, {1, 1, 1}, {}},
    };
    model.indices = {
        0, 1, 2, 0, 2, 3,
        7, 6, 5, 7, 5, 4,
        4, 5, 1, 4, 1, 0,
        3, 2, 6, 3, 6, 7,
        1, 5, 6, 1, 6, 2,
        4, 0, 3, 4, 3, 7,
    };
    model.parts = {{0, 36, 0}};
    model.materials = {AlbedoMaterial{{1, 1, 1}, {}}}; // part 0 references material 0
    return model;
}
