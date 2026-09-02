#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace zlauncher
{
struct NewProjectRequest
{
    std::wstring name;
    std::filesystem::path location;   // parent folder
    int templateIndex = -1;           // -1 == blank project, else index into the list passed in
};

// Modal "New Project" dialog. Returns nullopt when the user cancels.
std::optional<NewProjectRequest> ShowNewProjectDialog(HWND owner,
                                                      const std::vector<std::wstring>& templateNames);
}
