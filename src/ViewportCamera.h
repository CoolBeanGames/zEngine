#pragma once
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

// Shared projection for rendering and editor picking. No GPU or window ownership.
struct SceneCamera
{
    DirectX::XMFLOAT3 target{0,0,0};
    float yaw=std::atan(0.55f),pitch=std::atan(0.38f/std::sqrt(1.0f+0.55f*0.55f)),distance=0;
};
struct ViewportCamera
{
    float width, height;
    DirectX::XMMATRIX view, projection;
    ViewportCamera(float w,float h,const SceneCamera& state={}):width(std::max(1.0f,w)),height(std::max(1.0f,h))
    {
        using namespace DirectX;
        const float aspect=width/height;
        const float halfFov=std::min(XM_PI/6,std::atan(std::tan(XM_PI/6)*aspect));
        const float distance=state.distance>0?state.distance:1.75f/std::sin(halfFov)*1.1f*1.04f;
        const auto target=XMLoadFloat3(&state.target);
        const auto offset=XMVectorSet(std::sin(state.yaw)*std::cos(state.pitch),std::sin(state.pitch),-std::cos(state.yaw)*std::cos(state.pitch),0);
        view=XMMatrixLookAtLH(target+offset*distance,target,XMVectorSet(0,1,0,0));
        projection=XMMatrixPerspectiveFovLH(XM_PI/3,aspect,0.05f,std::max(100.0f,distance*50));
    }
};
