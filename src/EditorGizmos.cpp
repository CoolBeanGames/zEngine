#include "EditorShell.h"
#include "InspectorPanel.h"
#include "RenderTransform.h"
#include "physics/PhysicsBehavior.h"
#include <windowsx.h>
#include <cfloat>

zengine::GameObjectId EditorShell::PickObject(gizmo::Point p) const
{
    using namespace DirectX;
    if (requestedViewportWidth_ == 0 || requestedViewportHeight_ == 0) return 0;
    ViewportCamera camera(static_cast<float>(requestedViewportWidth_), static_cast<float>(requestedViewportHeight_), sceneCamera_);
    const XMMATRIX identity = XMMatrixIdentity();
    const XMVECTOR rayNear = XMVector3Unproject(XMVectorSet(p.x, p.y, 0.0f, 1.0f), 0, 0, camera.width, camera.height, 0.0f, 1.0f, camera.projection, camera.view, identity);
    const XMVECTOR rayFar  = XMVector3Unproject(XMVectorSet(p.x, p.y, 1.0f, 1.0f), 0, 0, camera.width, camera.height, 0.0f, 1.0f, camera.projection, camera.view, identity);

    float best = FLT_MAX;
    zengine::GameObjectId hit = 0;
    for (std::size_t i = 0; i < objects_.Size(); ++i)
    {
        const auto* object = zengine::As3D(&objects_.At(i));
        if (!object) continue;
        const XMMATRIX world = TransformMatrix(object->GetTransform()) * ParentMatrix(objects_, *object);
        XMVECTOR det;
        const XMMATRIX inverse = XMMatrixInverse(&det, world);
        if (std::abs(XMVectorGetX(det)) < 1e-12f) continue;
        const XMVECTOR lo = XMVector3TransformCoord(rayNear, inverse);
        const XMVECTOR dir = XMVector3Normalize(XMVector3TransformCoord(rayFar, inverse) - lo);

        Float3 bmin{-0.35f,-0.35f,-0.35f}, bmax{0.35f,0.35f,0.35f}; // grab box for empties / lights / cameras
        if (const auto b = meshBindings_.find(object->Id()); b != meshBindings_.end())
        { bmin = b->second.boundsMin; bmax = b->second.boundsMax; }
        else if (const auto* collider = object->GetBehavior<zengine::physics::Collider>())
        {
            const auto off = collider->Offset(), sz = collider->Size();
            bmin = {off.x - sz.x * 0.5f, off.y - sz.y * 0.5f, off.z - sz.z * 0.5f};
            bmax = {off.x + sz.x * 0.5f, off.y + sz.y * 0.5f, off.z + sz.z * 0.5f};
        }

        // Slab test in the object's local space.
        float tmin = -FLT_MAX, tmax = FLT_MAX;
        const float o[3]{XMVectorGetX(lo), XMVectorGetY(lo), XMVectorGetZ(lo)};
        const float d[3]{XMVectorGetX(dir), XMVectorGetY(dir), XMVectorGetZ(dir)};
        const float lohi[3][2]{{bmin.x,bmax.x},{bmin.y,bmax.y},{bmin.z,bmax.z}};
        bool miss = false;
        for (int a = 0; a < 3 && !miss; ++a)
        {
            if (std::abs(d[a]) < 1e-8f) { if (o[a] < lohi[a][0] || o[a] > lohi[a][1]) miss = true; continue; }
            float t1 = (lohi[a][0] - o[a]) / d[a], t2 = (lohi[a][1] - o[a]) / d[a];
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1); tmax = std::min(tmax, t2);
            if (tmin > tmax) miss = true;
        }
        if (miss || tmax < 0.0f) continue;
        const XMVECTOR worldHit = XMVector3TransformCoord(lo + dir * std::max(tmin, 0.0f), world);
        const float distance = XMVectorGetX(XMVector3Length(worldHit - rayNear));
        if (distance < best) { best = distance; hit = object->Id(); }
    }
    return hit;
}

