#pragma once
#include <windows.h>
#include <filesystem>
#include <optional>
#include <string>

class ProjectDialog final
{
public:
    static constexpr int NameField=4100, LocationField=4101, BrowseButton=4102, ErrorLabel=4103;
    struct Request { std::wstring name; std::filesystem::path location; };
    static std::optional<Request> Show(HWND owner,const std::filesystem::path& initialLocation={});
};
