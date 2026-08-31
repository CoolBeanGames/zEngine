#pragma once
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

// Shared projection for rendering and editor picking. No GPU or window ownership.
struct ViewportCamera
{
    float width, height;
    DirectX::XMMATRIX view, projection;
    ViewportCamera(float w,float h):width(std::max(1.0f,w)),height(std::max(1.0f,h))
    {
        using namespace DirectX;
        const float aspect=width/height;
        const float halfFov=std::min(XM_PI/6,std::atan(std::tan(XM_PI/6)*aspect));
        const float distance=1.75f/std::sin(halfFov)*1.1f;
        view=XMMatrixLookAtLH(XMVectorScale(XMVector3Normalize(XMVectorSet(0.55f,0.38f,-1,0)),distance*1.04f),XMVectorZero(),XMVectorSet(0,1,0,0));
        projection=XMMatrixPerspectiveFovLH(XM_PI/3,aspect,0.1f,100);
    }
};