void EditorShell::EndCameraDrag(){cameraDrag_=CameraDrag::None;if(GetCapture()==viewportWindow_)ReleaseCapture();}
void EditorShell::CameraMotion(POINT p)
{
    using namespace DirectX;
    ViewportCamera camera(static_cast<float>(requestedViewportWidth_),static_cast<float>(requestedViewportHeight_),sceneCamera_);
    const auto inverse=XMMatrixInverse(nullptr,camera.view);
    if(sceneCamera_.distance<=0)sceneCamera_.distance=XMVectorGetX(XMVector3Length(inverse.r[3]-XMLoadFloat3(&sceneCamera_.target)));
    const float dx=static_cast<float>(p.x-cameraPoint_.x),dy=static_cast<float>(p.y-cameraPoint_.y);cameraPoint_=p;
    if(cameraDrag_==CameraDrag::Pan){auto target=XMLoadFloat3(&sceneCamera_.target)+(-inverse.r[0]*dx+inverse.r[1]*dy)*sceneCamera_.distance*0.0025f;XMStoreFloat3(&sceneCamera_.target,target);}
    else {
        sceneCamera_.yaw=std::remainder(sceneCamera_.yaw-dx*0.006f,XM_2PI);sceneCamera_.pitch=std::clamp(sceneCamera_.pitch+dy*0.006f,-1.55f,1.55f);
        if(cameraDrag_==CameraDrag::Fly){const auto offset=XMVectorSet(std::sin(sceneCamera_.yaw)*std::cos(sceneCamera_.pitch),std::sin(sceneCamera_.pitch),-std::cos(sceneCamera_.yaw)*std::cos(sceneCamera_.pitch),0);XMStoreFloat3(&sceneCamera_.target,inverse.r[3]-offset*sceneCamera_.distance);}
    }
}
void EditorShell::CameraTick(float delta)
{
    if(cameraDrag_!=CameraDrag::Fly || GetFocus()!=viewportWindow_)return;
    using namespace DirectX;ViewportCamera camera(static_cast<float>(requestedViewportWidth_),static_cast<float>(requestedViewportHeight_),sceneCamera_);const auto inverse=XMMatrixInverse(nullptr,camera.view);
    auto key=[](int k){return (GetAsyncKeyState(k)&0x8000)?1.0f:0.0f;};
    auto direction=inverse.r[2]*(key('W')-key('S'))+inverse.r[0]*(key('D')-key('A'))+XMVectorSet(0,key('E')-key('Q'),0,0);
    if(XMVectorGetX(XMVector3LengthSq(direction))>0)XMStoreFloat3(&sceneCamera_.target,XMLoadFloat3(&sceneCamera_.target)+XMVector3Normalize(direction)*std::clamp(sceneCamera_.distance,1.0f,100.0f)*delta);
}

