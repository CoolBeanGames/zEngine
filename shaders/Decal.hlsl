// ZE-76: projected decals. The main mesh pass runs first; then, for each decal,
// the affected meshes are redrawn with this shader. Each fragment is transformed
// into the decal's local unit-cube space; fragments outside the box are clipped,
// the rest sample the decal texture (projected down local -Z) and alpha-blend.

cbuffer DecalConstants : register(b0)
{
    float4x4 WorldViewProjection; // mesh object -> clip
    float4x4 World;               // mesh object -> world
    float4x4 DecalInvWorld;       // world -> decal local (unit cube [-0.5, 0.5])
    float4 Tint;                  // rgb tint, a = opacity
    float4 Params;                // x = cos(angle fade); yzw = world-space projection direction (unit)
};

Texture2D DecalTexture : register(t0);
SamplerState DecalSampler : register(s0);

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
    float3 worldPos : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position = mul(float4(input.position, 1.0f), WorldViewProjection);
    output.worldPos = mul(float4(input.position, 1.0f), World).xyz;
    output.worldNormal = mul(float4(input.normal, 0.0f), World).xyz;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    float3 local = mul(float4(input.worldPos, 1.0f), DecalInvWorld).xyz;
    // Outside the projector box => no decal.
    clip(0.5f - abs(local.x));
    clip(0.5f - abs(local.y));
    clip(0.5f - abs(local.z));

    float3 projectDir = Params.yzw; // world-space, unit
    float facing = dot(normalize(input.worldNormal), -projectDir);
    // Fade from fully-visible when the surface faces the projector to zero at the
    // angle-fade cutoff; drop back faces entirely.
    float fade = saturate((facing - Params.x) / max(1.0f - Params.x, 1e-3f));
    clip(fade - 1e-3f);

    float2 uv = local.xy * float2(1.0f, -1.0f) + 0.5f;
    float4 texel = DecalTexture.Sample(DecalSampler, uv);
    return float4(texel.rgb * Tint.rgb, texel.a * Tint.a * fade);
}
