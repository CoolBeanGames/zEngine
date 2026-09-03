#pragma once
#include "ScriptHost.h"
#include "physics/PhysicsBehavior.h"
#include "ui/UiSerialize.h"
#include <string_view>
#include <utility>

namespace zengine::scenes
{
    constexpr std::size_t MaxSceneBytes=8*1024*1024;
    struct BehaviorData
    {
        enum class Kind { Mesh, Script, Collider, RigidBody, KinematicBody, StaticBody, Area, Camera, Ui, Audio };
        Kind kind=Kind::Mesh;
        std::string uiType; // when kind==Ui: ui::UiControl::TypeName()
        std::vector<std::pair<std::string,std::string>> uiProps; // ordered key/value
        std::string meshMaterial; // when kind==Mesh: project-relative ".material" (ZE-65), empty = default
        // ZE-67: when kind==Audio
        std::string audioClip; // project-relative audio file
        bool audioSpatial=true, audioAutoplay=true, audioLoop=false;
        float audioVolume=1, audioPitch=1;
        int audioAttenuation=1; // audio::Attenuation
        float audioMinDistance=1, audioMaxDistance=25;
        bool enabled=true;
        float priority=0;
        std::string asset;
        std::map<std::string,script::Value> variables;
        std::map<std::string,GameObjectId> objectReferences;
        std::map<std::string,std::vector<ScriptArrayElement>> arrays;
        physics::ColliderShape shape=physics::ColliderShape::Box;
        Vec3 colliderOffset{},colliderSize{1,1,1};
        std::uint32_t layer=1,mask=0xffffffffu;
        float friction=.5f,bounciness=0,mass=1,gravityScale=1;
        Vec3 velocity{},angularVelocity{},constantForce{},constantTorque{};
        float cameraFov=60,cameraNear=0.1f,cameraFar=1000;
        bool operator==(const BehaviorData&) const = default;
    };
    struct ObjectData
    {
        GameObjectId id=0;
        std::string name;
        std::vector<std::string> tags;
        Transform transform;
        bool is2D=false;
        Transform2D transform2d; // used when is2D
        std::vector<BehaviorData> behaviors;
        GameObjectId parent=0;
        std::string prefab;
        bool transformOverride=false;
        unsigned transformMask=0; // XYZ position, XYZ rotation, XYZ scale. Legacy bool-only references override all.
        unsigned prefabDataMask=0; // Name, tags, and behavior-list overrides while remaining linked.
    };
    struct Document { std::vector<ObjectData> objects; };
    // Scene data/codec have no filesystem, renderer, or window dependencies.
    Document Capture(const ObjectStore&, const ScriptHost&);
    std::string Encode(const Document&);
    Document Decode(std::string_view text);
    struct Instance { ObjectStore objects; ScriptHost scripts; };
    Instance Instantiate(const Document&);
    // Adds a resolved scene/prefab document to an existing live world and returns its first root.
    GameObjectId Append(const Document&,ObjectStore&,ScriptHost&,GameObjectId parent=0);
}
