#pragma once
#include <filesystem>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <windows.h>

namespace assetLibrary {
enum class Kind { Folder, Script, Prefab, Scene, Input, Image, Model, Shader, File };
inline std::filesystem::path Resolve(const std::filesystem::path& root,const std::filesystem::path& path) {
    const auto base=std::filesystem::weakly_canonical(root),target=std::filesystem::weakly_canonical(path.is_absolute()?path:base/path);
    auto b=base.begin(),p=target.begin();for(;b!=base.end();++b,++p)
        if(p==target.end() || _wcsicmp(b->c_str(),p->c_str()))throw std::runtime_error("Asset must stay inside the project's Assets folder.");
    return target;
}
inline bool Package(const std::filesystem::path& p){return std::filesystem::is_regular_file(p/L"asset.ready") && std::filesystem::is_regular_file(p/L"model.fbx");}
inline Kind Type(const std::filesystem::path& p) {
    if(std::filesystem::is_directory(p))return Kind::Folder;
    auto ext=p.extension().wstring();std::transform(ext.begin(),ext.end(),ext.begin(),::towlower);
    if(ext==L".zsh")return Kind::Script;if(ext==L".zprefab")return Kind::Prefab;if(ext==L".zscene")return Kind::Scene;if(ext==L".zinput")return Kind::Input;
    if(ext==L".shader")return Kind::Shader;
    if(ext==L".fbx")return Kind::Model;
    if(ext==L".png"||ext==L".jpg"||ext==L".jpeg"||ext==L".bmp"||ext==L".tga"||ext==L".gif"||ext==L".dds"||ext==L".tif")return Kind::Image;
    return Kind::File;
}
inline std::filesystem::path Storage(const std::filesystem::path& asset){return Type(asset)==Kind::Model&&Package(asset.parent_path())?asset.parent_path():asset;}
inline std::vector<std::filesystem::path> List(const std::filesystem::path& root,const std::filesystem::path& folder) {
    const auto directory=Resolve(root,folder);if(!std::filesystem::is_directory(directory)||Package(directory))throw std::runtime_error("Choose an asset folder, not a model package.");
    std::vector<std::filesystem::path> result;
    for(const auto& entry:std::filesystem::directory_iterator(directory)) {
        const auto attributes=GetFileAttributesW(entry.path().c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))continue;
        const auto p=Resolve(root,entry.path());
        if(entry.is_directory()) {
            if(std::filesystem::is_regular_file(p/L"model.fbx") && !Package(p))continue;
            result.push_back(Package(p)?p/L"model.fbx":p);
        }
        else if(entry.is_regular_file() && p.filename().wstring().find(L".save-")==std::wstring::npos)result.push_back(p);
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){
        const auto ka=Type(a),kb=Type(b);const int ra=ka==Kind::Folder?0:ka==Kind::Input?2:1,rb=kb==Kind::Folder?0:kb==Kind::Input?2:1;
        const auto an=ka==Kind::Model?a.parent_path().filename():a.filename(),bn=kb==Kind::Model?b.parent_path().filename():b.filename();
        return ra!=rb?ra<rb:_wcsicmp(an.c_str(),bn.c_str())<0;
    });return result;
}
// Small code-drawn icons: no texture files, font glyph dependency, or UI framework.
inline void Icon(HDC dc,Kind kind,int x,int y) {
    const COLORREF colors[]={RGB(217,178,89),RGB(133,199,144),RGB(120,174,236),RGB(182,159,227),RGB(219,155,107),RGB(137,194,209),RGB(122,179,227),RGB(226,140,196),RGB(167,174,186)};
    auto pen=CreatePen(PS_SOLID,1,colors[static_cast<int>(kind)]);auto oldPen=SelectObject(dc,pen),oldBrush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
    if(kind==Kind::Folder){POINT p[]={{x,y+4},{x+6,y+4},{x+8,y+6},{x+17,y+6},{x+17,y+17},{x,y+17},{x,y+4}};Polyline(dc,p,7);}
    else if(kind==Kind::Model || kind==Kind::Prefab){POINT p[]={{x+8,y+1},{x+17,y+6},{x+17,y+15},{x+8,y+20},{x,y+15},{x,y+6},{x+8,y+1}};Polyline(dc,p,7);MoveToEx(dc,x,y+6,nullptr);LineTo(dc,x+8,y+11);LineTo(dc,x+17,y+6);MoveToEx(dc,x+8,y+11,nullptr);LineTo(dc,x+8,y+20);if(kind==Kind::Prefab)Rectangle(dc,x+5,y+6,x+11,y+10);}
    else {Rectangle(dc,x+1,y+1,x+17,y+20);
        if(kind==Kind::Image){Ellipse(dc,x+10,y+4,x+14,y+8);MoveToEx(dc,x+3,y+17,nullptr);LineTo(dc,x+7,y+10);LineTo(dc,x+11,y+15);LineTo(dc,x+15,y+11);}
        else if(kind==Kind::Input){MoveToEx(dc,x+4,y+10,nullptr);LineTo(dc,x+11,y+10);MoveToEx(dc,x+7,y+7,nullptr);LineTo(dc,x+7,y+14);Ellipse(dc,x+12,y+7,x+15,y+10);}
        else if(kind==Kind::Scene){MoveToEx(dc,x+5,y+5,nullptr);LineTo(dc,x+5,y+16);MoveToEx(dc,x+5,y+9,nullptr);LineTo(dc,x+13,y+9);MoveToEx(dc,x+5,y+15,nullptr);LineTo(dc,x+13,y+15);}
        else if(kind==Kind::Shader){Ellipse(dc,x+4,y+4,x+14,y+14);MoveToEx(dc,x+4,y+9,nullptr);LineTo(dc,x+14,y+9);MoveToEx(dc,x+5,y+16,nullptr);LineTo(dc,x+13,y+16);}
        else {for(int row=0;row<3;++row){MoveToEx(dc,x+4,y+6+row*4,nullptr);LineTo(dc,x+14-row*2,y+6+row*4);}}
    }
    SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(pen);
}
}