RECT EditorShell::ToolRectangle(int index) const
{
    return {345+index*68,4,411+index*68,28};
}
void EditorShell::SetTransformTool(gizmo::Mode mode)
{
    EndGizmoDrag(true); transformTool_=mode; hoveredAxis_=-1;
    status_=mode==gizmo::Mode::Move?L"Move: drag an axis arrow (world X/Y/Z)":mode==gizmo::Mode::Rotate?L"Rotate: drag an axis ring (Inspector Euler angles)":L"Scale: drag a box-ended local axis";
    InvalidateRect(window_,nullptr,FALSE);
}
void EditorShell::EndGizmoDrag(bool cancel)
{
    if (!gizmoDrag_) return;
    if (cancel)
    {
        if (auto* object=zengine::As3D(objects_.Find(gizmoObject_))) object->GetTransform()=gizmoDrag_->Original();
        sceneDirty_=gizmoWasDirty_; UpdateSceneTitle();
    }
    if (!cancel)
    {
        if (const auto* object=zengine::As3D(objects_.Find(gizmoObject_)))
        {
            const auto equal=[](zengine::Vec3 a,zengine::Vec3 b) { return a.x==b.x && a.y==b.y && a.z==b.z; };
            const auto& original=gizmoDrag_->Original(); const auto& current=object->GetTransform();
            if (!equal(original.Position(),current.Position()) || !equal(original.Rotation(),current.Rotation()) || !equal(original.Scale(),current.Scale())) RecordTransformOverride(gizmoObject_);
        }
    }
    gizmoDrag_.reset(); gizmoObject_=0; hoveredAxis_=-1;
    if (GetCapture()==viewportWindow_) ReleaseCapture();
    if (inspectorPanel_) inspectorPanel_->RefreshLiveValues();
    status_=cancel?L"Transform drag canceled":L"Transform updated - save the scene to keep your changes";
    InvalidateRect(window_,nullptr,FALSE);
}
void EditorShell::UpdateGizmoDrag(gizmo::Point point)
{
    if (!gizmoDrag_) return;
    auto* object=zengine::As3D(objects_.Find(gizmoObject_));
    if (!object || Playing()) { EndGizmoDrag(true); return; }
    object->GetTransform()=gizmoDrag_->Update(point);
    // Keep no-op clicks and drags back to the start clean.
    const auto& current=object->GetTransform(); const auto& original=gizmoDrag_->Original();
    const auto equal=[](zengine::Vec3 a,zengine::Vec3 b) { return a.x==b.x && a.y==b.y && a.z==b.z; };
    const bool dirty=gizmoWasDirty_ || !equal(current.Position(),original.Position()) || !equal(current.Rotation(),original.Rotation()) || !equal(current.Scale(),original.Scale());
    if(dirty!=sceneDirty_){sceneDirty_=dirty;UpdateSceneTitle();}
}
LRESULT EditorShell::HandleViewportMessage(HWND window,UINT message,WPARAM w,LPARAM l)
{
    const gizmo::Point point{static_cast<float>(GET_X_LPARAM(l)),static_cast<float>(GET_Y_LPARAM(l))};
    switch (message)
    {
    case WM_RBUTTONDOWN:
        EndGizmoDrag(true);SetFocus(window);cameraPoint_={GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        cameraDrag_=(w&MK_CONTROL)?CameraDrag::Pan:(w&MK_SHIFT)?CameraDrag::Fly:CameraDrag::Orbit;
        SetCapture(window);CameraMotion(cameraPoint_);return 0;
    case WM_RBUTTONUP:EndCameraDrag();return 0;
    case WM_MBUTTONDOWN:
        EndGizmoDrag(true);SetFocus(window);cameraPoint_={GET_X_LPARAM(l),GET_Y_LPARAM(l)};cameraDrag_=CameraDrag::Pan;SetCapture(window);return 0;
    case WM_MBUTTONUP:EndCameraDrag();return 0;
    case WM_MOUSEWHEEL:
        EndGizmoDrag(true);{const auto mode=cameraDrag_;cameraDrag_=CameraDrag::Pan;CameraMotion(cameraPoint_);cameraDrag_=mode;sceneCamera_.distance=std::clamp(sceneCamera_.distance*std::exp(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(w))/WHEEL_DELTA*0.15f),0.05f,10000.0f);}return 0;
    case WM_LBUTTONDOWN:
    {
        if(cameraDrag_!=CameraDrag::None)return 0;
        SetFocus(window);
        if (ToggleStatsGroupAt(POINT{GET_X_LPARAM(l),GET_Y_LPARAM(l)})) { InvalidateRect(window,nullptr,FALSE); return 0; } // ZE-125
        // First, try to grab a transform-gizmo axis of the current selection.
        if (const auto* object=SelectedGameObject(); object && !Playing() && CanEdit(object->Id(),true))
        {
            ViewportCamera camera(static_cast<float>(requestedViewportWidth_),static_cast<float>(requestedViewportHeight_),sceneCamera_);
            const auto parent=ParentMatrix(objects_,*object);
            if (std::abs(DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(parent)))>1e-10f)
            {
                camera.view=parent*camera.view;
                const auto shape=gizmo::Build(camera,object->GetTransform(),transformTool_);
                if (const auto hit=gizmo::Pick(camera,shape,point))
                {
                    gizmoWasDirty_=sceneDirty_; gizmoObject_=object->Id(); hoveredAxis_=hit->axis;
                    gizmoDrag_.emplace(camera,object->GetTransform(),transformTool_,shape,*hit,point);
                    SetCapture(window); status_=L"Dragging transform - release to apply, Escape to cancel";
                    InvalidateRect(window_,&statusBar_,FALSE);
                    return 0;
                }
            }
        }
        // ZE-104: otherwise click-select the object under the cursor (empty space clears).
        const auto picked=PickObject(point);
        if (picked==selectedObject_) return 0;
        if (picked) SelectGameObject(picked);
        else { EndGizmoDrag(true); selectedObject_=0; if(inspectorPanel_)inspectorPanel_->Bind(nullptr); InvalidateRect(window_,&sceneBrowser_,FALSE); SetWindowTextW(viewportWindow_,L"Scene Viewport"); }
        return 0;
    }
    case WM_MOUSEMOVE:
        if(cameraDrag_!=CameraDrag::None){CameraMotion({GET_X_LPARAM(l),GET_Y_LPARAM(l)});return 0;}
        if (gizmoDrag_) UpdateGizmoDrag(point);
        else
        {
            hoveredAxis_=-1;
            if (const auto* object=SelectedGameObject(); object && !Playing() && CanEdit(object->Id(),true))
            {
                ViewportCamera camera(static_cast<float>(requestedViewportWidth_),static_cast<float>(requestedViewportHeight_),sceneCamera_);
                camera.view=ParentMatrix(objects_,*object)*camera.view;
                const auto shape=gizmo::Build(camera,object->GetTransform(),transformTool_);
                if (const auto hit=gizmo::Pick(camera,shape,point)) hoveredAxis_=hit->axis;
            }
            TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0}; TrackMouseEvent(&track);
        }
        SetCursor(LoadCursorW(nullptr,hoveredAxis_>=0?IDC_HAND:IDC_ARROW)); return 0;
    case WM_LBUTTONUP:
        if (gizmoDrag_) { UpdateGizmoDrag(point); EndGizmoDrag(false); } return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
    case WM_KILLFOCUS:
        EndGizmoDrag(true); EndCameraDrag(); return 0;
    case WM_MOUSELEAVE:
        if (!gizmoDrag_) hoveredAxis_=-1; return 0;
    case WM_KEYDOWN:
        if (w==VK_ESCAPE) { EndGizmoDrag(true); EndCameraDrag(); return 0; }
        if(cameraDrag_!=CameraDrag::None)return 0;
        if (w=='W' || w=='E' || w=='R') { SetTransformTool(w=='W'?gizmo::Mode::Move:w=='E'?gizmo::Mode::Rotate:gizmo::Mode::Scale); return 0; }
        break;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(window,message,w,l);
}
