// ZE-74: forward per-vertex lighting (point / directional / spot). Zero lights or
// an unlit material => full brightness.  ZE-75: distance + height fog, a cheap
// volumetric term, per-vertex specular, and ordered dithering in the pixel shader.

cbuffer SceneConstants : register(b0)
{
    float4x4 WorldViewProjection;
    float4x4 World;
    float4x4 NormalWorld;
    float4 MaterialTint;   // ZE-65: albedo multiplier (rgb); a = 1
    float4 Ambient;        // ZE-74: rgb ambient; w = 1 apply lights, 0 = full bright
    float4 LightParams;    // x = active light count
    float4 CameraPos;      // ZE-75: xyz world camera
    float4 FogColorMode;   // ZE-75: rgb fog colour, w mode (0 off, 1 linear, 2 exp2)
    float4 FogParams;      // ZE-75: x near, y far, z density, w volumetric step count
    float4 HeightFog;      // ZE-75: x base, y falloff, z strength, w volumetric on
    float4 MaterialSpec;   // ZE-75: x roughness, y specular
    float4x4 ShadowMatrix; // ZE-112: world -> shadow-caster light clip space
    float4 ShadowParams;   // ZE-112: x strength (0 = no shadows), y texel size, z depth bias, w caster light index
};

Texture2D ShadowMap : register(t1);
SamplerComparisonState ShadowSampler : register(s1);

struct GpuLight
{
    float4 posType;         // xyz position, w type (0 dir, 1 point, 2 spot)
    float4 dirRange;        // xyz beam direction (unit), w range
    float4 colorIntensity;  // xyz colour * intensity
    float4 spot;            // x cos(inner), y cos(outer), z falloff exponent, w fog scatter
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
    float3 worldPos : TEXCOORD1;
    float3 shadowTerm : TEXCOORD2; // ZE-112: the shadow-caster light's own contribution
};

float LightAtten(GpuLight light, int type, float3 worldPos, out float3 toLight)
{
    if (type == 0) { toLight = -normalize(light.dirRange.xyz); return 1.0f; }
    float3 delta = light.posType.xyz - worldPos;
    float dist = length(delta);
    toLight = dist > 1e-4f ? delta / dist : float3(0, 1, 0);
    float t = saturate(dist / max(light.dirRange.w, 1e-3f));
    float a = pow(saturate(1.0f - t), max(light.spot.z, 0.1f));
    if (type == 2)
    {
        float aligned = dot(normalize(light.dirRange.xyz), -toLight);
        a *= saturate((aligned - light.spot.y) / max(light.spot.x - light.spot.y, 1e-3f));
    }
    return a;
}

float3 ShadeVertex(float3 worldPos, float3 worldNormal, out float3 shadowTerm)
{
    shadowTerm = 0.0f;
    float3 result = Ambient.rgb;
    float3 toView = normalize(CameraPos.xyz - worldPos);
    float shininess = lerp(6.0f, 120.0f, saturate(1.0f - MaterialSpec.x));
    int casterIndex = (int)ShadowParams.w;
    int count = (int)LightParams.x;
    [loop]
    for (int i = 0; i < count; ++i)
    {
        GpuLight light = Lights[i];
        int type = (int)light.posType.w;
        float3 toLight;
        float attenuation = LightAtten(light, type, worldPos, toLight);
        float ndotl = saturate(dot(worldNormal, toLight));
        float3 contribution = light.colorIntensity.xyz * (ndotl * attenuation);
        if (MaterialSpec.y > 0.001f && ndotl > 0.0f)
        {
            float3 h = normalize(toLight + toView);
            float spec = pow(saturate(dot(worldNormal, h)), shininess);
            contribution += light.colorIntensity.xyz * (spec * attenuation * MaterialSpec.y);
        }
        result += contribution;
        if (i == casterIndex) shadowTerm = contribution;
    }
    return result;
}

// 0 = fully shadowed, 1 = lit. PCF 3x3.
float ShadowFactor(float3 worldPos)
{
    if (ShadowParams.x < 0.001f) return 1.0f;
    float4 lc = mul(float4(worldPos, 1.0f), ShadowMatrix);
    lc.xyz /= lc.w;
    float2 uv = lc.xy * float2(0.5f, -0.5f) + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f || lc.z > 1.0f) return 1.0f;
    float depth = lc.z - ShadowParams.z;
    float sum = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
            sum += ShadowMap.SampleCmpLevelZero(ShadowSampler, uv + float2(x, y) * ShadowParams.y, depth);
    return lerp(1.0f, sum / 9.0f, ShadowParams.x);
}

float FogAmount(float3 worldPos)
{
    int mode = (int)FogColorMode.w;
    if (mode == 0) return 0.0f;
    float dist = length(worldPos - CameraPos.xyz);
    float f;
    if (mode == 1)
        f = saturate((dist - FogParams.x) / max(FogParams.y - FogParams.x, 1e-3f));
    else
    {
        float d = dist * FogParams.z;
        f = 1.0f - exp(-d * d);
    }
    // Height fog: extra density below HeightFog.x, fading over HeightFog.y.
    if (HeightFog.z > 0.0f)
    {
        float below = saturate((HeightFog.x - worldPos.y) / max(HeightFog.y, 1e-3f));
        f = saturate(f + below * HeightFog.z * saturate(dist / max(FogParams.y, 1e-3f)));
    }
    return f;
}

