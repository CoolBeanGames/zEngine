#pragma once
#include "Project.h"
#include "ViewportCamera.h"
#include <filesystem>
#include <functional>

namespace zengine::game {
struct Settings {SceneCamera camera;};
std::string EncodeSettings(const Settings&);
Settings DecodeSettings(const std::string&);
// Produces a new, uniquely named output directory. Never overwrites an earlier build.
std::filesystem::path Export(const projects::Project&,const std::filesystem::path& startupScene,
    const Settings&,const std::filesystem::path& outputParent,const std::filesystem::path& playerDirectory,
    const std::function<void(unsigned,const std::string&)>& progress={});
std::filesystem::path ExecutableDirectory();
}
