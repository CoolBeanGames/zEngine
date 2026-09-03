// ZE-74: forward per-vertex lighting. With LightParams.x (light count) == 0, or a
// material whose Ambient.w == 0, meshes render at full brightness (unlit).

cbuffer SceneConstants : register(b0)
{
    float4x4 WorldViewProjection;
    float4x4 World;
    float4x4 NormalWorld;
    float4 MaterialTint;   // ZE-65: albedo multiplier (rgb); a = 1
    float4 Ambient;        // ZE-74: rgb ambient floor; w = 1 apply lights, 0 = full bright
    float4 LightParams;    // x = active light count
};

struct GpuLight
{
    float4 posType;         // xyz position, w type (0 dir, 1 point, 2 spot)
    float4 dirRange;        // xyz beam direction (unit), w range
    float4 colorIntensity;  // xyz colour * intensity
    float4 spot;            // x cos(inner), y cos(outer), z falloff exponent
};

cbuffer LightConstants : register(b1)
{
    GpuLight Lights[8];
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

float3 ShadeVertex(float3 worldPos, float3 worldNormal)
{
    float3 result = Ambient.rgb;
    int count = (int)LightParams.x;
    [loop]
    for (int i = 0; i < count; ++i)
    {
        GpuLight light = Lights[i];
        int type = (int)light.posType.w;

        float3 toLight;
        float attenuation = 1.0f;
        if (type == 0) // directional
        {
            toLight = -normalize(light.dirRange.xyz);
        }
        else
        {
            float3 delta = light.posType.xyz - worldPos;
            float dist = length(delta);
            toLight = dist > 1e-4f ? delta / dist : float3(0, 1, 0);
            float t = saturate(dist / max(light.dirRange.w, 1e-3f));
            attenuation = pow(saturate(1.0f - t), max(light.spot.z, 0.1f));
            if (type == 2) // spot cone
            {
                float aligned = dot(normalize(light.dirRange.xyz), -toLight);
                attenuation *= saturate((aligned - light.spot.y) / max(light.spot.x - light.spot.y, 1e-3f));
            }
        }

        float ndotl = saturate(dot(worldNormal, toLight));
        result += light.colorIntensity.xyz * (ndotl * attenuation);
    }
    return result;
}

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position = mul(float4(input.position, 1.0f), WorldViewProjection);

    if (Ambient.w > 0.5f && LightParams.x > 0.5f)
    {
        float3 worldPos = mul(float4(input.position, 1.0f), World).xyz;
        float3 n = mul(float4(input.normal, 0.0f), NormalWorld).xyz;
        float3 worldNormal = n * rsqrt(max(dot(n, n), 1e-20f));
        output.color = input.color * ShadeVertex(worldPos, worldNormal);
    }
    else
    {
        output.color = input.color;
    }
    output.uv = input.uv;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    return float4(input.color * Albedo.Sample(AlbedoSampler, input.uv).rgb * MaterialTint.rgb, 1.0f);
}
