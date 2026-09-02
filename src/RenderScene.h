#pragma once
#include "core/GameObject.h"
#include "TransformGizmo.h"
#include "ViewportCamera.h"
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
// A Camera GameObject: its frustum is drawn as an editor guide, and the main one
// replaces the orbit camera as the view when set as gameView.
struct CameraView
{
    zengine::Transform transform;
    std::optional<DirectX::XMFLOAT4X4> parentMatrix;
    float fovY = 60, nearZ = 0.1f, farZ = 1000;
    bool selected = false, main = false;
};
// Submission values only: no GameObject pointers or importer types cross this boundary.
struct ViewportFrame
{
    std::vector<MeshDraw> meshes;
    std::vector<ColliderDraw> colliders;
    std::vector<CameraView> cameraGizmos;
    std::optional<CameraView> gameView; // when set, the renderer views the scene through this camera
    std::optional<zengine::Transform> selectionTransform;
    bool showEditorGuides = false;
    gizmo::Mode tool = gizmo::Mode::Move;
    int highlightedAxis = -1;
    std::optional<DirectX::XMFLOAT4X4> selectionParent;
    SceneCamera camera;
    std::optional<unsigned> fps;
};
