#pragma once
#include "core/GameObject.h"
#include <DirectXMath.h>

// Renderer-side adapter. Core GameObjects and behaviors have no DirectX/Win32 dependencies.
inline DirectX::XMMATRIX TransformMatrix(const zengine::Transform& transform)
{
    using namespace DirectX;
    const auto& s = transform.Scale();
    const auto& r = transform.Rotation();
    const auto& p = transform.Position();
    return XMMatrixScaling(s.x, s.y, s.z) * XMMatrixRotationX(XMConvertToRadians(r.x)) *
        XMMatrixRotationY(XMConvertToRadians(r.y)) * XMMatrixRotationZ(XMConvertToRadians(r.z)) *
        XMMatrixTranslation(p.x, p.y, p.z);
}
