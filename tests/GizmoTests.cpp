#include "TransformGizmo.h"
#include "EditorShell.h"
#include "InspectorPanel.h"
#include "WindowCapture.h"
#include <objbase.h>
#include <windowsx.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
    void Check(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
    float Component(zengine::Vec3 v,int axis) { return axis==0?v.x:axis==1?v.y:v.z; }
    gizmo::Point Midpoint(const ViewportCamera& camera,const gizmo::Segment& line)
    {
        const auto a=gizmo::Project(camera,line.a),b=gizmo::Project(camera,line.b);
        Check(a && b,"Test segment not visible"); return {(a->x+b->x)/2,(a->y+b->y)/2};
    }
    gizmo::Hit Find(const ViewportCamera& camera,const gizmo::Shape& shape,int axis,gizmo::Point& point)
    {
        for (std::size_t i=0;i<shape.lines.size();++i) if (shape.lines[i].axis==axis)
        {
            point=Midpoint(camera,shape.lines[i]); point.x=std::round(point.x); point.y=std::round(point.y);
            const auto hit=gizmo::Pick(camera,shape,point);
            if (hit && hit->axis==axis) return *hit;
        }
        throw std::runtime_error("Cannot pick requested axis");
    }
    gizmo::Point Destination(const ViewportCamera& camera,const gizmo::Shape& shape,gizmo::Hit hit,gizmo::Point start,gizmo::Mode mode)
    {
        if (mode==gizmo::Mode::Rotate)
            return Midpoint(camera,shape.lines[static_cast<std::size_t>(hit.axis)*96+(hit.segment%96+12)%96]);
        const auto a=gizmo::Project(camera,shape.center),b=gizmo::Project(camera,shape.lines[hit.segment].b);
        Check(a && b,"Test axis not visible");
        return {start.x+(b->x-a->x)*.3f,start.y+(b->y-a->y)*.3f};
    }
    void MathTests()
    {
        for (const auto size:{gizmo::Point{800,600},gizmo::Point{220,600},gizmo::Point{1100,280}})
        for (const auto mode:{gizmo::Mode::Move,gizmo::Mode::Rotate,gizmo::Mode::Scale})
        for (int axis=0;axis<3;++axis)
        {
            const ViewportCamera camera(size.x,size.y); zengine::Transform original;
            const auto shape=gizmo::Build(camera,original,mode); gizmo::Point start;
            const auto hit=Find(camera,shape,axis,start);
            gizmo::Drag drag(camera,original,mode,shape,hit,start);
            const auto next=drag.Update(Destination(camera,shape,hit,start,mode));
            const auto value=mode==gizmo::Mode::Move?next.Position():mode==gizmo::Mode::Rotate?next.Rotation():next.Scale();
            const float base=mode==gizmo::Mode::Scale?1.f:0.f;
            Check(std::abs(Component(value,axis)-base)>.01f,"Axis drag did not change transform");
            for (int other=0;other<3;++other) if (other!=axis) Check(Component(value,other)==base,"Drag changed wrong axis");
            if (mode==gizmo::Mode::Rotate) Check(std::abs(Component(value,axis)-45)<3,"Rotation ring did not track pointer angle");
            const auto restored=drag.Update(start);
            const auto reset=mode==gizmo::Mode::Move?restored.Position():mode==gizmo::Mode::Rotate?restored.Rotation():restored.Scale();
            Check(std::abs(Component(reset,axis)-base)<.001f,"Drag back to start did not restore value");
        }
        const ViewportCamera camera(800,600); zengine::Transform tiny;
        tiny.SetRotation({15,30,45}); tiny.SetScale({0,-2,.00001f});
        const auto shape=gizmo::Build(camera,tiny,gizmo::Mode::Scale);
        for (int axis=0;axis<3;++axis)
        {
            gizmo::Point start; const auto hit=Find(camera,shape,axis,start);
            gizmo::Drag drag(camera,tiny,gizmo::Mode::Scale,shape,hit,start);
            const float value=Component(drag.Update(Destination(camera,shape,hit,start,gizmo::Mode::Scale)).Scale(),axis);
            Check(std::isfinite(value) && std::abs(value-Component(tiny.Scale(),axis))>.01f,"Zero/negative scale could not be edited");
        }
        zengine::Transform edge;
        edge.SetRotation({0,-std::atan(.55f)*180/DirectX::XM_PI,0});
        const auto edgeShape=gizmo::Build(camera,edge,gizmo::Mode::Rotate); gizmo::Point edgeStart;
        const auto edgeHit=Find(camera,edgeShape,0,edgeStart);
        const auto ea=gizmo::Project(camera,edgeShape.lines[edgeHit.segment].a),eb=gizmo::Project(camera,edgeShape.lines[edgeHit.segment].b);
        const float dx=eb->x-ea->x,dy=eb->y-ea->y,length=std::hypot(dx,dy);
        gizmo::Drag edgeDrag(camera,edge,gizmo::Mode::Rotate,edgeShape,edgeHit,edgeStart);
        const auto rotated=edgeDrag.Update({edgeStart.x+dx/length*40,edgeStart.y+dy/length*40});
        Check(std::isfinite(rotated.Rotation().x) && std::abs(rotated.Rotation().x)>1,"Edge-on rotation fallback failed");
        Check(!gizmo::Pick(camera,shape,{-500,-500}),"Background incorrectly picked");
        tiny.SetPosition({0,0,-500}); Check(gizmo::Build(camera,tiny,gizmo::Mode::Move).lines.empty(),"Behind-camera gizmo should be hidden");
    }
}

