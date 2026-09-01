#pragma once
#include "ScriptHost.h"
#include "physics/PhysicsBehavior.h"
#include <string_view>

namespace zengine::scenes
{
    constexpr std::size_t MaxSceneBytes=8*1024*1024;
    struct BehaviorData
    {
        enum class Kind { Mesh, Script, Collider, RigidBody, KinematicBody, StaticBody, Area };
        Kind kind=Kind::Mesh;
        bool enabled=true;
        float priority=0;
        std::string asset;
        std::map<std::string,script::Value> variables;
        physics::ColliderShape shape=physics::ColliderShape::Box;
        Vec3 colliderOffset{},colliderSize{1,1,1};
        std::uint32_t layer=1,mask=0xffffffffu;
        float friction=.5f,bounciness=0,mass=1,gravityScale=1;
        Vec3 velocity{},angularVelocity{},constantForce{},constantTorque{};
        bool operator==(const BehaviorData&) const = default;
    };
    struct ObjectData
    {
        GameObjectId id=0;
        std::string name;
        std::vector<std::string> tags;
        Transform transform;
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
}
