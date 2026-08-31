#pragma once
#include "Prefab.h"
#include <filesystem>
namespace zengine::prefabs
{
    bool IsPrefab(const std::filesystem::path&);
    std::filesystem::path Resolve(const std::filesystem::path& assets,const std::filesystem::path& file);
    scenes::Document Load(const std::filesystem::path& assets,const std::filesystem::path& file);
    void Save(const std::filesystem::path& assets,const std::filesystem::path& file,const scenes::Document&,const std::string* expected=nullptr);
    std::filesystem::path Create(const std::filesystem::path& assets,const scenes::Document&,const std::filesystem::path& folder={});
    Expansion ResolveScene(const std::filesystem::path& assets,const scenes::Document&);
}
