#include "EditorShell.h"
#include "ProjectDialog.h"
#include "InspectorPanel.h"
#include "ScriptEditor.h"
#include "SceneAssets.h"
#include "input/InputMapEditor.h"
#include <commdlg.h>
#include <array>
#include <stdexcept>

namespace
{
    std::wstring Wide(const std::string& value)
    {
        const int size=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0);
        std::wstring result(size,L' ');
        MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),size);
        return result;
    }
}

void EditorShell::RequireProject() const
{
    if (!project_) throw std::runtime_error("Create or open a project first (File menu).");
}
void EditorShell::RequireScene() const
{
    RequireProject();
    if (!sceneOpen_) throw std::runtime_error("Create or open a scene first (File menu).");
}
bool EditorShell::ConfirmProjectClose()
{
    if(Building())throw std::runtime_error("Wait for the game build before switching projects.");
    if (!editingPrefab_.empty() && !ClosePrefab()) return false;
    if (Playing()) throw std::runtime_error("Stop Play before switching projects.");
    // Imports write into their captured Assets folder. Finish them before releasing that project.
    if (assetWork_.valid() || !assetJobs_.empty())
        throw std::runtime_error("Wait for asset imports and model loading to finish before switching projects.");
    return ConfirmScriptClose() && ConfirmSceneClose();
}
void EditorShell::RememberProjectScene()
{
    if (!project_ || !editingPrefab_.empty()) return;
    try
    {
        auto next=*project_;
        if (sceneOpen_ && !scenePath_.empty()) zengine::projects::TrackScene(next,scenePath_);
        zengine::projects::Save(next);
        project_=std::move(next);
        if (!recentSessionFile_.empty()) zengine::projects::WriteRecent(recentSessionFile_,project_->file);
    }
    catch (const std::exception& error)
    {
        // Scene saves remain valid even if a read-only/conflicting config cannot remember them.
        status_+=L" | Could not remember project/scene: "+Wide(error.what());
    }
}
void EditorShell::ActivateProject(zengine::projects::Project project)
{
    EndGizmoDrag(true);
    const auto assets=zengine::projects::Assets(project);
    zengine::input::Ensure(assets); // Validate/ensure the one project-owned root asset before switching state.
    inspectorPanel_->Bind(nullptr);
    scriptEditors_.clear();
    inputEditor_.reset(); inputSystem_.Configure({});
    ++sceneGeneration_;
    assetJobs_.clear(); activeAssetJob_.reset();
    meshBindings_.clear(); meshCache_.clear(); meshRevisions_.clear();
    prefabLinks_.clear(); prefabGenerated_.clear(); prefabSources_.clear(); editingPrefab_.clear(); prefabReturn_.reset();objectClipboard_.reset();
    scriptHost_=zengine::ScriptHost{}; objects_=zengine::ObjectStore{};
    project_=std::move(project); assetsDirectory_=assets;assetFolder_.clear();
    sceneOpen_=false; sceneDirty_=false; scenePath_.clear(); sceneSource_.clear();
    sceneBaseline_.clear(); playScene_.reset(); selectedObject_=0; firstObject_=0; firstAsset_=0; collapsedObjects_.clear();
    SetWindowTextW(viewportWindow_,L"Scene Viewport");
    status_=L"Opened project: "+Wide(project_->config.name);
    RefreshAssets(); RememberProjectScene(); UpdateSceneTitle(); InvalidateRect(window_,nullptr,FALSE);
}
bool EditorShell::CreateProject(const std::wstring& name,const std::filesystem::path& location)
{
    zengine::projects::ValidateName(name);
    if (!ConfirmProjectClose()) return false;
    ActivateProject(zengine::projects::Create(location,name));
    return true;
}
bool EditorShell::OpenProject(const std::filesystem::path& file)
{
    auto next=zengine::projects::Open(file); // Invalid configs never discard the current project.
    if (!ConfirmProjectClose()) return false;
    next=zengine::projects::Open(next.file); // A Save in the prompt may have updated this same config.
    ActivateProject(std::move(next));
    if (!project_->config.lastScene.empty())
    {
        try
        {
            const auto scene=zengine::projects::ScenePath(*project_,project_->config.lastScene);
            const auto source=zengine::scenes::Load(scene);
            ApplyScene(scene,source,zengine::scenes::Decode(source));
        }
        catch (const std::exception& error)
        { status_=L"Last scene unavailable: "+Wide(error.what()); }
    }
    InvalidateRect(window_,nullptr,FALSE);
    return true;
}
void EditorShell::PromptForScene()
{
    if (!project_ || sceneOpen_) return;
    const auto message=status_+L"\n\nNo scene is open. Create a new scene?\n\nYes: Create a new scene\nNo: Open an existing scene in this project's Assets\nCancel: Leave the project open without a scene";
    const int answer=MessageBoxW(window_,message.c_str(),L"Open a scene",MB_YESNOCANCEL|MB_ICONQUESTION);
    if (answer==IDYES) NewScene();
    else if (answer==IDNO) ChooseScene();
}
void EditorShell::NewProjectDialog()
{
    if (Playing()) throw std::runtime_error("Stop Play before creating a project.");
    const auto request=ProjectDialog::Show(window_,project_?project_->file.parent_path().parent_path():std::filesystem::path{});
    if (request && CreateProject(request->name,request->location)) PromptForScene();
}
void EditorShell::ChooseProject()
{
    if (Playing()) throw std::runtime_error("Stop Play before opening a project.");
    std::array<wchar_t,32768> file{};
    const auto initial=project_?project_->file.parent_path().wstring():std::wstring{};
    OPENFILENAMEW dialog{sizeof(dialog)}; dialog.hwndOwner=window_; dialog.lpstrFile=file.data(); dialog.nMaxFile=static_cast<DWORD>(file.size());
    dialog.lpstrFilter=L"zEngine projects (*.zproject)\0*.zproject\0\0"; dialog.lpstrInitialDir=initial.empty()?nullptr:initial.c_str();
    dialog.lpstrTitle=L"Open a zEngine project config";
    dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog) && OpenProject(file.data())) PromptForScene();
}
void EditorShell::InitializeStartup(const std::filesystem::path& sessionFile)
{
    try
    {
        recentSessionFile_=sessionFile.empty()?zengine::projects::DefaultSessionFile():sessionFile;
        if (const auto recent=zengine::projects::ReadRecent(recentSessionFile_))
        {
            if (!OpenProject(*recent)) return;
            PromptForScene(); return;
        }
        status_=L"No recent project is saved.";
    }
    catch (const std::exception& error) { status_=L"Could not reopen the previous project: "+Wide(error.what()); }
    const auto message=status_+L"\n\nCreate a new project?\n\nYes: Create a project\nNo: Open an existing .zproject config\nCancel: Leave the editor empty";
    const int answer=MessageBoxW(window_,message.c_str(),L"Welcome to zEngine",MB_YESNOCANCEL|MB_ICONQUESTION);
    if (answer==IDYES) NewProjectDialog();
    else if (answer==IDNO) ChooseProject();
    InvalidateRect(window_,nullptr,FALSE);
}
