#include "GameSession.h"
#include "GamePackage.h"
#include "Renderer.h"
#include "RenderTransform.h"
#include "ui/UiSystem.h"
#include "ui/VideoClip.h"
#include "UiAssetBinding.h"
#include "AssetLibrary.h"
#include "MaterialAssets.h"
#include "ShaderAssets.h"
#include "FbxImporter.h"
#include "core/MeshRenderer.h"
#include "core/Light.h"
#include "core/Environment.h"
#include "SceneLights.h"
#include "input/InputAssets.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cmath>
#include <string_view>

namespace {
struct WindowState {unsigned width=1280,height=720;bool closed=false;float wheel=0;std::vector<char32_t> typed;};
LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM w,LPARAM l) {
    auto* state=reinterpret_cast<WindowState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){state=static_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));}
    if(message==WM_SIZE && state){state->width=LOWORD(l);state->height=HIWORD(l);return 0;}
    if(message==WM_MOUSEWHEEL && state){state->wheel+=GET_WHEEL_DELTA_WPARAM(w)/120.0f;return 0;}
    if(message==WM_CHAR && state){ if(state->typed.size()<256) state->typed.push_back(static_cast<char32_t>(w)); return 0; } // ZE-96: UI text/keys
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
            const auto loadMeshes=[&] {
                for(std::size_t i=0;i<session.Objects().Size();++i) {
                    const auto& object=session.Objects().At(i);if(meshes.contains(object.Id()))continue;const auto* mesh=object.GetBehavior<zengine::MeshRenderer>();if(!mesh||mesh->Asset().empty())continue;
                    if(mesh->Asset()==zengine::MeshRenderer::CubeAsset)meshes[object.Id()]=renderer.Cube();
                    else {const auto path=session.Models().at(object.Id());if(!cache.contains(path)){std::vector<std::string> warnings;cache[path]=renderer.UploadModel(FbxImporter::Load(path,true),warnings);}meshes[object.Id()]=cache.at(path);}
                }
            };
            loadMeshes();
            const auto visible=[&](zengine::GameObjectId id){const auto* object=session.Objects().Find(id);const auto* mesh=object?object->GetBehavior<zengine::MeshRenderer>():nullptr;return mesh&&mesh->Enabled()&&meshes.contains(id);};
            auto previous=std::chrono::steady_clock::now(),fpsSample=previous;double accumulated=0;unsigned rendered=0,fpsFrames=0,currentFps=0;
            bool mousePrev[3]={};
            unsigned sceneGeneration=session.SceneGeneration();
            const auto assetsRoot=zengine::projects::Assets(project);
            zengine::ui::UiSystem ui;
            zengine::ui::UiContext uiContext;
            uiContext.measureText=[&](std::string_view s,float h){return renderer.MeasureText(s,h);};
            uiContext.referenceResolution={settings.uiReferenceWidth,settings.uiReferenceHeight};
            uiContext.scaleMode=static_cast<zengine::ui::ScaleMode>(settings.uiScaleMode);
            zengine::ui::UiAssetBinding uiAssets(renderer,assetsRoot);
            uiAssets.Bind(uiContext);
            std::map<std::string,MaterialHandle> materialCache;
            const auto resolveMeshMaterial=[&](const std::string& asset)->MaterialHandle{
                if(asset.empty())return {};
                if(const auto it=materialCache.find(asset);it!=materialCache.end())return it->second;
                MaterialHandle handle;
                try{
                    const auto doc=zengine::materials::Load(assetLibrary::Resolve(assetsRoot,std::filesystem::u8path(asset)));
                    const auto effective=zengine::materials::Resolve(doc,[&](const std::string& shaderPath){
                        return zengine::shaders::Load(assetLibrary::Resolve(assetsRoot,std::filesystem::u8path(shaderPath)));
                    });
                    const auto tint=effective.Numbers("tint",{{1,1,1,1}});
                    TextureHandle albedo;
                    if(const auto texture=effective.Texture("albedo");!texture.empty())
                        try{albedo=renderer.UploadImage(assetLibrary::Resolve(assetsRoot,std::filesystem::u8path(texture)));}catch(...){}
                    handle=renderer.UploadMaterial(albedo,Float4{tint[0],tint[1],tint[2],tint[3]},effective.lit,effective.Numbers("roughness",{{0.5f,0,0,0}})[0],effective.Numbers("specular",{{0,0,0,0}})[0]);
                }catch(...){handle={};}
                materialCache[asset]=handle;
                return handle;
            };
            while(!state.closed && (!automated||rendered<frames)) {
                MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message==WM_QUIT)state.closed=true;TranslateMessage(&message);DispatchMessageW(&message);}
                if(state.closed)break;
                const auto now=std::chrono::steady_clock::now();const auto elapsed=std::min(.1,std::chrono::duration<double>(now-previous).count());previous=now;
                if(!state.width||!state.height||IsIconic(window)){Sleep(10);continue;}
                if(width!=state.width||height!=state.height){renderer.Resize(state.width,state.height);width=state.width;height=state.height;}
                zengine::script::MouseFrame mouse;
                // ZE-96: run the UI first so it can consume the pointer / keyboard before
                // the scripts tick, and route its signals.
                ui.Build(session.Objects(),{static_cast<float>(width),static_cast<float>(height)},uiContext,static_cast<float>(elapsed));
                bool uiTookPointer=false, uiTookKeyboard=false;
                if(automated)session.Tick(1.0f/60.0f,{});
                else {
                    const bool focused=GetForegroundWindow()==window;
                    if(state.width && state.height) {
                        POINT cursor{}; GetCursorPos(&cursor); ScreenToClient(window,&cursor);
                        mouse.inside = cursor.x>=0 && cursor.y>=0 && cursor.x<static_cast<LONG>(state.width) && cursor.y<static_cast<LONG>(state.height);
                        mouse.x = std::clamp(cursor.x/static_cast<double>(state.width)*2.0-1.0,-1.0,1.0);
                        mouse.y = std::clamp(1.0-cursor.y/static_cast<double>(state.height)*2.0,-1.0,1.0);
                    }
                    const int vks[3]={VK_LBUTTON,VK_RBUTTON,VK_MBUTTON};
                    for(int i=0;i<3;++i) {
                        const bool down=focused && (GetKeyState(vks[i])&0x8000)!=0;
                        mouse.buttons[static_cast<std::size_t>(i)]={down,down&&!mousePrev[i],!down&&mousePrev[i]};
                        mousePrev[i]=down;
                    }
                    const float px=static_cast<float>((mouse.x+1.0)*0.5*width),py=static_cast<float>((1.0-mouse.y)*0.5*height);
                    const float wheel=state.wheel;state.wheel=0;
                    const bool shift=focused && (GetKeyState(VK_SHIFT)&0x8000)!=0;
                    const auto uiClicks=ui.Interact(zengine::Vec2{px,py},mouse.buttons[0].pressed,state.typed,wheel,shift);
                    state.typed.clear();
                    uiTookPointer=ui.TookPointer(); uiTookKeyboard=ui.TookKeyboard();
                    for(const auto id:ui.Presses())session.UiPressed(id);
                    for(const auto id:ui.Releases())session.UiReleased(id);
                    for(const auto id:ui.Entered())session.UiMouseEntered(id);
                    for(const auto id:ui.Exited())session.UiMouseExited(id);
                    for(const auto id:ui.Submissions())session.UiSubmitted(id);
                    if(ui.FocusEntered())session.UiFocusEntered(ui.FocusEntered());
                    if(ui.FocusExited())session.UiFocusExited(ui.FocusExited());
                    for(const auto clickedId:uiClicks)session.UiClicked(clickedId);
                    // The game does not see input the UI consumed this frame.
                    auto gameMouse=mouse; if(uiTookPointer) for(auto& b:gameMouse.buttons) b={};
                    const auto hardware=uiTookKeyboard?zengine::input::Hardware{}:zengine::input::PollWindows(focused);
                    accumulated+=elapsed;while(accumulated>=1.0/60.0){session.Tick(1.0f/60.0f,hardware,gameMouse);accumulated-=1.0/60.0;}
                }
                if(session.SceneGeneration()!=sceneGeneration){sceneGeneration=session.SceneGeneration();meshes.clear();materialCache.clear();uiAssets.Invalidate();}
                loadMeshes();
                session.Draw(visible);ViewportFrame frame;frame.camera=settings.camera;++fpsFrames;const auto fpsElapsed=std::chrono::duration<double>(now-fpsSample).count();if(fpsElapsed>=.5){currentFps=static_cast<unsigned>(std::lround(fpsFrames/fpsElapsed));fpsFrames=0;fpsSample=now;}if(settings.showFps)frame.fps=automated?60:currentFps;
                for(std::size_t i=0;i<session.Objects().Size();++i){const auto& object=session.Objects().At(i);if(object.Is2D())continue;
                    if(const auto* env=object.GetBehavior<zengine::Environment>();env&&env->Enabled()&&!frame.environment)frame.environment=MakeEnvironment(*env);
                    if(const auto* light=object.GetBehavior<zengine::Light>();light&&light->Enabled()&&frame.lights.size()<8){DirectX::XMFLOAT4X4 lp;DirectX::XMStoreFloat4x4(&lp,ParentMatrix(session.Objects(),object));frame.lights.push_back(MakeLight(*light,zengine::As3D(object).GetTransform(),DirectX::XMLoadFloat4x4(&lp)));}
                    if(!visible(object.Id()))continue;const auto& t3d=zengine::As3D(object).GetTransform();DirectX::XMFLOAT4X4 parent;DirectX::XMStoreFloat4x4(&parent,ParentMatrix(session.Objects(),object));const auto* mr=object.GetBehavior<zengine::MeshRenderer>();frame.meshes.push_back({meshes.at(object.Id()),t3d,parent,mr?resolveMeshMaterial(mr->Material()):MaterialHandle{}});}
                ui.Build(session.Objects(),{static_cast<float>(width),static_cast<float>(height)},uiContext);
                ui.Emit(frame.sprites,frame.texts);
                renderer.Render(frame);++rendered;
            }
            if(!report.empty()) {
                std::ofstream out(report,std::ios::binary);out<<"ZENGINE_PLAYER_REPORT 1\nscene "<<std::quoted(session.Scene())<<"\nframes "<<rendered<<"\nmeshes "<<renderer.LastMeshCount()<<'\n';
                for(std::size_t i=0;i<session.Objects().Size();++i){const auto& object=session.Objects().At(i);if(object.Is2D())continue;const auto p=zengine::As3D(object).GetTransform().Position(),r=zengine::As3D(object).GetTransform().Rotation();out<<"object "<<object.Id()<<' '<<std::quoted(object.Name())<<" parent "<<object.Parent()<<" position "<<p.x<<' '<<p.y<<' '<<p.z<<" rotation "<<r.x<<' '<<r.y<<' '<<r.z<<'\n';}
                out.close();if(!out)throw std::runtime_error("Could not write player test report.");
            }
            DestroyWindow(window);window=nullptr;
        }
        CoUninitialize();return 0;
    }catch(const std::exception& error){if(window)DestroyWindow(window);if(apartment)CoUninitialize();OutputDebugStringA(error.what());std::cerr<<error.what()<<'\n';if(!automated)MessageBoxA(nullptr,error.what(),"Game could not start",MB_OK|MB_ICONERROR);return 1;}
}
