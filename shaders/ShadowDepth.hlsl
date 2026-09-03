// ZE-112: depth-only pass for the directional shadow map. Reuses SceneConstants
// b0 - WorldViewProjection is set to the light's view-projection during this pass.

cbuffer SceneConstants : register(b0)
{
    float4x4 WorldViewProjection;
    // (the rest of SceneConstants is unused here)
    float4x4 _World;
    float4x4 _NormalWorld;
    float4 _MaterialTint;
    float4 _Ambient;
    float4 _LightParams;
    float4 _CameraPos;
    float4 _FogColorMode;
    float4 _FogParams;
    float4 _HeightFog;
    float4 _MaterialSpec;
    float4x4 _ShadowMatrix;
    float4 _ShadowParams;
};

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 color : COLOR;
    float2 uv : TEXCOORD0;
};

float4 VSMain(VertexInput input) : SV_POSITION
{
    return mul(float4(input.position, 1.0f), WorldViewProjection);
}
