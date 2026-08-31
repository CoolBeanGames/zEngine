#pragma once
#include "ScriptHost.h"
#include <string_view>

namespace zengine::scenes
{
    constexpr std::size_t MaxSceneBytes=8*1024*1024;
    struct BehaviorData
    {
        enum class Kind { Mesh, Script };
        Kind kind=Kind::Mesh;
        bool enabled=true;
        float priority=0;
        std::string asset;
        std::map<std::string,script::Value> variables;
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
    };
    struct Document { std::vector<ObjectData> objects; };
    // Scene data/codec have no filesystem, renderer, or window dependencies.
    Document Capture(const ObjectStore&, const ScriptHost&);
    std::string Encode(const Document&);
    Document Decode(std::string_view text);
    struct Instance { ObjectStore objects; ScriptHost scripts; };
    Instance Instantiate(const Document&);
}
