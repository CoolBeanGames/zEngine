#include "EditorShell.h"
#include "runtime/GamePackage.h"
#include "ScriptEditor.h"
#include "input/InputMapEditor.h"
#include <shobjidl.h>

bool EditorShell::BuildProject(const std::filesystem::path& outputParent) {
    RequireScene();
    if(buildWork_.valid())throw std::runtime_error("A game build is already running.");
    if(Playing() || !editingPrefab_.empty())throw std::runtime_error("Stop Play and close prefab editing before building.");
    if(assetWork_.valid() || !assetJobs_.empty())throw std::runtime_error("Wait for imports and model loading before building.");
    for(const auto& editor:scriptEditors_)if(editor->Dirty())throw std::runtime_error("Save open script edits before building.");
    if(inputEditor_ && inputEditor_->Dirty())throw std::runtime_error("Save the Input Map before building.");
    if(!PrepareScripts() || !SaveScene())return false;
    auto player=zengine::game::ExecutableDirectory();
#ifdef _DEBUG
    if(std::filesystem::is_regular_file(player.parent_path()/L"Release"/L"zPlayer.exe"))player=player.parent_path()/L"Release";
#endif
    const auto project=*project_;const auto scene=scenePath_;const zengine::game::Settings settings{sceneCamera_};
    buildProgress_=std::make_shared<BuildProgress>();const auto progress=buildProgress_;lastBuild_.clear();buildError_.clear();
    buildWork_=std::async(std::launch::async,[project,scene,settings,outputParent,player,progress] {
        return zengine::game::Export(project,scene,settings,outputParent,player,[progress](unsigned percent,const std::string& message){std::lock_guard lock(progress->mutex);progress->percent=percent;progress->message=message;});
    });
    status_=L"Building standalone game...";InvalidateRect(window_,nullptr,FALSE);return true;
}
void EditorShell::ChooseBuildFolder() {
    RequireScene();IFileOpenDialog* picker=nullptr;
    if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&picker))))throw std::runtime_error("Cannot open build destination picker.");
    DWORD flags=0;picker->GetOptions(&flags);picker->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);
    picker->SetTitle(L"Choose a destination outside the project (a new game folder will be created)");
    std::filesystem::path path;
    if(SUCCEEDED(picker->Show(window_))) {IShellItem* item=nullptr;if(SUCCEEDED(picker->GetResult(&item))){PWSTR text=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&text))){path=text;CoTaskMemFree(text);}item->Release();}}
    picker->Release();if(!path.empty())BuildProject(path);
}
void EditorShell::PollBuild() {
    if(!buildWork_.valid())return;
    if(buildWork_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try {lastBuild_=buildWork_.get();status_=L"Built standalone game: "+lastBuild_.wstring();}
        catch(const std::exception& e){buildError_=e.what();const auto count=MultiByteToWideChar(CP_UTF8,0,buildError_.data(),static_cast<int>(buildError_.size()),nullptr,0);std::wstring message(count,L' ');MultiByteToWideChar(CP_UTF8,0,buildError_.data(),static_cast<int>(buildError_.size()),message.data(),count);status_=L"Build failed: "+message;}
        InvalidateRect(window_,nullptr,FALSE);
    } else if(GetTickCount64()-lastBusyPaint_>100){lastBusyPaint_=GetTickCount64();InvalidateRect(window_,&statusBar_,FALSE);}
}
