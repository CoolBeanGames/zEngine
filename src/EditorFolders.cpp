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
std::string Narrow(const std::wstring& value){if(value.empty())return {};const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);if(!count)throw std::runtime_error("Script name is not valid UTF-8.");std::string out(count,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),count,nullptr,nullptr);return out;}
std::string RenameScriptClass(std::string source,const std::string& className){auto identifier=[](unsigned char c){return std::isalnum(c)||c=='_';};for(std::size_t p=0;p+5<=source.size();++p){if(source.compare(p,5,"class")!=0||(p&&identifier(static_cast<unsigned char>(source[p-1])))||(p+5<source.size()&&identifier(static_cast<unsigned char>(source[p+5]))))continue;auto name=p+5;while(name<source.size()&&std::isspace(static_cast<unsigned char>(source[name])))++name;const auto end=name;while(name<source.size()&&identifier(static_cast<unsigned char>(source[name])))++name;if(name==end)continue;source.replace(end,name-end,className);return source;}return source;}
bool ScriptIdentifier(const std::string& value){if(value.empty()||(!std::isalpha(static_cast<unsigned char>(value.front()))&&value.front()!='_'))return false;return std::all_of(value.begin()+1,value.end(),[](unsigned char c){return std::isalnum(c)||c=='_';});}
std::string RelativeAsset(const std::filesystem::path& path,const std::filesystem::path& assets){const auto value=std::filesystem::relative(path,assets).generic_u8string();return {reinterpret_cast<const char*>(value.data()),value.size()};}
bool Rewrite(std::string& value,const std::string& from,const std::string& to){if(value==from){value=to;return true;}if(value.starts_with(from+"/")){value=to+value.substr(from.size());return true;}return false;}
bool Rewrite(zengine::scenes::Document& document,const std::string& from,const std::string& to){bool changed=false;for(auto& object:document.objects){changed|=Rewrite(object.prefab,from,to);for(auto& behavior:object.behaviors){changed|=Rewrite(behavior.asset,from,to);for(auto& [name,value]:behavior.variables)if(auto* prefab=std::get_if<zengine::script::PrefabRef>(&value))changed|=Rewrite(prefab->asset,from,to);}}return changed;}
bool Within(const std::filesystem::path& child,const std::filesystem::path& parent){const auto c=std::filesystem::weakly_canonical(child),p=std::filesystem::weakly_canonical(parent);auto ci=c.begin();for(auto pi=p.begin();pi!=p.end();++pi,++ci)if(ci==c.end()||_wcsicmp(pi->c_str(),ci->c_str()))return false;return true;}
// ZE-114: remap `path` when `source` (a file or a folder that contains it) moves
// to `destination`. std::filesystem::relative(path,source) is "." when path==source,
// so `destination/relative(...)` would yield "<destination>/." - use `destination`.
std::filesystem::path Remap(const std::filesystem::path& path,const std::filesystem::path& source,const std::filesystem::path& destination){
    return _wcsicmp(std::filesystem::weakly_canonical(path).c_str(),std::filesystem::weakly_canonical(source).c_str())==0
        ? destination : destination/std::filesystem::relative(path,source);
}
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
void EditorShell::RefreshOpenDocumentAfterAssetMove(const std::filesystem::path& source,const std::filesystem::path& destination,const std::string& from,const std::string& to)
{
    if(!sceneOpen_)return;
    auto live=CaptureDocument();Rewrite(live,from,to);const auto selected=selectedObject_;
    if(Within(scenePath_,source))scenePath_=Remap(scenePath_,source,destination);
    if(!editingPrefab_.empty()&&Within(editingPrefab_,source))editingPrefab_=Remap(editingPrefab_,source,destination);
    if(prefabReturn_)
    {
        Rewrite(prefabReturn_->document,from,to);
        if(Within(prefabReturn_->path,source))prefabReturn_->path=Remap(prefabReturn_->path,source,destination);
        prefabReturn_->source=zengine::scenes::Load(prefabReturn_->path);
        prefabReturn_->baseline=zengine::scenes::Encode(zengine::scenes::Decode(prefabReturn_->source));
    }
    const auto diskSource=zengine::scenes::Load(scenePath_);
    const auto diskDocument=editingPrefab_.empty()?zengine::scenes::Decode(diskSource):zengine::prefabs::Decode(diskSource);
    ApplyScene(scenePath_,diskSource,live);
    sceneBaseline_=zengine::scenes::Encode(diskDocument);
    sceneDirty_=zengine::scenes::Encode(CaptureDocument())!=sceneBaseline_;
    if(selected&&objects_.Find(selected))SelectGameObject(selected);
    UpdateSceneTitle();
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
    RefreshOpenDocumentAfterAssetMove(source,destination,from,to);
    if(AssetFolder()==source || RelativeAsset(AssetFolder(),assetsDirectory_).starts_with(from+"/"))assetFolder_=Remap(AssetFolder(),source,destination);
    RefreshAssets();status_=L"Moved asset to "+destinationFolder.filename().wstring();InvalidateRect(window_,nullptr,FALSE);
}
void EditorShell::RenameAsset(const std::filesystem::path& asset,const std::wstring& requested)
{
    if(assetLibrary::Type(asset)==assetLibrary::Kind::Input)throw std::runtime_error("The project Input Map cannot be renamed.");
    auto source=assetLibrary::Storage(asset);for(const auto& editor:scriptEditors_)if(Within(editor->Path(),source))throw std::runtime_error("Close this script editor before moving or renaming its asset.");std::wstring name=requested;while(!name.empty()&&std::iswspace(name.back()))name.pop_back();while(!name.empty()&&std::iswspace(name.front()))name.erase(name.begin());
    if(name.empty())throw std::runtime_error("Asset name cannot be empty.");
    if(source.has_extension() && std::filesystem::path(name).extension().empty())name+=source.extension().wstring();zengine::projects::ValidateName(name);
    const auto destination=source.parent_path()/name;if(_wcsicmp(source.c_str(),destination.c_str())==0)return;
    std::optional<std::string> renamedScript;
    if(assetLibrary::Type(source)==assetLibrary::Kind::Script){const auto className=Narrow(destination.stem().wstring());if(!ScriptIdentifier(className))throw std::runtime_error("Script names must be identifiers: letters, digits, underscores; not starting with a digit.");renamedScript=RenameScriptClass(zengine::scripts::Load(source),className);}
    MoveAsset(asset,source.parent_path()); // Validates state; same-folder is intentionally a no-op.
    if(std::filesystem::exists(destination))throw std::runtime_error("An asset with that name already exists.");
    const auto temporary=source.parent_path()/(source.filename().wstring()+L".rename-temp");if(_wcsicmp(source.filename().c_str(),destination.filename().c_str())==0){std::filesystem::rename(source,temporary);std::filesystem::rename(temporary,destination);}else std::filesystem::rename(source,destination);
    if(renamedScript)zengine::scripts::Save(assetsDirectory_,destination,*renamedScript);
    const auto from=RelativeAsset(source,assetsDirectory_),to=RelativeAsset(destination,assetsDirectory_);
    for(const auto& entry:std::filesystem::recursive_directory_iterator(assetsDirectory_))if(entry.is_regular_file()){
        if(zengine::scenes::IsScene(entry.path())){const auto original=zengine::scenes::Load(entry.path());auto document=zengine::scenes::Decode(original);if(Rewrite(document,from,to))ReplaceFile(entry.path(),zengine::scenes::Encode(document));}
        else if(zengine::prefabs::IsPrefab(entry.path())){const auto original=zengine::scenes::Load(entry.path());auto document=zengine::prefabs::Decode(original);if(Rewrite(document,from,to))ReplaceFile(entry.path(),zengine::prefabs::Encode(document));}
    }
    if(project_){const auto a="Assets/"+from,b="Assets/"+to;for(auto& scene:project_->config.scenes)Rewrite(scene,a,b);Rewrite(project_->config.lastScene,a,b);zengine::projects::Save(*project_);}
    RefreshOpenDocumentAfterAssetMove(source,destination,from,to);
    if(Within(AssetFolder(),source))assetFolder_=Remap(AssetFolder(),source,destination);
    RefreshAssets();status_=L"Renamed asset to "+destination.filename().wstring();InvalidateRect(window_,nullptr,FALSE);
}
