#pragma once
#include "Scene.h"
#include <filesystem>
namespace zengine::scenes
{
    bool IsScene(const std::filesystem::path&);
    std::filesystem::path Resolve(const std::filesystem::path& assets,const std::filesystem::path& path);
    std::string Load(const std::filesystem::path&);
    // Atomic replacement with optimistic external-edit protection. Null expected means create only.
    void Save(const std::filesystem::path& assets,const std::filesystem::path&,std::string_view text,const std::string* expected=nullptr);
    std::filesystem::path Create(const std::filesystem::path& assets);
}
