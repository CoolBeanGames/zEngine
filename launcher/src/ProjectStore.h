#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace zlauncher
{
// One registered project. `configFile` is the concrete <name>.zproject the
// engine editor opens; `valid` is false when that file is missing so the row can
// still be shown and removed from the list.
struct ProjectEntry
{
    std::filesystem::path folder;
    std::filesystem::path configFile;
    std::wstring name;
    bool valid = false;

    // Optional metadata for the project row. Any field that is empty is simply
    // not shown. `created` / `launched` come from the per-project `.zlaunch`
    // sidecar; `built` is derived from the project's `builds/current` folder.
    std::wstring version;   // e.g. "0.1.0"
    std::wstring created;   // "YYYY-MM-DD HH:MM:SS"
    std::wstring launched;  // "YYYY-MM-DD HH:MM:SS"
    std::wstring built;     // "YYYY-MM-DD HH:MM:SS"
};

// A project sitting in the deployed build's `templates/` folder. Creating a new
// project copies one of these verbatim.
struct ProjectTemplate
{
    std::wstring name;
    std::filesystem::path folder;
};

// One zEngine editor release on GitHub, as shown on the launcher's "Engine" tab.
struct EngineReleaseInfo
{
    std::wstring tag;              // e.g. "eng_4"
    std::wstring title;            // release name, falls back to the tag
    int number = -1;              // build number from the "engine_<n>.zip" asset
    std::string url;              // that asset's download URL
    bool downloaded = false;
    std::filesystem::path localDir; // where it is on disk when downloaded
};

class ProjectStore
{
public:
    void Load();
    const std::vector<ProjectEntry>& Projects() const { return projects_; }

    // Register an already-existing project folder (no files created).
    void AddExisting(const std::filesystem::path& folder);
    // Drop a project from the launcher list. Never touches project files.
    void Remove(std::size_t index);

    // Stamp the project's `.zlaunch` sidecar with "just launched now" (creating
    // the sidecar if needed) and refresh the in-memory row.
    void MarkLaunched(std::size_t index);

    // Create <parent>/<name>: copy `tmpl` if given, otherwise write a minimal
    // ZENGINE_PROJECT config plus an empty Assets folder. Registers and returns
    // the new entry. Throws std::runtime_error on any problem.
    ProjectEntry Create(const std::filesystem::path& parent, const std::wstring& name,
                        const ProjectTemplate* tmpl);

    std::vector<ProjectTemplate> Templates() const;

    // Copy an entire project into the editor's "templates/<name>" folder so it
    // can be picked as a template for a new project. Build outputs and VCS
    // metadata are skipped. Returns false + fills `message` on any problem
    // (no editor, name already taken, copy failure); `message` also carries the
    // success text.
    bool CopyToTemplates(const ProjectEntry& entry, std::wstring& message) const;

    // This launcher's baked-in release number and build date.
    static int LauncherBuildNumber();
    static std::wstring LauncherBuildDate();

    // Check GitHub for a launcher release ("launcher_<n>.zip") newer than this
    // build. When one is found, spawn the updater helper and set `started` so the
    // caller can quit. Returns false only on an actual error (message explains).
    bool LauncherUpdate(std::wstring& message, bool& started) const;

    // One-time setup: back-fill a `.zlaunch` sidecar into every registered
    // project that lacks one, and register the `.zlaunch` file association so
    // double-clicking one opens its project. Guarded by a marker file, so it
    // only does real work on the first run.
    void RunFirstTimeSetup();

    // --- the launcher's "own end" of the integration -----------------------
    // Point the editor at `entry` (via its last-project state file) and start it.
    // Returns false and fills `error` when the editor exe cannot be found.
    bool LaunchEditor(const ProjectEntry& entry, std::wstring& error) const;
    // Resolve a project folder and launch it in the editor. Used for the
    // `.zlaunch` double-click entry point.
    static bool LaunchProjectAt(const std::filesystem::path& folder, std::wstring& error);
    // The playable build of a project: "<folder>/builds/current/<exe>" when that
    // folder exists and contains an executable, otherwise empty.
    static std::optional<std::filesystem::path> PlayableExe(const ProjectEntry& entry);
    // Run the project's current build (see PlayableExe). Returns false + error
    // when there is nothing to run or it could not start.
    static bool PlayProject(const ProjectEntry& entry, std::wstring& error);

    // Make sure an editor is installed under "C:\Program Files\z engine\versions".
    // When that location is empty this downloads the latest editor release and
    // unzips a new "<n>" version folder there; otherwise it checks GitHub for a
    // higher-numbered build and installs it alongside. Either way it (re)creates
    // the Start-menu and Desktop shortcuts that point at the newest installed
    // editor. `allowDownload` gates the (possibly large) network download;
    // `changed` is set when files were installed. `message` always carries a
    // human-readable result.
    bool EnsureEditorInstalled(std::wstring& message, bool& changed, bool allowDownload) const;

    // The editor-versions root: "<Program Files>\z engine\versions".
    static std::filesystem::path EngineVersionsRoot();

    // --- the "Engine" tab: every editor release on GitHub -------------------
    // Fetch the release list (newest build first). Returns false + `error` when
    // GitHub cannot be reached.
    bool ListEngineReleases(std::vector<EngineReleaseInfo>& out, std::wstring& error) const;
    // Download + install one release (same placement rules as EnsureEditorInstalled).
    bool DownloadEngineRelease(const EngineReleaseInfo& release, std::wstring& message) const;
    // Delete a downloaded build from the per-user downloads folder.
    static bool DeleteEngineVersion(int number, std::wstring& message);
    // Launch a downloaded build's zEngine.exe.
    static bool LaunchEngineVersion(const std::filesystem::path& versionDir, std::wstring& error);
    // Local folder for a downloaded editor build, if present.
    static std::optional<std::filesystem::path> EngineVersionDir(int number);

    // Reserved: headless standalone build. Not wired up yet.
    bool BuildProject(const ProjectEntry& entry, std::wstring& error) const;
    // Download the most recent GitHub release of the editor, unzip it and replace
    // the installed editor files. `message` is filled with a human-readable
    // result in both the success and failure case. Blocks while it runs.
    bool UpdateEditor(std::wstring& message) const;

    static std::filesystem::path RegistryFile();
    static std::optional<std::filesystem::path> LocateEditor();
    // The folder of the editor the launcher manages: the flat
    // "<Program Files>\zEngine" when present, else the newest per-user
    // "<LocalAppData>\zEngine\Engine\Downloads\<n>". `outNumber` gets its build
    // number (-1 if unknown / nothing installed).
    static std::optional<std::filesystem::path> InstalledEditorDir(int* outNumber);
    // Best-effort editor version string ("" when it cannot be determined): a
    // `version.txt` next to the editor, else the exe's version resource.
    static std::wstring EditorVersion();

private:
    bool InstallEditorRelease(int number, const std::wstring& url, std::wstring& message,
                              bool& changed) const;
    void Save() const;
    static ProjectEntry Resolve(const std::filesystem::path& folder);
    static bool LaunchEntry(const ProjectEntry& entry, std::wstring& error);

    std::vector<ProjectEntry> projects_;
};
}
