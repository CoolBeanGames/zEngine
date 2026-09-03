#pragma once

// ZE-74: turns a Light behaviour + its world transform into a renderer LightData.
// Shared by the editor viewport and the standalone player.

#include "RenderScene.h"
#include "RenderTransform.h"
#include "core/Light.h"

#include <cmath>

inline LightData MakeLight(const zengine::Light& light, const zengine::Transform& local,
                           const DirectX::XMMATRIX& parentWorld)
{
    LightData d;
    d.type = static_cast<int>(light.LightType());
    d.color = {light.Color().x, light.Color().y, light.Color().z};
    d.intensity = light.Intensity();
    d.range = light.Range();
    d.falloff = light.Falloff();
    d.spotCosInner = std::cos(light.SpotInner() * 3.14159265f / 180.0f);
    d.spotCosOuter = std::cos(light.SpotOuter() * 3.14159265f / 180.0f);

    const DirectX::XMMATRIX world = TransformMatrix(local) * parentWorld;
    DirectX::XMFLOAT3 pos;
    DirectX::XMStoreFloat3(&pos, DirectX::XMVector3TransformCoord(DirectX::XMVectorZero(), world));
    d.position = {pos.x, pos.y, pos.z};
    DirectX::XMFLOAT3 fwd;
    DirectX::XMStoreFloat3(&fwd, DirectX::XMVector3Normalize(
        DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0, 0, 1, 0), world)));
    d.direction = {fwd.x, fwd.y, fwd.z};
    return d;
}
