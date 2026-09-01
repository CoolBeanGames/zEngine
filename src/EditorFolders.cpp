#include "EditorShell.h"
#include "AssetLibrary.h"
#include "Project.h"
#include "SceneAssets.h"
#include "PrefabAssets.h"
#include "ScriptAssets.h"
#include "ScriptEditor.h"
#include <fstream>
#include <cwctype>

namespace {
std::string RelativeAsset(const std::filesystem::path& path,const std::filesystem::path& assets){const auto value=std::filesystem::relative(path,assets).generic_u8string();return {reinterpret_cast<const char*>(value.data()),value.size()};}
bool Rewrite(std::string& value,const std::string& from,const std::string& to){if(value==from){value=to;return true;}if(value.starts_with(from+"/")){value=to+value.substr(from.size());return true;}return false;}
bool Rewrite(zengine::scenes::Document& document,const std::string& from,const std::string& to){bool changed=false;for(auto& object:document.objects){changed|=Rewrite(object.prefab,from,to);for(auto& behavior:object.behaviors){changed|=Rewrite(behavior.asset,from,to);for(auto& [name,value]:behavior.variables)if(auto* prefab=std::get_if<zengine::script::PrefabRef>(&value))changed|=Rewrite(prefab->asset,from,to);}}return changed;}
bool Within(const std::filesystem::path& child,const std::filesystem::path& parent){const auto c=std::filesystem::weakly_canonical(child),p=std::filesystem::weakly_canonical(parent);auto ci=c.begin();for(auto pi=p.begin();pi!=p.end();++pi,++ci)if(ci==c.end()||_wcsicmp(pi->c_str(),ci->c_str()))return false;return true;}
void ReplaceFile(const std::filesystem::path& path,std::string_view text){auto temp=path;temp+=L".move-save";{std::ofstream out(temp,std::ios::binary|std::ios::trunc);out.write(text.data(),static_cast<std::streamsize>(text.size()));out.flush();if(!out)throw std::runtime_error("Cannot update moved asset references.");}if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temp.c_str());throw std::runtime_error("Cannot replace an updated asset file.");}}
}
std::filesystem::path EditorShell::AssetFolder() const {return assetFolder_.empty()?assetsDirectory_:assetLibrary::Resolve(assetsDirectory_,assetFolder_);}
void EditorShell::OpenAssetFolder(const std::filesystem::path& path) {
    RequireProject();const auto folder=assetLibrary::Resolve(assetsDirectory_,path);
    if(!std::filesystem::is_directory(folder)||assetLibrary::Package(folder))throw std::runtime_error("Choose an asset folder.");
    assetFolder_=folder;firstAsset_=0;RefreshAssets();InvalidateRect(window_,nullptr,FALSE);
}
std::filesystem::path EditorShell::CreateAssetFolder(const std::wstring& name) {
    RequireProject();zengine::projects::ValidateName(name);
    const auto folder=assetLibrary::Resolve(assetsDirectory_,AssetFolder()/name);
    if(!std::filesystem::create_directory(folder))throw std::runtime_error("A folder with this name already exists.");
    RefreshAssets();InvalidateRect(window_,nullptr,FALSE);return folder;
}
void EditorShell::NewAssetFolderDialog() {
    RequireProject();std::wstring name=L"New Folder";for(unsigned suffix=1;std::filesystem::exists(AssetFolder()/name);++suffix)name=L"New Folder "+std::to_wstring(suffix);
    BeginAssetRename(CreateAssetFolder(name));
}
void EditorShell::MoveAsset(const std::filesystem::path& asset,const std::filesystem::path& folder)
{
    RequireProject();if(Playing())throw std::runtime_error("Stop Play before moving assets.");
    if(assetLibrary::Type(asset)==assetLibrary::Kind::Input)throw std::runtime_error("The project Input Map stays in the Assets root.");const auto source=assetLibrary::Storage(asset),destinationFolder=assetLibrary::Resolve(assetsDirectory_,folder);if(!std::filesystem::is_directory(destinationFolder)||assetLibrary::Package(destinationFolder))throw std::runtime_error("Drop onto an asset folder.");
    const auto destination=assetLibrary::Resolve(assetsDirectory_,destinationFolder/source.filename());if(source==destination)return;if(std::filesystem::exists(destination))throw std::runtime_error("That folder already contains an asset with this name.");
    for(const auto& editor:scriptEditors_)if(Within(editor->Path(),source))throw std::runtime_error("Close this script editor before moving or renaming its asset.");
    const auto from=RelativeAsset(source,assetsDirectory_),to=RelativeAsset(destination,assetsDirectory_);
    std::filesystem::rename(source,destination);
    for(const auto& entry:std::filesystem::recursive_directory_iterator(assetsDirectory_))if(entry.is_regular_file()){
        if(zengine::scenes::IsScene(entry.path())){const auto original=zengine::scenes::Load(entry.path());auto document=zengine::scenes::Decode(original);if(Rewrite(document,from,to))ReplaceFile(entry.path(),zengine::scenes::Encode(document));}
        else if(zengine::prefabs::IsPrefab(entry.path())){const auto original=zengine::scenes::Load(entry.path());auto document=zengine::prefabs::Decode(original);if(Rewrite(document,from,to))ReplaceFile(entry.path(),zengine::prefabs::Encode(document));}
    }
    if(project_){const auto projectFrom="Assets/"+from,projectTo="Assets/"+to;for(auto& scene:project_->config.scenes)Rewrite(scene,projectFrom,projectTo);Rewrite(project_->config.lastScene,projectFrom,projectTo);zengine::projects::Save(*project_);}
    if(sceneOpen_){const auto storageScene=assetLibrary::Storage(scenePath_);if(storageScene==source || RelativeAsset(storageScene,assetsDirectory_).starts_with(from+"/"))scenePath_=destination/std::filesystem::relative(storageScene,source);const auto sourceText=zengine::scenes::Load(scenePath_);ApplyScene(scenePath_,sourceText,zengine::scenes::Decode(sourceText));}
    if(AssetFolder()==source || RelativeAsset(AssetFolder(),assetsDirectory_).starts_with(from+"/"))assetFolder_=destination/std::filesystem::relative(AssetFolder(),source);
    RefreshAssets();status_=L"Moved asset to "+destinationFolder.filename().wstring();InvalidateRect(window_,nullptr,FALSE);
}
void EditorShell::RenameAsset(const std::filesystem::path& asset,const std::wstring& requested)
{
    if(assetLibrary::Type(asset)==assetLibrary::Kind::Input)throw std::runtime_error("The project Input Map cannot be renamed.");
    auto source=assetLibrary::Storage(asset);for(const auto& editor:scriptEditors_)if(Within(editor->Path(),source))throw std::runtime_error("Close this script editor before moving or renaming its asset.");std::wstring name=requested;while(!name.empty()&&std::iswspace(name.back()))name.pop_back();while(!name.empty()&&std::iswspace(name.front()))name.erase(name.begin());
    if(name.empty())throw std::runtime_error("Asset name cannot be empty.");
    if(source.has_extension() && std::filesystem::path(name).extension().empty())name+=source.extension().wstring();zengine::projects::ValidateName(name);
    const auto destination=source.parent_path()/name;if(_wcsicmp(source.c_str(),destination.c_str())==0)return;MoveAsset(asset,source.parent_path()); // Validates state; same-folder is intentionally a no-op.
    if(std::filesystem::exists(destination))throw std::runtime_error("An asset with that name already exists.");
    const auto temporary=source.parent_path()/(source.filename().wstring()+L".rename-temp");if(_wcsicmp(source.filename().c_str(),destination.filename().c_str())==0){std::filesystem::rename(source,temporary);std::filesystem::rename(temporary,destination);}else std::filesystem::rename(source,destination);
    const auto from=RelativeAsset(source,assetsDirectory_),to=RelativeAsset(destination,assetsDirectory_);
    for(const auto& entry:std::filesystem::recursive_directory_iterator(assetsDirectory_))if(entry.is_regular_file()){
        if(zengine::scenes::IsScene(entry.path())){const auto original=zengine::scenes::Load(entry.path());auto document=zengine::scenes::Decode(original);if(Rewrite(document,from,to))ReplaceFile(entry.path(),zengine::scenes::Encode(document));}
        else if(zengine::prefabs::IsPrefab(entry.path())){const auto original=zengine::scenes::Load(entry.path());auto document=zengine::prefabs::Decode(original);if(Rewrite(document,from,to))ReplaceFile(entry.path(),zengine::prefabs::Encode(document));}
    }
    if(project_){const auto a="Assets/"+from,b="Assets/"+to;for(auto& scene:project_->config.scenes)Rewrite(scene,a,b);Rewrite(project_->config.lastScene,a,b);zengine::projects::Save(*project_);}
    if(sceneOpen_){if(Within(scenePath_,source))scenePath_=destination/std::filesystem::relative(scenePath_,source);const auto sourceText=zengine::scenes::Load(scenePath_);ApplyScene(scenePath_,sourceText,zengine::scenes::Decode(sourceText));}
    if(Within(AssetFolder(),source))assetFolder_=destination/std::filesystem::relative(AssetFolder(),source);
    RefreshAssets();status_=L"Renamed asset to "+destination.filename().wstring();InvalidateRect(window_,nullptr,FALSE);
}
