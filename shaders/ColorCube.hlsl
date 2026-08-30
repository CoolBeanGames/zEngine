cbuffer SceneConstants : register(b0)
{
    float4x4 WorldViewProjection;
    float4x4 NormalWorld;
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

    const float3 transformedNormal = mul(float4(input.normal, 0.0f), NormalWorld).xyz;
    const float3 worldNormal = transformedNormal * rsqrt(max(dot(transformedNormal, transformedNormal), 1e-20f));
    const float diffuse = saturate(dot(worldNormal, normalize(-LightDirection.xyz)));
    const float lighting = lerp(0.25f + diffuse * 0.75f, 1.0f, LightDirection.w);
    output.color = input.color * lighting;
    output.uv = input.uv;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    return float4(input.color * Albedo.Sample(AlbedoSampler, input.uv).rgb, 1.0f);
}
