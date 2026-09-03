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
    bool lit = true; // ZE-74: false => draw at full brightness even when the scene has lights
};
// ZE-74: one scene light, already resolved to world space. `type` 0=directional,
// 1=point, 2=spot. `direction` is the beam direction (unit); `range`/`falloff` /
// the spot cone cosines only matter for point/spot.
struct LightData
{
    int type = 0;
    zengine::Vec3 position{};
    zengine::Vec3 direction{0, -1, 0};
    Float3 color{1, 1, 1};
    float intensity = 1;
    float range = 10;
    float falloff = 2;
    float spotCosInner = 0.94f;
    float spotCosOuter = 0.82f;
    float fogScatter = 0; // ZE-75
};
// ZE-75: scene atmosphere. fogMode 0 off / 1 linear / 2 exp2.
struct EnvironmentData
{
    int fogMode = 0;
    Float3 fogColor{0.55f, 0.60f, 0.68f};
    float fogNear = 8, fogFar = 60, fogDensity = 0.03f;
    float heightBase = 0, heightFalloff = 6, heightStrength = 0;
    int volumetric = 0, volumetricSteps = 6;
};
struct ColliderDraw { zengine::physics::ColliderShape shape; zengine::Transform transform; zengine::Vec3 offset{},size{1,1,1}; std::optional<DirectX::XMFLOAT4X4> parentMatrix; bool selected=false; bool audioZone=false; };
// ZE-111: audible range of a 3D AudioSource - inner (full volume) + outer (silent)
// wireframe spheres centred on the object. 2D / global sources emit no AudioRange.
struct AudioRange { zengine::Transform transform; std::optional<DirectX::XMFLOAT4X4> parentMatrix; float minDistance=1, maxDistance=25; bool selected=false; };
// ZE-74: editor guide for a Light - a marker at its position plus a beam line
// (directional / spot) or a range sphere (point / spot), tinted by the colour.
struct LightGizmo { int type=0; zengine::Vec3 position{}; zengine::Vec3 direction{0,0,1}; Float3 color{1,1,1}; float range=10; float spotOuterDeg=35; bool selected=false; };
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
    std::vector<LightData> lights; // ZE-74: empty => the scene renders unlit
    std::optional<EnvironmentData> environment; // ZE-75: fog / atmosphere
    std::vector<ColliderDraw> colliders;
    std::vector<AudioRange> audioRanges;
    std::vector<LightGizmo> lightGizmos;
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
