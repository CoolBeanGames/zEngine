#include "Project.h"
#include <cstdint>
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace zengine::projects
{
namespace
{
    constexpr std::size_t Limit=1024*1024;
    std::string Utf8(const std::wstring& text)
    {
        const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
        if (!text.empty() && !count) throw std::runtime_error("Invalid project text.");
        std::string out(count,' '); WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),count,nullptr,nullptr); return out;
    }
    std::wstring Wide(const std::string& text)
    {
        const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
        if (!text.empty() && !count) throw std::runtime_error("Project config must be UTF-8.");
        std::wstring out(count,L' '); MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),count); return out;
    }
    void Require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
    void Token(std::istream& in,const char* expected) { std::string s; Require(static_cast<bool>(in>>s)&&s==expected,"Invalid project config structure/version."); }
    std::string Quoted(std::istream& in)
    {
        in>>std::ws; Require(in.peek()=='"',"Expected quoted project text.");
        std::string text; Require(static_cast<bool>(in>>std::quoted(text))&&text.find('\0')==std::string::npos,"Invalid project text."); return text;
    }
    bool Extension(const std::filesystem::path& path,const wchar_t* ext) { return _wcsicmp(path.extension().c_str(),ext)==0; }
    void Reference(const std::string& ref)
    {
        Require(ref.starts_with("Assets/") && ref.find_first_of("\\:\r\n") == std::string::npos,"Scene reference must be relative to project Assets.");
        std::istringstream parts(ref); std::string p;
        while (std::getline(parts,p,'/')) Require(!p.empty() && p!="." && p!="..","Invalid project scene reference.");
        Require(Extension(std::filesystem::path(Wide(ref)),L".zscene"),"Project scene must be a .zscene asset.");
    }
    void Contained(const std::filesystem::path& base,const std::filesystem::path& target)
    {
        auto b=base.begin(),t=target.begin();
        for (;b!=base.end();++b,++t) Require(t!=target.end() && _wcsicmp(b->c_str(),t->c_str())==0,"Project asset path escapes its owner.");
        Require(t!=target.end(),"Expected a path inside the project.");
    }
    std::string Read(const std::filesystem::path& file,bool validateText=true)
    {
        const auto size=std::filesystem::file_size(file); Require(size<=Limit,"Project file exceeds 1 MiB.");
        std::ifstream in(file,std::ios::binary); std::string text(static_cast<std::size_t>(size),'\0');
        Require(static_cast<bool>(in.read(text.data(),static_cast<std::streamsize>(size))),"Cannot read project file.");
        if (validateText) { Require(text.find('\0')==std::string::npos,"Project file contains NUL text."); Wide(text); }
        return text;
    }
    void Write(const std::filesystem::path& target,const std::string& text,const std::string* expected)
    {
        Require(text.size()<=Limit,"Project file exceeds 1 MiB.");
        if (expected) Require(Read(target,false)==*expected,"Project config changed externally. Reopen it before changing its scene list.");
        else Require(!std::filesystem::exists(target),"Project file already exists.");
        std::filesystem::path temp; HANDLE file=INVALID_HANDLE_VALUE;
        for (unsigned n=0;n<1000 && file==INVALID_HANDLE_VALUE;++n)
        {
            temp=target; temp+=L".save-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(n);
            file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if (file==INVALID_HANDLE_VALUE && GetLastError()!=ERROR_FILE_EXISTS) throw std::runtime_error("Cannot create project save file.");
        }
        Require(file!=INVALID_HANDLE_VALUE,"Cannot create project save file.");
        try
        {
            DWORD written=0; Require(WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size()&&FlushFileBuffers(file),"Cannot write project file.");
            CloseHandle(file); file=INVALID_HANDLE_VALUE;
            if (expected) Require(Read(target,false)==*expected,"Project config changed during save; original preserved.");
            Require(MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH|(expected?MOVEFILE_REPLACE_EXISTING:0))!=0,"Cannot replace project file; original preserved.");
        }
        catch (...) { if (file!=INVALID_HANDLE_VALUE) CloseHandle(file); DeleteFileW(temp.c_str()); throw; }
    }
}
void ValidateName(const std::wstring& name)
{
    Require(!name.empty() && name.size()<=80 && name!=L"." && name!=L".." && name.back()!=L'.' && name.back()!=L' ' && name.front()!=L' ',"Choose a project name (1-80 characters, no leading/trailing spaces or trailing dot).");
    Require(name.find_first_of(L"<>:\"/\\|?*")==std::wstring::npos && std::none_of(name.begin(),name.end(),[](wchar_t c) { return c<32; }),"Project name contains invalid folder characters.");
    auto base=name.substr(0,name.find(L'.')); std::transform(base.begin(),base.end(),base.begin(),[](wchar_t c) { return std::towupper(c); });
    Require(base!=L"CON"&&base!=L"PRN"&&base!=L"AUX"&&base!=L"NUL"&&!(base.size()==4 && (base.starts_with(L"COM")||base.starts_with(L"LPT")) && base[3]>=L'1'&&base[3]<=L'9'),"Project name is a reserved Windows device name.");
    Utf8(name);
}
std::string Encode(const Config& config)
{
    std::ostringstream out; out<<"ZENGINE_PROJECT 1\nname "<<std::quoted(config.name)<<"\nassets \"Assets\"\nlast_scene "<<std::quoted(config.lastScene)<<"\nscenes "<<config.scenes.size()<<'\n';
    for (const auto& scene:config.scenes) out<<std::quoted(scene)<<'\n';
    out<<"end\n"; auto text=out.str(); Decode(text); return text;
}
Config Decode(const std::string& text)
{
    Require(text.size()<=Limit && text.find('\0')==std::string::npos,"Invalid/oversized project config."); Wide(text);
    std::istringstream in(text); Token(in,"ZENGINE_PROJECT"); Token(in,"1"); Token(in,"name");
    Config c; c.name=Quoted(in); ValidateName(Wide(c.name)); Token(in,"assets"); Require(Quoted(in)=="Assets","Project assets must use the owned Assets folder.");
    Token(in,"last_scene"); c.lastScene=Quoted(in); if (!c.lastScene.empty()) Reference(c.lastScene);
    Token(in,"scenes"); std::uint64_t count; Require(static_cast<bool>(in>>count)&&count<=10000,"Invalid project scene count.");
    std::set<std::wstring> unique;
    for (std::uint64_t i=0;i<count;++i)
    {
        auto scene=Quoted(in); Reference(scene); auto key=Wide(scene); std::transform(key.begin(),key.end(),key.begin(),[](wchar_t v) { return std::towlower(v); });
        Require(unique.insert(key).second,"Duplicate scene reference."); c.scenes.push_back(std::move(scene));
    }
    Require(c.lastScene.empty()||std::find(c.scenes.begin(),c.scenes.end(),c.lastScene)!=c.scenes.end(),"Last scene is not in the project scene list.");
    Token(in,"end"); in>>std::ws; Require(in.eof(),"Unexpected project data."); return c;
}
std::filesystem::path Assets(const Project& project)
{
    const auto root=std::filesystem::weakly_canonical(project.file.parent_path());
    const auto assets=std::filesystem::weakly_canonical(root/L"Assets"); Contained(root,assets);
    Require(!std::filesystem::exists(assets)||std::filesystem::is_directory(assets),"Project Assets path is not a folder."); return assets;
}
std::filesystem::path ScenePath(const Project& project,const std::string& ref)
{
    Reference(ref); const auto scene=std::filesystem::weakly_canonical(project.file.parent_path()/std::filesystem::path(Wide(ref)));
    Contained(Assets(project),scene); return scene;
}
Project Open(const std::filesystem::path& file)
{
    Require(Extension(file,L".zproject"),"Choose a .zproject config file.");
    Project p; p.file=std::filesystem::canonical(file); p.source=Read(p.file); p.config=Decode(p.source); Assets(p);
    for (const auto& ref:p.config.scenes) ScenePath(p,ref); return p;
}
Project Create(const std::filesystem::path& location,const std::wstring& name)
{
    ValidateName(name); Require(std::filesystem::is_directory(location),"Choose an existing parent folder for the project.");
    const auto parent=std::filesystem::canonical(location),root=parent/name; Contained(parent,root);
    Require(std::filesystem::create_directory(root),"That project folder already exists. Choose a different name or location.");
    // Do not delete a newly created folder on failure: partial files are recoverable.
    std::filesystem::create_directory(root/L"Assets");
    Project p{root/(name+L".zproject"),Config{Utf8(name),{},{}},{}};
    p.source=Encode(p.config); Write(p.file,p.source,nullptr); return p;
}
Project InitializeDirectory(const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory);
    const auto root=std::filesystem::canonical(directory); std::vector<std::filesystem::path> configs;
    for (const auto& e:std::filesystem::directory_iterator(root)) if (e.is_regular_file()&&Extension(e.path(),L".zproject")) configs.push_back(e.path());
    Require(configs.size()<=1,"Multiple project configs found; open a specific .zproject file.");
    if (!configs.empty()) return Open(configs.front());
    const auto name=root.filename().wstring(); ValidateName(name);
    Project p{root/(name+L".zproject"),Config{Utf8(name),{},{}},{}};
    std::filesystem::create_directories(Assets(p));
    for (const auto& e:std::filesystem::recursive_directory_iterator(Assets(p)))
        if (e.is_regular_file()&&Extension(e.path(),L".zscene"))
        {
            const auto ref=Utf8(std::filesystem::relative(e.path(),root).generic_wstring()); ScenePath(p,ref); p.config.scenes.push_back(ref);
        }
    std::sort(p.config.scenes.begin(),p.config.scenes.end()); p.source=Encode(p.config); Write(p.file,p.source,nullptr); return p;
}
void TrackScene(Project& project,const std::filesystem::path& scene)
{
    const auto file=std::filesystem::weakly_canonical(scene); Contained(Assets(project),file); Require(Extension(file,L".zscene"),"Expected a scene asset.");
    auto ref=Utf8(std::filesystem::relative(file,project.file.parent_path()).generic_wstring()); Reference(ref);
    const auto it=std::find_if(project.config.scenes.begin(),project.config.scenes.end(),[&](const auto& s) { return _wcsicmp(Wide(s).c_str(),Wide(ref).c_str())==0; });
    if (it==project.config.scenes.end()) project.config.scenes.push_back(ref); else ref=*it;
    project.config.lastScene=ref;
}
void Save(Project& project)
{
    const auto text=Encode(project.config); if (text==project.source) return;
    Write(project.file,text,&project.source); project.source=text;
}
std::filesystem::path DefaultSessionFile()
{
    PWSTR location=nullptr; const auto hr=SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&location);
    if (FAILED(hr)) throw std::runtime_error("Cannot locate local editor settings.");
    const auto file=std::filesystem::path(location)/L"zEngine"/L"editor.state"; CoTaskMemFree(location); return file;
}
std::optional<std::filesystem::path> ReadRecent(const std::filesystem::path& sessionFile)
{
    if (!std::filesystem::is_regular_file(sessionFile)) return {};
    std::istringstream in(Read(sessionFile)); Token(in,"ZENGINE_EDITOR"); Token(in,"1"); Token(in,"project");
    const auto path=std::filesystem::path(Wide(Quoted(in))); in>>std::ws; Require(in.eof()&&path.is_absolute(),"Invalid recent project path."); return path;
}
void WriteRecent(const std::filesystem::path& sessionFile,const std::filesystem::path& projectFile)
{
    std::filesystem::create_directories(sessionFile.parent_path());
    std::ostringstream out; out<<"ZENGINE_EDITOR 1\nproject "<<std::quoted(Utf8(std::filesystem::absolute(projectFile).wstring()))<<'\n';
    std::optional<std::string> prior;
    // Settings are replaceable preferences; allow recovery from malformed UTF-8/NUL bytes too.
    if (std::filesystem::exists(sessionFile)) prior=Read(sessionFile,false);
    Write(sessionFile,out.str(),prior?&*prior:nullptr);
}
}
