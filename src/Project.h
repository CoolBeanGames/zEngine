#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace zengine::projects
{
    struct Config
    {
        std::string name;
        std::vector<std::string> scenes; // Paths relative to the project root, always inside Assets.
        std::string lastScene;
    };
    struct Project
    {
        std::filesystem::path file;
        Config config;
        std::string source; // Last read/written bytes for external-edit detection.
    };
    void ValidateName(const std::wstring& name);
    std::string Encode(const Config&);
    Config Decode(const std::string&);
    Project Create(const std::filesystem::path& location,const std::wstring& name);
    Project Open(const std::filesystem::path& file);
    // Explicit embedding/legacy-folder entry point; never overwrites an existing config.
    Project InitializeDirectory(const std::filesystem::path& directory);
    std::filesystem::path Assets(const Project&);
    std::filesystem::path ScenePath(const Project&,const std::string& reference);
    void TrackScene(Project&,const std::filesystem::path& scene);
    void Save(Project&);
    std::filesystem::path DefaultSessionFile();
    std::optional<std::filesystem::path> ReadRecent(const std::filesystem::path& sessionFile);
    void WriteRecent(const std::filesystem::path& sessionFile,const std::filesystem::path& projectFile);
}
