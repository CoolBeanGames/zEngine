#pragma once

#include "ModelData.h"
#include <filesystem>

namespace FbxImporter
{
    // Project packages read only their copied albedo images, never original source paths.
    ModelData Load(const std::filesystem::path& file, bool projectPackage = false);
    std::filesystem::path Import(const std::filesystem::path& source,
                                 const std::filesystem::path& assetsDirectory,
                                 std::vector<std::string>& warnings);
}
