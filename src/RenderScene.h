#pragma once
#include "core/GameObject.h"
#include "TransformGizmo.h"
#include <memory>
#include <optional>
#include <vector>

struct RenderMesh;
using MeshHandle = std::shared_ptr<const RenderMesh>;
struct MeshDraw
{
    MeshHandle mesh;
    zengine::Transform transform;
    std::optional<DirectX::XMFLOAT4X4> parentMatrix;
};
// Submission values only: no GameObject pointers or importer types cross this boundary.
struct ViewportFrame
{
    std::vector<MeshDraw> meshes;
    std::optional<zengine::Transform> selectionTransform;
    bool showEditorGuides = false;
    gizmo::Mode tool = gizmo::Mode::Move;
    int highlightedAxis = -1;
    std::optional<DirectX::XMFLOAT4X4> selectionParent;
};