void GizmoTests(bool capture)
{
    MathTests();
    Check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM failed");
    const auto directory=std::filesystem::temp_directory_path()/(L"zEngine-gizmo-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    Check(std::filesystem::create_directory(directory),"Cannot reserve test directory");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path,error); } } cleanup{directory};
    {
        EditorShell editor(GetModuleHandleW(nullptr)); const auto window=editor.Create(SW_HIDE,directory); editor.InitializeRenderer();
        const auto viewport=FindWindowExW(window,nullptr,L"zEngineViewportWindow",nullptr);
        RECT area{}; GetClientRect(viewport,&area); const ViewportCamera camera(static_cast<float>(area.right),static_cast<float>(area.bottom));
        const auto scene=editor.AssetsDirectory()/L"Gizmos.zscene"; Check(editor.SaveScene(scene),"Cannot save fixture scene");
        const auto send=[&](UINT message,gizmo::Point p,WPARAM w=0) { SendMessageW(viewport,message,w,MAKELPARAM(static_cast<short>(std::lround(p.x)),static_cast<short>(std::lround(p.y)))); };
        for (int mode=0;mode<3;++mode)
        for (int axis=0;axis<3;++axis)
        {
            Check(editor.OpenScene(scene),"Cannot reset scene");
            SendMessageW(window,WM_COMMAND,EditorShell::MoveToolCommand+mode,0);
            const auto original=editor.SelectedGameObject()->GetTransform();
            const auto shape=gizmo::Build(camera,original,editor.TransformTool()); gizmo::Point start;
            const auto hit=Find(camera,shape,axis,start);
            send(WM_LBUTTONDOWN,start); Check(GetCapture()==viewport,"Viewport did not capture handle drag");
            const auto end=Destination(camera,shape,hit,start,editor.TransformTool()); send(WM_MOUSEMOVE,end,MK_LBUTTON);
            Check(editor.SceneDirty(),"Gizmo did not mark scene dirty");
            send(WM_LBUTTONUP,end); Check(GetCapture()!=viewport,"Drag did not release capture");
            const auto transform=editor.SelectedGameObject()->GetTransform();
            const auto v=mode==0?transform.Position():mode==1?transform.Rotation():transform.Scale();
            Check(std::abs(Component(v,axis)-(mode==2?1.f:0.f))>.01f,"Native input did not change selected axis");
            const auto inspector=FindWindowExW(window,nullptr,L"zEngineInspector",nullptr);
            wchar_t field[80]{}; GetDlgItemTextW(inspector,InspectorPanel::FirstTransformField+mode*3+axis,field,80);
            Check(std::abs(static_cast<float>(std::wcstod(field,nullptr))-Component(v,axis))<.002f,"Inspector did not follow gizmo");
            editor.Render();
            if (capture && axis==0) CaptureWindow(window,L"gizmo-mode"+std::to_wstring(mode)+L".bmp");
            Check(editor.SaveScene(),"Edited transform could not be saved");
            // Restore a clean starting point for the next axis without prompting.
            auto& object=const_cast<zengine::GameObject&>(zengine::As3D(editor.GameObjects().At(0))); object.GetTransform()=zengine::Transform{}; Check(editor.SaveScene(),"Fixture reset failed");
        }
        editor.SetTransformTool(gizmo::Mode::Move);
        gizmo::Point start; auto shape=gizmo::Build(camera,editor.SelectedGameObject()->GetTransform(),gizmo::Mode::Move);
        auto hit=Find(camera,shape,0,start); const auto end=Destination(camera,shape,hit,start,gizmo::Mode::Move);
        send(WM_LBUTTONDOWN,start); send(WM_MOUSEMOVE,end,MK_LBUTTON); SendMessageW(viewport,WM_KEYDOWN,VK_ESCAPE,0);
        Check(!editor.SceneDirty() && editor.SelectedGameObject()->GetTransform().Position().x==0 && IsWindow(window),"Escape did not cancel cleanly");
        send(WM_LBUTTONDOWN,start); send(WM_MOUSEMOVE,end,MK_LBUTTON); ReleaseCapture();
        Check(!editor.SceneDirty() && editor.SelectedGameObject()->GetTransform().Position().x==0,"Capture loss did not cancel");
        editor.CreateEmptyGameObject(); // Preserve prior scene dirtiness when a subsequent drag is canceled.
        shape=gizmo::Build(camera,editor.SelectedGameObject()->GetTransform(),gizmo::Mode::Move); hit=Find(camera,shape,0,start);
        send(WM_LBUTTONDOWN,start); send(WM_MOUSEMOVE,{start.x+25,start.y+10},MK_LBUTTON);
        editor.SetTransformTool(gizmo::Mode::Rotate);
        Check(editor.SceneDirty() && editor.SelectedGameObject()->GetTransform().Position().x==0 && GetCapture()!=viewport,"Tool change did not cancel while preserving existing edits");
        Check(editor.SaveScene(),"Cannot save empty object");
        SendMessageW(viewport,WM_KEYDOWN,'E',0); Check(editor.TransformTool()==gizmo::Mode::Rotate,"Rotate shortcut failed");
        SendMessageW(viewport,WM_KEYDOWN,'R',0); Check(editor.TransformTool()==gizmo::Mode::Scale,"Scale shortcut failed");
        editor.SetTransformTool(gizmo::Mode::Move); Check(editor.Play(),"Play failed");
        send(WM_LBUTTONDOWN,start); Check(GetCapture()!=viewport,"Authoring gizmos should be disabled during Play"); editor.Stop();
        const auto initial=editor.BuildSceneFrame().camera;
        send(WM_RBUTTONDOWN,{100,100},MK_RBUTTON);send(WM_MOUSEMOVE,{140,120},MK_RBUTTON);send(WM_RBUTTONUP,{140,120});
        const auto orbit=editor.BuildSceneFrame().camera;
        Check(orbit.yaw!=initial.yaw && orbit.pitch!=initial.pitch && GetCapture()!=viewport,"RMB orbit failed");
        send(WM_RBUTTONDOWN,{100,100},MK_RBUTTON|MK_CONTROL);send(WM_MOUSEMOVE,{150,120},MK_RBUTTON|MK_CONTROL);send(WM_RBUTTONUP,{150,120});
        const auto pan=editor.BuildSceneFrame().camera;
        Check(pan.target.x!=orbit.target.x && pan.yaw==orbit.yaw,"Ctrl RMB pan failed");
        SendMessageW(viewport,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),0);
        Check(editor.BuildSceneFrame().camera.distance<pan.distance,"Wheel zoom failed");
        const auto beforeFly=editor.BuildSceneFrame().camera;
        const ViewportCamera beforeView(static_cast<float>(area.right),static_cast<float>(area.bottom),beforeFly);
        send(WM_RBUTTONDOWN,{100,100},MK_RBUTTON|MK_SHIFT);send(WM_MOUSEMOVE,{150,120},MK_RBUTTON|MK_SHIFT);
        const ViewportCamera afterView(static_cast<float>(area.right),static_cast<float>(area.bottom),editor.BuildSceneFrame().camera);
        const auto eyeA=DirectX::XMMatrixInverse(nullptr,beforeView.view).r[3],eyeB=DirectX::XMMatrixInverse(nullptr,afterView.view).r[3];
        Check(DirectX::XMVectorGetX(DirectX::XMVector3Length(DirectX::XMVectorSubtract(eyeA,eyeB)))<.001f,"Fly look changed camera position");
        SendMessageW(viewport,WM_KEYDOWN,VK_ESCAPE,0);Check(GetCapture()!=viewport && !editor.SceneDirty(),"Camera navigation changed scene data or retained capture");

        // ZE-104: click-to-select in the scene view. The default scene has a cube at the origin.
        Check(editor.OpenScene(scene),"Cannot reset scene for pick test");
        const auto cube=editor.SelectedGameObject()->Id();
        const gizmo::Point centre{static_cast<float>(area.right)/2,static_cast<float>(area.bottom)/2};
        send(WM_LBUTTONDOWN,{4,4}); send(WM_LBUTTONUP,{4,4});
        Check(editor.SelectedGameObject()==nullptr,"Clicking empty space did not clear the selection");
        send(WM_LBUTTONDOWN,centre); send(WM_LBUTTONUP,centre);
        Check(editor.SelectedGameObject() && editor.SelectedGameObject()->Id()==cube,"Clicking the cube did not select it");
        Check(!editor.SceneDirty(),"Click-select marked the scene dirty");
    }
    CoUninitialize(); std::cout<<"PASS: all transform axes, rotation angles, aspect ratios, zero/negative scales, native dragging, cancellation, shortcuts, save and Play guards, click-select\n";
}
