#include "SceneAssets.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
namespace zengine::scenes
{
bool IsScene(const std::filesystem::path& path)
{
    auto ext=path.extension().wstring();
    std::transform(ext.begin(),ext.end(),ext.begin(),[](wchar_t c) { return std::towlower(c); }); return ext==L".zscene";
}
std::filesystem::path Resolve(const std::filesystem::path& assets,const std::filesystem::path& path)
{
    const auto base=std::filesystem::weakly_canonical(assets);
    const auto file=std::filesystem::weakly_canonical(path.is_absolute()?path:base/path);
    auto b=base.begin(),f=file.begin();
    for (;b!=base.end();++b,++f)
        if (f==file.end() || _wcsicmp(b->c_str(),f->c_str())!=0) throw std::runtime_error("Scenes must be inside the project's Assets directory.");
    if (f==file.end() || !IsScene(file)) throw std::runtime_error("Choose a .zscene asset."); return file;
}
std::string Load(const std::filesystem::path& path)
{
    const auto size=std::filesystem::file_size(path);
    if (size>MaxSceneBytes) throw std::runtime_error("Scene exceeds the 8 MiB limit.");
    std::ifstream file(path,std::ios::binary); std::string text(static_cast<std::size_t>(size),'\0');
    if (!file || !file.read(text.data(),static_cast<std::streamsize>(size))) throw std::runtime_error("Cannot read scene.");
    if (text.find('\0')!=std::string::npos || (!text.empty() && !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0)))
        throw std::runtime_error("Scene must be UTF-8 text."); return text;
}
void Save(const std::filesystem::path& assets,const std::filesystem::path& path,std::string_view text,const std::string* expected)
{
    if (text.size()>MaxSceneBytes || text.find('\0')!=std::string_view::npos || (!text.empty() && !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0)))
        throw std::runtime_error("Scene must be bounded UTF-8 text.");
    Decode(text);
    const auto target=Resolve(assets,path);
    if (expected && Load(target)!=*expected) throw std::runtime_error("Scene changed on disk. Open it again or Save As a different scene.");
    if (!expected && std::filesystem::exists(target)) throw std::runtime_error("Scene already exists.");
    std::filesystem::path temp; HANDLE file=INVALID_HANDLE_VALUE;
    for (unsigned i=0;i<1000 && file==INVALID_HANDLE_VALUE;++i)
    {
        temp=target; temp+=L".save-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(i);
        file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file==INVALID_HANDLE_VALUE && GetLastError()!=ERROR_FILE_EXISTS) throw std::runtime_error("Cannot create scene save file.");
    }
    if (file==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create scene save file.");
    try
    {
        DWORD written=0;
        if (!WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) || written!=text.size() || !FlushFileBuffers(file)) throw std::runtime_error("Cannot write scene.");
        CloseHandle(file); file=INVALID_HANDLE_VALUE;
        if (expected && Load(target)!=*expected) throw std::runtime_error("Scene changed during save; original preserved.");
        if (!MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH|(expected?MOVEFILE_REPLACE_EXISTING:0))) throw std::runtime_error("Cannot replace scene; original preserved.");
    }
    catch (...) { if (file!=INVALID_HANDLE_VALUE) CloseHandle(file); DeleteFileW(temp.c_str()); throw; }
}
std::filesystem::path Create(const std::filesystem::path& assets)
{
    std::filesystem::create_directories(assets);
    for (unsigned i=0;i<10000;++i)
    {
        const auto file=Resolve(assets,std::filesystem::path("NewScene"+(i?std::to_string(i):"")+".zscene"));
        if (std::filesystem::exists(file)) continue;
        Save(assets,file,Encode({})); return file;
    }
    throw std::runtime_error("Too many scenes with this name.");
}
}
