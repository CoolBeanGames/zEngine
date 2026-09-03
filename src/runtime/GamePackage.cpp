#include "GamePackage.h"
#include "GameSession.h"
#include "FbxImporter.h"
#include "AssetLibrary.h"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <set>
#include <windows.h>

namespace zengine::game {
namespace {
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
bool Within(const std::filesystem::path& root,const std::filesystem::path& path){try{assetLibrary::Resolve(root,path);return true;}catch(...){return false;}}
void Write(const std::filesystem::path& file,const std::string& text){std::ofstream stream(file,std::ios::binary);stream.write(text.data(),static_cast<std::streamsize>(text.size()));stream.close();Require(static_cast<bool>(stream),"Could not write game package.");}
std::wstring Wide(const std::string& text){const auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);Require(n>0,"Invalid game name.");std::wstring result(n,L' ');MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),n);return result;}
}
std::filesystem::path ExecutableDirectory(){std::vector<wchar_t> path(32768);const auto n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));Require(n>0 && n<path.size(),"Cannot locate executable.");return std::filesystem::path(std::wstring(path.data(),n)).parent_path();}
std::string EncodeSettings(const Settings& settings) {
    const auto& c=settings.camera;std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(9)<<"ZENGINE_GAME 3\ncamera "<<c.target.x<<' '<<c.target.y<<' '<<c.target.z<<' '<<c.yaw<<' '<<c.pitch<<' '<<c.distance<<"\nshow_fps "<<(settings.showFps?1:0)<<"\nui_scale "<<settings.uiReferenceWidth<<' '<<settings.uiReferenceHeight<<' '<<settings.uiScaleMode<<"\nend\n";
    const auto text=out.str();DecodeSettings(text);return text;
}
Settings DecodeSettings(const std::string& text) {
    Require(text.size()<4096,"Game settings exceed size limit.");std::istringstream in(text);in.imbue(std::locale::classic());std::string magic,version,token;Settings s;auto& c=s.camera;
    Require(static_cast<bool>(in>>magic>>version>>token) && magic=="ZENGINE_GAME" && (version=="1"||version=="2"||version=="3") && token=="camera","Unsupported game package settings.");
    Require(static_cast<bool>(in>>c.target.x>>c.target.y>>c.target.z>>c.yaw>>c.pitch>>c.distance),"Invalid camera settings.");
    if(version=="2"||version=="3"){int enabled=-1;Require(static_cast<bool>(in>>token>>enabled)&&token=="show_fps"&&(enabled==0||enabled==1),"Invalid FPS setting.");s.showFps=enabled!=0;}
    if(version=="3"){Require(static_cast<bool>(in>>token>>s.uiReferenceWidth>>s.uiReferenceHeight>>s.uiScaleMode)&&token=="ui_scale","Invalid UI scale setting.");Require(std::isfinite(s.uiReferenceWidth)&&std::isfinite(s.uiReferenceHeight)&&s.uiReferenceWidth>=0&&s.uiReferenceHeight>=0&&s.uiReferenceWidth<=16384&&s.uiReferenceHeight<=16384&&s.uiScaleMode>=0&&s.uiScaleMode<=4,"UI scale setting out of range.");}
    Require(static_cast<bool>(in>>token)&&token=="end","Invalid game settings ending.");
    for(float v:{c.target.x,c.target.y,c.target.z,c.yaw,c.pitch,c.distance})Require(std::isfinite(v),"Nonfinite game camera.");
    Require(c.distance>=0 && c.distance<=10000 && std::abs(c.pitch)<=1.55f,"Game camera out of range.");in>>std::ws;Require(in.eof(),"Unexpected game settings data.");return s;
}
std::filesystem::path Export(const projects::Project& project,const std::filesystem::path& startupScene,const Settings& settings,const std::filesystem::path& outputParent,const std::filesystem::path& playerDirectory,const std::function<void(unsigned,const std::string&)>& progress) {
    const auto report=[&](unsigned percent,const char* message){if(progress)progress(percent,message);};
    const auto parent=std::filesystem::canonical(outputParent),source=std::filesystem::canonical(project.file.parent_path());
    Require(std::filesystem::is_directory(parent),"Choose an existing build destination folder.");
    Require(!Within(source,parent),"Build outside the source project so exported files cannot become project assets.");
    const auto player=std::filesystem::canonical(playerDirectory);
    Require(std::filesystem::is_regular_file(player/L"zPlayer.exe") && std::filesystem::is_regular_file(player/L"shaders"/L"ColorCube.hlsl") && std::filesystem::is_regular_file(player/L"shaders"/L"ShadowDepth.hlsl") && std::filesystem::is_regular_file(player/L"shaders"/L"Sprite.hlsl"),"Standalone player is missing. Build the zPlayer target first.");
    Require(projects::Open(project.file).source==project.source,"Project config changed on disk. Reopen it before building.");
    auto config=project.config;const auto startup=std::filesystem::relative(assetLibrary::Resolve(projects::Assets(project),startupScene),source).generic_u8string();
    config.lastScene.assign(startup.begin(),startup.end());projects::Decode(projects::Encode(config));
    Require(!config.lastScene.empty(),"Choose a startup scene.");const auto settingsText=EncodeSettings(settings);
    report(0,"Validating scenes and scripts");
    std::set<std::filesystem::path> checkedModels;
    for(std::size_t i=0;i<config.scenes.size();++i) {
        Session session(project,config.scenes[i]);
        for(const auto& [id,file]:session.Models())if(checkedModels.insert(file).second)FbxImporter::Load(file,true);
        report(static_cast<unsigned>(20*(i+1)/config.scenes.size()),"Validated scene");
    }
    const auto assets=projects::Assets(project);std::vector<std::filesystem::path> files;std::uintmax_t total=0;
    for(std::filesystem::recursive_directory_iterator it(assets),end;it!=end;++it) {
        const auto attributes=GetFileAttributesW(it->path().c_str());
        Require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Build assets cannot contain symbolic links or junctions.");
        const auto file=assetLibrary::Resolve(assets,it->path());
        if(it->is_regular_file()) {
            if(file.filename().wstring().find(L".save-")!=std::wstring::npos)continue;
            const auto size=it->file_size();Require(size<=2ull*1024*1024*1024 && total<=8ull*1024*1024*1024-size,"Game assets exceed the 8 GiB package limit.");total+=size;files.push_back(file);
            Require(files.size()<=100000,"Too many files in project assets.");
        }
    }
    const auto name=Wide(config.name);projects::ValidateName(name);
    std::filesystem::path output;for(unsigned i=0;i<10000;++i){output=parent/(name+(i?L" ("+std::to_wstring(i)+L")":L""));if(!std::filesystem::exists(output))break;output.clear();}
    Require(!output.empty(),"Too many builds with this name.");
    Require(!Within(output,source),"Build output cannot contain the source project.");
    std::filesystem::path staging;
    for(unsigned i=0;i<10000;++i){auto candidate=parent/(L".zengine-build-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(i));if(std::filesystem::create_directory(candidate)){staging=candidate;break;}}
    Require(!staging.empty(),"Cannot reserve a temporary build directory.");
    try {
        std::filesystem::create_directories(staging/L"Data"/L"Assets");
        for(std::size_t i=0;i<files.size();++i){const auto target=staging/L"Data"/L"Assets"/std::filesystem::relative(files[i],assets);std::filesystem::create_directories(target.parent_path());std::filesystem::copy_file(files[i],target);report(20+static_cast<unsigned>(60*(i+1)/files.size()),"Copying project assets");}
        Write(staging/L"Data"/L"Game.zproject",projects::Encode(config));Write(staging/L"Data"/L"Game.settings",settingsText);
        std::filesystem::copy_file(player/L"zPlayer.exe",staging/(name+L".exe"));
        std::filesystem::create_directory(staging/L"shaders");
        for(const auto* shader:{L"ColorCube.hlsl",L"ShadowDepth.hlsl",L"Sprite.hlsl"})std::filesystem::copy_file(player/L"shaders"/shader,staging/L"shaders"/shader);
        Require(std::filesystem::is_regular_file(player/L"ufbx-LICENSE.txt"),"Player third-party license is missing.");
        std::filesystem::copy_file(player/L"ufbx-LICENSE.txt",staging/L"ufbx-LICENSE.txt");
        Require(std::filesystem::is_regular_file(player/L"Jolt-LICENSE.txt"),"Player Jolt license is missing.");std::filesystem::copy_file(player/L"Jolt-LICENSE.txt",staging/L"Jolt-LICENSE.txt");
        Write(staging/L"README.txt","Run the .exe next to this file. Keep Data and shaders beside it.\r\nWindows 10/11 x64 and Direct3D 11 required. No editor or Visual Studio required.\r\nStartup scene: "+config.lastScene+"\r\nThe build camera is the editor viewport at export time.\r\nScript source is included; this package is not encrypted.\r\n");
        report(85,"Validating packaged data");
        const auto packaged=projects::Open(staging/L"Data"/L"Game.zproject");
        for(const auto& scene:config.scenes){Session check(packaged,scene);for(const auto& [id,file]:check.Models())FbxImporter::Load(file,true);}
        std::filesystem::rename(staging,output);report(100,"Build complete");return output/(name+L".exe");
    }catch(...){std::error_code error;if(Within(parent,staging) && staging.parent_path()==parent)std::filesystem::remove_all(staging,error);throw;}
}
}
