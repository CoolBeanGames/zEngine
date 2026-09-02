// 2D sprite / UI pass. Positions arrive in screen pixels (origin top-left, Y down);
// the vertex shader maps them straight to clip space. Deliberately unlit and
// depth-independent so nothing here is touched by the 3D lighting systems.
cbuffer SpriteConstants : register(b0)
{
    float2 InverseScreen; // 1 / (width, height)
    float2 Padding;
};

Texture2D Sprite : register(t0);
SamplerState SpriteSampler : register(s0);

struct VertexInput
{
    float2 position : POSITION;  // screen pixels
    float2 uv : TEXCOORD0;
    float4 color : COLOR;        // straight-alpha tint
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    const float2 ndc = float2(
        input.position.x * InverseScreen.x * 2.0f - 1.0f,
        1.0f - input.position.y * InverseScreen.y * 2.0f);
    output.position = float4(ndc, 0.0f, 1.0f);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    return Sprite.Sample(SpriteSampler, input.uv) * input.color;
}
