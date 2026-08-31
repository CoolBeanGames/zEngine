#include "PrefabAssets.h"
#include "SceneAssets.h"
#include <windows.h>

namespace zengine::prefabs
{
bool IsPrefab(const std::filesystem::path& file) { return file.extension()==L".zprefab"; }
std::filesystem::path Resolve(const std::filesystem::path& assets,const std::filesystem::path& path)
{
    const auto root=std::filesystem::weakly_canonical(assets),file=std::filesystem::weakly_canonical(path.is_absolute()?path:root/path);
    auto r=root.begin(),f=file.begin();
    for (;r!=root.end();++r,++f) if (f==file.end() || _wcsicmp(r->c_str(),f->c_str())!=0) throw std::runtime_error("Prefabs must belong to this project's Assets folder.");
    if (f==file.end() || !IsPrefab(file)) throw std::runtime_error("Choose a .zprefab asset."); return file;
}
scenes::Document Load(const std::filesystem::path& assets,const std::filesystem::path& file) { return Decode(scenes::Load(Resolve(assets,file))); }
Expansion ResolveScene(const std::filesystem::path& assets,const scenes::Document& document)
{
    return Expand(document,[&](const std::string& ref) { return Load(assets,std::filesystem::path(std::u8string(ref.begin(),ref.end()))); });
}
void Save(const std::filesystem::path& assets,const std::filesystem::path& path,const scenes::Document& document,const std::string* expected)
{
    const auto target=Resolve(assets,path); const auto text=prefabs::Encode(document);
    // Resolve the proposed graph, substituting this file, before touching disk. This catches indirect cycles too.
    scenes::Document check; scenes::ObjectData link; link.id=1; link.name="Prefab";
    const auto ref=std::filesystem::relative(target,assets).generic_u8string(); link.prefab.assign(reinterpret_cast<const char*>(ref.data()),ref.size()); check.objects.push_back(link);
    Expand(check,[&](const std::string& asset) {
        const auto file=Resolve(assets,std::filesystem::path(std::u8string(asset.begin(),asset.end())));
        return _wcsicmp(file.c_str(),target.c_str())==0?document:Load(assets,file);
    });
    if (expected && scenes::Load(target)!=*expected) throw std::runtime_error("Prefab changed on disk. Reopen it before saving.");
    if (!expected && std::filesystem::exists(target)) throw std::runtime_error("Prefab already exists.");
    std::filesystem::path temp; HANDLE file=INVALID_HANDLE_VALUE;
    for (unsigned i=0;i<1000 && file==INVALID_HANDLE_VALUE;++i)
    {
        temp=target; temp+=L".save-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(i);
        file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file==INVALID_HANDLE_VALUE && GetLastError()!=ERROR_FILE_EXISTS) throw std::runtime_error("Cannot create prefab save file.");
    }
    if (file==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create prefab save file.");
    try
    {
        DWORD written=0;
        if (!WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)||written!=text.size()||!FlushFileBuffers(file)) throw std::runtime_error("Cannot write prefab.");
        CloseHandle(file); file=INVALID_HANDLE_VALUE;
        if (expected && scenes::Load(target)!=*expected) throw std::runtime_error("Prefab changed during save; original preserved.");
        if (!MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH|(expected?MOVEFILE_REPLACE_EXISTING:0))) throw std::runtime_error("Cannot replace prefab; original preserved.");
    }
    catch (...) { if (file!=INVALID_HANDLE_VALUE) CloseHandle(file); DeleteFileW(temp.c_str()); throw; }
}
std::filesystem::path Create(const std::filesystem::path& assets,const scenes::Document& document)
{
    std::filesystem::create_directories(assets);
    for (unsigned i=0;i<10000;++i)
    {
        const auto file=assets/(L"NewPrefab"+(i?std::to_wstring(i):L"")+L".zprefab");
        if (!std::filesystem::exists(file)) { Save(assets,file,document); return file; }
    }
    throw std::runtime_error("Too many prefabs with this name.");
}
}
