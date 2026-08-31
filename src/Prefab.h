#pragma once
#include "Scene.h"
#include <functional>
#include <set>

namespace zengine::prefabs
{
    using Loader=std::function<scenes::Document(const std::string&)>;
    struct Expansion
    {
        scenes::Document scene;
        std::set<GameObjectId> generated;
        std::map<GameObjectId,std::string> sources;
    };
    // One root, optional ordinary children, and references to other prefab assets.
    void Validate(const scenes::Document&);
    std::string Encode(const scenes::Document&);
    scenes::Document Decode(std::string_view);
    Expansion Expand(const scenes::Document&,const Loader&);
}
