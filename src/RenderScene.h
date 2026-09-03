#pragma once
#include "core/GameObject.h"
#include "TransformGizmo.h"
#include "ViewportCamera.h"
#include "Render2D.h"
#include "physics/PhysicsBehavior.h"
#include <memory>
#include <optional>
#include <vector>

struct RenderMesh;
using MeshHandle = std::shared_ptr<const RenderMesh>;
// ZE-65: a resolved Material Instance. `albedo` overrides the model's imported
// texture; `tint` multiplies the albedo. Null handle = the model's own material.
struct RenderMaterial;
using MaterialHandle = std::shared_ptr<const RenderMaterial>;
struct MeshDraw
{
    MeshHandle mesh;
    zengine::Transform transform;
    std::optional<DirectX::XMFLOAT4X4> parentMatrix;
    MaterialHandle material;
};
struct ColliderDraw { zengine::physics::ColliderShape shape; zengine::Transform transform; zengine::Vec3 offset{},size{1,1,1}; std::optional<DirectX::XMFLOAT4X4> parentMatrix; bool selected=false; };
// ZE-111: audible range of a 3D AudioSource - inner (full volume) + outer (silent)
// wireframe spheres centred on the object. 2D / global sources emit no AudioRange.
struct AudioRange { zengine::Transform transform; std::optional<DirectX::XMFLOAT4X4> parentMatrix; float minDistance=1, maxDistance=25; bool selected=false; };
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
    std::vector<AudioRange> audioRanges;
    std::vector<CameraView> cameraGizmos;
    std::optional<CameraView> gameView; // when set, the renderer views the scene through this camera
    std::optional<zengine::Transform> selectionTransform;
    bool showEditorGuides = false;
    gizmo::Mode tool = gizmo::Mode::Move;
    int highlightedAxis = -1;
    std::optional<DirectX::XMFLOAT4X4> selectionParent;
    SceneCamera camera;
    std::optional<unsigned> fps;
    // 2D / UI overlay, drawn last in screen pixels, unlit and depth-independent.
    std::vector<SpriteDraw> sprites;
    std::vector<TextDraw> texts;
};
