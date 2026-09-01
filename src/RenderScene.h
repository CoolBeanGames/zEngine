#pragma once
#include "core/GameObject.h"
#include "TransformGizmo.h"
#include "physics/PhysicsBehavior.h"
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
struct ColliderDraw { zengine::physics::ColliderShape shape; zengine::Transform transform; zengine::Vec3 offset{},size{1,1,1}; std::optional<DirectX::XMFLOAT4X4> parentMatrix; bool selected=false; };
// Submission values only: no GameObject pointers or importer types cross this boundary.
struct ViewportFrame
{
    std::vector<MeshDraw> meshes;
    std::vector<ColliderDraw> colliders;
    std::optional<zengine::Transform> selectionTransform;
    bool showEditorGuides = false;
    gizmo::Mode tool = gizmo::Mode::Move;
    int highlightedAxis = -1;
    std::optional<DirectX::XMFLOAT4X4> selectionParent;
    SceneCamera camera;
    std::optional<unsigned> fps;
};
