#include "EditorShell.h"
#include "InspectorPanel.h"
#include "RenderTransform.h"
#include <windowsx.h>

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
        if (auto* object=objects_.Find(gizmoObject_)) object->GetTransform()=gizmoDrag_->Original();
        sceneDirty_=gizmoWasDirty_; UpdateSceneTitle();
    }
    if (!cancel)
    {
        if (const auto* object=objects_.Find(gizmoObject_))
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
    auto* object=objects_.Find(gizmoObject_);
    if (!object || Playing()) { EndGizmoDrag(true); return; }
    object->GetTransform()=gizmoDrag_->Update(point);
    // Keep no-op clicks and drags back to the start clean.
    const auto& current=object->GetTransform(); const auto& original=gizmoDrag_->Original();
    const auto equal=[](zengine::Vec3 a,zengine::Vec3 b) { return a.x==b.x && a.y==b.y && a.z==b.z; };
    sceneDirty_=gizmoWasDirty_ || !equal(current.Position(),original.Position()) || !equal(current.Rotation(),original.Rotation()) || !equal(current.Scale(),original.Scale());
    UpdateSceneTitle(); inspectorPanel_->RefreshLiveValues();
}
LRESULT EditorShell::HandleViewportMessage(HWND window,UINT message,WPARAM w,LPARAM l)
{
    const gizmo::Point point{static_cast<float>(GET_X_LPARAM(l)),static_cast<float>(GET_Y_LPARAM(l))};
    switch (message)
    {
    case WM_LBUTTONDOWN:
    {
        SetFocus(window);
        const auto* object=SelectedGameObject(); if (!object || Playing() || !CanEdit(object->Id(),true)) return 0;
        ViewportCamera camera(static_cast<float>(requestedViewportWidth_),static_cast<float>(requestedViewportHeight_));
        const auto parent=ParentMatrix(objects_,*object);
        if (std::abs(DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(parent)))<=1e-10f) return 0;
        camera.view=parent*camera.view;
        const auto shape=gizmo::Build(camera,object->GetTransform(),transformTool_);
        if (const auto hit=gizmo::Pick(camera,shape,point))
        {
            gizmoWasDirty_=sceneDirty_; gizmoObject_=object->Id(); hoveredAxis_=hit->axis;
            gizmoDrag_.emplace(camera,object->GetTransform(),transformTool_,shape,*hit,point);
            SetCapture(window); status_=L"Dragging transform - release to apply, Escape to cancel";
            InvalidateRect(window_,&statusBar_,FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (gizmoDrag_) UpdateGizmoDrag(point);
        else
        {
            hoveredAxis_=-1;
            if (const auto* object=SelectedGameObject(); object && !Playing() && CanEdit(object->Id(),true))
            {
                ViewportCamera camera(static_cast<float>(requestedViewportWidth_),static_cast<float>(requestedViewportHeight_));
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
        EndGizmoDrag(true); return 0;
    case WM_MOUSELEAVE:
        if (!gizmoDrag_) hoveredAxis_=-1; return 0;
    case WM_KEYDOWN:
        if (w==VK_ESCAPE) { EndGizmoDrag(true); return 0; }
        if (w=='W' || w=='E' || w=='R') { SetTransformTool(w=='W'?gizmo::Mode::Move:w=='E'?gizmo::Mode::Rotate:gizmo::Mode::Scale); return 0; }
        break;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(window,message,w,l);
}
