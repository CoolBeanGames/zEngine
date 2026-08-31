#pragma once
#include "InputMap.h"
#include <filesystem>
namespace zengine::input {
std::filesystem::path AssetPath(const std::filesystem::path& assets);
std::string Load(const std::filesystem::path& assets);
void Save(const std::filesystem::path& assets,const Map&,const std::string* expected=nullptr);
void Ensure(const std::filesystem::path& assets);
Hardware PollWindows(bool focused);
}