// ZE-124: single-tap shadow visibility for the volumetric march (no PCF / strength lerp).
// 1 = the sample point sees the caster light, 0 = it is in shadow -> no in-scatter there.
float VolumetricShadow(float3 worldPos)
{
    float4 lc = mul(float4(worldPos, 1.0f), ShadowMatrix);
    lc.xyz /= lc.w;
    float2 uv = lc.xy * float2(0.5f, -0.5f) + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f || lc.z > 1.0f) return 1.0f;
    return ShadowMap.SampleCmpLevelZero(ShadowSampler, uv, lc.z - ShadowParams.z);
}

// Henyey-Greenstein phase: forward-scattering (g > 0) brightens haze toward the light.
float HGPhase(float cosTheta, float g)
{
    float g2 = g * g;
    float d = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * 3.14159265f * max(d, 1e-4f) * sqrt(max(d, 1e-4f)));
}

// Ray-march the view ray, sampling the shadow map so light shafts / god rays appear
// where the directional light passes gaps in geometry. Tunables (Environment behavior):
//   volumetric        -> on / off              (HeightFog.w)
//   volumetric_steps  -> sample count          (FogParams.w)
//   fog_density       -> medium thickness      (FogParams.z)
//   fog_color         -> atmospheric tint      (FogColorMode.rgb)
//   Light.fog_scatter -> per-light beam weight (spot.w)
float3 Volumetric(float3 worldPos, float2 pixel)
{
    float3 accum = 0.0f;
    if (HeightFog.w < 0.5f) return accum;

    int steps = clamp((int)FogParams.w, 2, 32);
    int count = (int)LightParams.x;
    int caster = (int)ShadowParams.w;

    float3 camToPos = worldPos - CameraPos.xyz;
    float total = length(camToPos);
    float3 dir = total > 1e-4f ? camToPos / total : float3(0, 0, 1);
    float marchEnd = min(total, max(FogParams.y, 1.0f)); // never march past the fog far plane
    float segment = marchEnd / (float)steps;

    // Per-pixel dither on the start offset: few steps still read as smooth beams.
    const float bayer[16] = {
        0.0f, 8.0f, 2.0f, 10.0f, 12.0f, 4.0f, 14.0f, 6.0f,
        3.0f, 11.0f, 1.0f, 9.0f, 15.0f, 7.0f, 13.0f, 5.0f
    };
    int bx = ((int)pixel.x) & 3, by = ((int)pixel.y) & 3;
    float jitter = (bayer[by * 4 + bx] + 0.5f) / 16.0f;

    float baseDensity = 0.04f + 0.6f * saturate(FogParams.z * 3.0f);
    float3 tint = ((int)FogColorMode.w != 0) ? saturate(FogColorMode.rgb + 0.15f) : float3(1, 1, 1);

    [loop]
    for (int s = 0; s < steps; ++s)
    {
        float3 p = CameraPos.xyz + dir * ((s + jitter) * segment);

        float density = baseDensity;
        if (HeightFog.z > 0.0f)
        {
            float below = saturate((HeightFog.x - p.y) / max(HeightFog.y, 1e-3f));
            density += below * HeightFog.z * 0.2f;
        }

        [loop]
        for (int i = 0; i < count; ++i)
        {
            GpuLight light = Lights[i];
            float scatter = light.spot.w;
            if (scatter <= 0.001f) continue;
            int type = (int)light.posType.w;

            float3 toLight;
            float a = LightAtten(light, type, p, toLight);
            if (a <= 0.001f) continue;

            float vis = 1.0f;
            if (type == 0 && ShadowParams.x > 0.001f && i == caster)
                vis = VolumetricShadow(p);

            float phase = HGPhase(dot(dir, toLight), 0.55f);
            accum += light.colorIntensity.xyz * (a * vis * scatter * phase * density * segment);
        }
    }
    return accum * tint;
}

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position = mul(float4(input.position, 1.0f), WorldViewProjection);
    output.worldPos = mul(float4(input.position, 1.0f), World).xyz;

    output.shadowTerm = 0.0f;
    if (Ambient.w > 0.5f && LightParams.x > 0.5f)
    {
        float3 n = mul(float4(input.normal, 0.0f), NormalWorld).xyz;
        float3 worldNormal = n * rsqrt(max(dot(n, n), 1e-20f));
        float3 shadowTerm;
        output.color = input.color * ShadeVertex(output.worldPos, worldNormal, shadowTerm);
        output.shadowTerm = input.color * shadowTerm;
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
    // ZE-112: darken the shadow-caster light's contribution where the fragment is occluded.
    float shadow = ShadowFactor(input.worldPos);
    float3 shaded = max(input.color - input.shadowTerm * (1.0f - shadow), 0.0f);
    float3 lit = shaded * Albedo.Sample(AlbedoSampler, input.uv).rgb * MaterialTint.rgb;

    float fog = FogAmount(input.worldPos);
    float3 result = lerp(lit, FogColorMode.rgb, fog) + Volumetric(input.worldPos, input.position.xy);

    // Ordered 4x4 Bayer dithering to break up fog / gradient banding.
    const float bayer[16] = {
        0.0f, 8.0f, 2.0f, 10.0f, 12.0f, 4.0f, 14.0f, 6.0f,
        3.0f, 11.0f, 1.0f, 9.0f, 15.0f, 7.0f, 13.0f, 5.0f
    };
    int bx = ((int)input.position.x) & 3;
    int by = ((int)input.position.y) & 3;
    float d = (bayer[by * 4 + bx] / 16.0f - 0.5f) / 64.0f;
    return float4(saturate(result + d), 1.0f);
}
