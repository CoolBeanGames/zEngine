cbuffer SceneConstants : register(b0)
{
    float4x4 WorldViewProjection;
    float4x4 World;
    float4 LightDirection;
};

Texture2D Albedo : register(t0);
SamplerState AlbedoSampler : register(s0);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 color : COLOR;
    float2 uv : TEXCOORD0;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float3 color : COLOR;
    float2 uv : TEXCOORD0;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position = mul(float4(input.position, 1.0f), WorldViewProjection);

    const float3 worldNormal = normalize(mul(float4(input.normal, 0.0f), World).xyz);
    const float diffuse = saturate(dot(worldNormal, normalize(-LightDirection.xyz)));
    const float lighting = 0.25f + diffuse * 0.75f;
    output.color = input.color * lighting;
    output.uv = input.uv;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    return float4(input.color * Albedo.Sample(AlbedoSampler, input.uv).rgb, 1.0f);
}
