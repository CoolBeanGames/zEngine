#pragma once
#include "core/GameObject.h"
#include <memory>
#include <optional>
#include <vector>

struct RenderMesh;
using MeshHandle = std::shared_ptr<const RenderMesh>;
struct MeshDraw
{
    MeshHandle mesh;
    zengine::Transform transform;
};
// Submission values only: no GameObject pointers or importer types cross this boundary.
struct ViewportFrame
{
    std::vector<MeshDraw> meshes;
    std::optional<zengine::Transform> selectionTransform;
    bool showEditorGuides = false;
};
