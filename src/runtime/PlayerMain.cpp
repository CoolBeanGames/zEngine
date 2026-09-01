#include "GameSession.h"
#include "GamePackage.h"
#include "Renderer.h"
#include "RenderTransform.h"
#include "FbxImporter.h"
#include "core/MeshRenderer.h"
#include "input/InputAssets.h"
#include <windows.h>
#include <shellapi.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cmath>

namespace {
struct WindowState {unsigned width=1280,height=720;bool closed=false;};
LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM w,LPARAM l) {
    auto* state=reinterpret_cast<WindowState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){state=static_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));}
    if(message==WM_SIZE && state){state->width=LOWORD(l);state->height=HIWORD(l);return 0;}
    if(message==WM_CLOSE && state){state->closed=true;return 0;}
    if(message==WM_ERASEBKGND)return 1;
    return DefWindowProcW(window,message,w,l);
}
std::string Read(const std::filesystem::path& file){if(std::filesystem::file_size(file)>4096)throw std::runtime_error("Game settings are too large.");std::ifstream in(file,std::ios::binary);return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};}
std::string Utf8(const std::wstring& s){const auto n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);if(!n && !s.empty())throw std::runtime_error("Invalid Unicode argument.");std::string result(n,' ');WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),result.data(),n,nullptr,nullptr);return result;}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    bool automated=false;HWND window=nullptr;bool apartment=false;
    try {
        unsigned frames=0;std::filesystem::path report;std::string scene;
        int count=0;auto args=CommandLineToArgvW(GetCommandLineW(),&count);
        if(!args)throw std::runtime_error("Cannot read game arguments.");
        std::vector<std::wstring> arguments;for(int i=1;i<count;++i)arguments.emplace_back(args[i]);LocalFree(args);
        for(std::size_t i=0;i<arguments.size();++i) {
            if(arguments[i]==L"--frames" && i+1<arguments.size()){automated=true;std::size_t used=0;auto value=std::stoul(arguments[++i],&used);if(!value||value>3600||used!=arguments[i].size())throw std::runtime_error("--frames requires 1-3600 ticks.");frames=static_cast<unsigned>(value);}
            else if(arguments[i]==L"--report" && i+1<arguments.size())report=arguments[++i];
            else if(arguments[i]==L"--scene" && i+1<arguments.size())scene=Utf8(arguments[++i]);
            else throw std::runtime_error("Unknown player argument.");
        }
        if(!report.empty() && !automated)throw std::runtime_error("--report is only available with --frames.");
        if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))throw std::runtime_error("Could not initialize Windows image services.");apartment=true;
        {
            const auto root=zengine::game::ExecutableDirectory();const auto project=zengine::projects::Open(root/L"Data"/L"Game.zproject");
            const auto settings=zengine::game::DecodeSettings(Read(root/L"Data"/L"Game.settings"));
            zengine::game::Session session(project,scene);
            WNDCLASSW cls{};cls.hInstance=instance;cls.lpfnWndProc=Procedure;cls.lpszClassName=L"zEnginePlayerWindow";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&cls);
            WindowState state;RECT rect{0,0,1280,720};AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
            const auto name=std::filesystem::u8path(project.config.name).wstring();
            window=CreateWindowExW(0,cls.lpszClassName,name.c_str(),WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,instance,&state);
            if(!window)throw std::runtime_error("Could not open game window.");
            Renderer renderer;renderer.Initialize(window,state.width,state.height);unsigned width=state.width,height=state.height;
            std::map<zengine::GameObjectId,MeshHandle> meshes;std::map<std::filesystem::path,MeshHandle> cache;
            for(std::size_t i=0;i<session.Objects().Size();++i) {
                const auto& object=session.Objects().At(i);const auto mesh=object.GetBehavior<zengine::MeshRenderer>();
                if(!mesh||mesh->Asset().empty())continue;
                if(mesh->Asset()==zengine::MeshRenderer::CubeAsset)meshes[object.Id()]=renderer.Cube();
                else {const auto path=session.Models().at(object.Id());if(!cache.contains(path)){std::vector<std::string> warnings;cache[path]=renderer.UploadModel(FbxImporter::Load(path,true),warnings);}meshes[object.Id()]=cache.at(path);}
            }
            session.Start();ShowWindow(window,automated?SW_HIDE:show);
            const auto visible=[&](zengine::GameObjectId id){const auto* object=session.Objects().Find(id);const auto* mesh=object?object->GetBehavior<zengine::MeshRenderer>():nullptr;return mesh&&mesh->Enabled()&&meshes.contains(id);};
            auto previous=std::chrono::steady_clock::now(),fpsSample=previous;double accumulated=0;unsigned rendered=0,fpsFrames=0,currentFps=0;
            while(!state.closed && (!automated||rendered<frames)) {
                MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message==WM_QUIT)state.closed=true;TranslateMessage(&message);DispatchMessageW(&message);}
                if(state.closed)break;
                const auto now=std::chrono::steady_clock::now();const auto elapsed=std::min(.1,std::chrono::duration<double>(now-previous).count());previous=now;
                if(!state.width||!state.height||IsIconic(window)){Sleep(10);continue;}
                if(width!=state.width||height!=state.height){renderer.Resize(state.width,state.height);width=state.width;height=state.height;}
                if(automated)session.Tick(1.0f/60.0f,{});
                else {accumulated+=elapsed;while(accumulated>=1.0/60.0){session.Tick(1.0f/60.0f,zengine::input::PollWindows(GetForegroundWindow()==window));accumulated-=1.0/60.0;}}
                session.Draw(visible);ViewportFrame frame;frame.camera=settings.camera;++fpsFrames;const auto fpsElapsed=std::chrono::duration<double>(now-fpsSample).count();if(fpsElapsed>=.5){currentFps=static_cast<unsigned>(std::lround(fpsFrames/fpsElapsed));fpsFrames=0;fpsSample=now;}if(settings.showFps)frame.fps=automated?60:currentFps;
                for(std::size_t i=0;i<session.Objects().Size();++i){const auto& object=session.Objects().At(i);if(!visible(object.Id()))continue;DirectX::XMFLOAT4X4 parent;DirectX::XMStoreFloat4x4(&parent,ParentMatrix(session.Objects(),object));frame.meshes.push_back({meshes.at(object.Id()),object.GetTransform(),parent});}
                renderer.Render(frame);++rendered;
            }
            if(!report.empty()) {
                std::ofstream out(report,std::ios::binary);out<<"ZENGINE_PLAYER_REPORT 1\nscene "<<std::quoted(session.Scene())<<"\nframes "<<rendered<<"\nmeshes "<<renderer.LastMeshCount()<<'\n';
                for(std::size_t i=0;i<session.Objects().Size();++i){const auto& object=session.Objects().At(i);const auto p=object.GetTransform().Position(),r=object.GetTransform().Rotation();out<<"object "<<object.Id()<<' '<<std::quoted(object.Name())<<" parent "<<object.Parent()<<" position "<<p.x<<' '<<p.y<<' '<<p.z<<" rotation "<<r.x<<' '<<r.y<<' '<<r.z<<'\n';}
                out.close();if(!out)throw std::runtime_error("Could not write player test report.");
            }
            DestroyWindow(window);window=nullptr;
        }
        CoUninitialize();return 0;
    }catch(const std::exception& error){if(window)DestroyWindow(window);if(apartment)CoUninitialize();OutputDebugStringA(error.what());std::cerr<<error.what()<<'\n';if(!automated)MessageBoxA(nullptr,error.what(),"Game could not start",MB_OK|MB_ICONERROR);return 1;}
}
