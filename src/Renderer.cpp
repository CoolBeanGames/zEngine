#include "Renderer.h"
#include "RenderTransform.h"
#include "Render2D.h"

#include <DirectXMath.h>
#include <d3dcompiler.h>
#include <wincodec.h>

#include <array>
#include <cstddef>
#include <fstream>
#include <random>
#include <vector>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct RenderMesh
{
    ComPtr<ID3D11Buffer> vertices, indices;
    std::vector<ComPtr<ID3D11ShaderResourceView>> textures;
    std::vector<MeshPart> parts;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT;
    ID3D11Device* device = nullptr; // Buffers keep their creating device alive.
};

struct RenderTexture
{
    ComPtr<ID3D11ShaderResourceView> view;
    std::uint32_t width = 0, height = 0;
    ID3D11Device* device = nullptr;
};

struct RenderMaterial
{
    TextureHandle albedo;             // null -> fall back to the model's imported texture
    Float4 tint{1, 1, 1, 1};
    bool lit = true;                  // ZE-74: false -> mesh ignores scene lights
    float roughness = 0.5f;           // ZE-75
    float specular = 0.0f;
};

namespace
{
    void ThrowIfFailed(const HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            std::ostringstream message;
            message << operation << " failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(result) << ").";
            throw std::runtime_error(message.str());
        }
    }

    ComPtr<ID3DBlob> CompileShader(
        const std::filesystem::path& path,
        const char* entryPoint,
        const char* target)
    {
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ComPtr<ID3DBlob> bytecode;
        ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompileFromFile(
            path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entryPoint, target, flags, 0, &bytecode, &errors);

        if (FAILED(result))
        {
            const std::string details = errors
                ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                : "No compiler details were provided.";
            throw std::runtime_error("Could not compile shader " + path.string() + ":\n" + details);
        }
        return bytecode;
    }
}

struct Renderer::SceneConstants
{
    XMFLOAT4X4 worldViewProjection;
    XMFLOAT4X4 world;                  // ZE-74: object -> world (for per-vertex lighting)
    XMFLOAT4X4 normalWorld;
    XMFLOAT4 materialTint{1, 1, 1, 1}; // ZE-65: material instance tint (albedo multiplier)
    XMFLOAT4 ambient{0.10f, 0.10f, 0.12f, 0.0f}; // ZE-74: rgb ambient; w = 1 apply lights, 0 = full bright
    XMFLOAT4 lightParams{0, 0, 0, 0};  // x = active light count
    XMFLOAT4 cameraPos{0, 0, 0, 0};                // ZE-75
    XMFLOAT4 fogColorMode{0.55f, 0.60f, 0.68f, 0}; // rgb + mode
    XMFLOAT4 fogParams{8, 60, 0.03f, 6};           // near, far, density, volumetric steps
    XMFLOAT4 heightFog{0, 6, 0, 0};                // base, falloff, strength, volumetric on
    XMFLOAT4 materialSpec{0.5f, 0, 0, 0};          // roughness, specular
    XMFLOAT4X4 shadowMatrix{};                     // ZE-112: world -> shadow light clip
    XMFLOAT4 shadowParams{0, 0, 0, -1};            // strength, texel size, bias, caster index (-1 none)
};
struct Renderer::LightConstants
{
    struct GpuLight { XMFLOAT4 posType{}, dirRange{}, colorIntensity{}, spot{}; } lights[8]{};
};

struct Renderer::SpriteConstants
{
    XMFLOAT2 inverseScreen;
    XMFLOAT2 padding;
};

struct Renderer::DecalConstants
{
    XMFLOAT4X4 worldViewProjection; // mesh object -> clip
    XMFLOAT4X4 world;               // mesh object -> world
    XMFLOAT4X4 decalInvWorld;       // world -> decal local unit cube
    XMFLOAT4 tint{1, 1, 1, 1};      // rgb + opacity
    XMFLOAT4 params{0.2588f, 0, 0, -1}; // x = cos(angle fade), yzw = world projection direction
};

void Renderer::Initialize(HWND window, const std::uint32_t width, const std::uint32_t height)
{
    CreateDeviceAndSwapChain(window, width, height);
    CreateRenderTargets(width, height);
    CreateShaders();
    CreateCube();
    // White fallback makes the original vertex-colored cube use the same material path.
    const std::uint32_t white = 0xffffffff;
    D3D11_TEXTURE2D_DESC textureDescription{};
    textureDescription.Width = textureDescription.Height = 1;
    textureDescription.MipLevels = textureDescription.ArraySize = 1;
    textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDescription.SampleDesc.Count = 1;
    textureDescription.Usage = D3D11_USAGE_IMMUTABLE;
    textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA whiteData{&white, 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    ThrowIfFailed(device_->CreateTexture2D(&textureDescription, &whiteData, &texture), "Create white texture");
    ThrowIfFailed(device_->CreateShaderResourceView(texture.Get(), nullptr, &whiteTexture_), "Create white texture view");
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    ThrowIfFailed(device_->CreateSamplerState(&sampler, &albedoSampler_), "Create albedo sampler");
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE; // Static previews also display mirrored/two-sided FBX geometry.
    rasterizer.DepthClipEnable = TRUE;
    ThrowIfFailed(device_->CreateRasterizerState(&rasterizer, &rasterizer_), "Create rasterizer");
    CreateShadowResources();
    CreateEditorGuides();
    CreateSpritePass();
    CreateDecalPass();
}

void Renderer::CreateDecalPass()
{
    const std::filesystem::path shaderPath = ShaderFile(L"Decal.hlsl");
    const ComPtr<ID3DBlob> vertexBytecode = CompileShader(shaderPath, "VSMain", "vs_5_0");
    const ComPtr<ID3DBlob> pixelBytecode = CompileShader(shaderPath, "PSMain", "ps_5_0");
    ThrowIfFailed(device_->CreateVertexShader(vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
                                              nullptr, &decalVertexShader_), "Create decal vertex shader");
    ThrowIfFailed(device_->CreatePixelShader(pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(),
                                             nullptr, &decalPixelShader_), "Create decal pixel shader");

    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(DecalConstants);
    constants.Usage = D3D11_USAGE_DYNAMIC;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ThrowIfFailed(device_->CreateBuffer(&constants, nullptr, &decalConstantBuffer_), "Create decal constant buffer");

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ThrowIfFailed(device_->CreateBlendState(&blend, &decalBlend_), "Create decal blend state");

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ThrowIfFailed(device_->CreateDepthStencilState(&depth, &decalDepth_), "Create decal depth state");
}

void Renderer::RenderDecals(const ViewportFrame& frame, const XMMATRIX& viewProjection)
{
    if (frame.decals.empty() || frame.meshes.empty()) return;

    constexpr UINT stride = static_cast<UINT>(sizeof(Vertex));
    constexpr UINT offset = 0;
    context_->IASetInputLayout(inputLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(decalVertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(decalPixelShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, decalConstantBuffer_.GetAddressOf());
    context_->PSSetConstantBuffers(0, 1, decalConstantBuffer_.GetAddressOf());
    context_->PSSetSamplers(0, 1, albedoSampler_.GetAddressOf());
    const float blendFactor[4]{0, 0, 0, 0};
    context_->OMSetBlendState(decalBlend_.Get(), blendFactor, 0xffffffff);
    context_->OMSetDepthStencilState(decalDepth_.Get(), 0);

    for (const auto& decal : frame.decals)
    {
        const XMMATRIX decalWorld = TransformMatrix(decal.transform) *
            (decal.parentMatrix ? XMLoadFloat4x4(&*decal.parentMatrix) : XMMatrixIdentity());
        if (std::abs(XMVectorGetX(XMMatrixDeterminant(decalWorld))) < 1e-12f) continue;
        const XMMATRIX decalInvWorld = XMMatrixInverse(nullptr, decalWorld);
        XMFLOAT3 projectDir;
        XMStoreFloat3(&projectDir, XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0, 0, -1, 0), decalWorld)));
        ID3D11ShaderResourceView* texture = decal.texture ? decal.texture->view.Get() : whiteTexture_.Get();
        context_->PSSetShaderResources(0, 1, &texture);

        for (const auto& draw : frame.meshes)
        {
            if (!draw.mesh) continue;
            const XMMATRIX meshWorld = TransformMatrix(draw.transform) *
                (draw.parentMatrix ? XMLoadFloat4x4(&*draw.parentMatrix) : XMMatrixIdentity());
            DecalConstants dc{};
            XMStoreFloat4x4(&dc.worldViewProjection, XMMatrixTranspose(meshWorld * viewProjection));
            XMStoreFloat4x4(&dc.world, XMMatrixTranspose(meshWorld));
            XMStoreFloat4x4(&dc.decalInvWorld, XMMatrixTranspose(decalInvWorld));
            dc.tint = {decal.tint.x, decal.tint.y, decal.tint.z, decal.opacity};
            dc.params = {decal.angleFadeCos, projectDir.x, projectDir.y, projectDir.z};
            D3D11_MAPPED_SUBRESOURCE mapped{};
            ThrowIfFailed(context_->Map(decalConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map decal constants");
            *static_cast<DecalConstants*>(mapped.pData) = dc;
            context_->Unmap(decalConstantBuffer_.Get(), 0);
            context_->IASetVertexBuffers(0, 1, draw.mesh->vertices.GetAddressOf(), &stride, &offset);
            context_->IASetIndexBuffer(draw.mesh->indices.Get(), draw.mesh->indexFormat, 0);
            for (const auto& part : draw.mesh->parts) context_->DrawIndexed(part.indexCount, part.firstIndex, 0);
        }
    }

    // Restore the main-pass pipeline so the editor guides / sprites draw normally.
    context_->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    context_->OMSetDepthStencilState(nullptr, 0);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, sceneConstantBuffer_.GetAddressOf());
    context_->VSSetConstantBuffers(1, 1, lightConstantBuffer_.GetAddressOf());
    context_->PSSetConstantBuffers(0, 1, sceneConstantBuffer_.GetAddressOf());
    context_->PSSetConstantBuffers(1, 1, lightConstantBuffer_.GetAddressOf());
}

void Renderer::CreateShadowResources()
{
    constexpr UINT kSize = 2048;
    D3D11_TEXTURE2D_DESC t{};
    t.Width = t.Height = kSize;
    t.MipLevels = t.ArraySize = 1;
    t.Format = DXGI_FORMAT_R32_TYPELESS;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ThrowIfFailed(device_->CreateTexture2D(&t, nullptr, &shadowTexture_), "Create shadow map");

    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ThrowIfFailed(device_->CreateDepthStencilView(shadowTexture_.Get(), &dsv, &shadowDsv_), "Create shadow DSV");

    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    ThrowIfFailed(device_->CreateShaderResourceView(shadowTexture_.Get(), &srv, &shadowSrv_), "Create shadow SRV");

    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    s.BorderColor[0] = s.BorderColor[1] = s.BorderColor[2] = s.BorderColor[3] = 1.0f;
    s.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    ThrowIfFailed(device_->CreateSamplerState(&s, &shadowSampler_), "Create shadow sampler");

    context_->ClearDepthStencilView(shadowDsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
}

void Renderer::CreateDeviceAndSwapChain(HWND window, const std::uint32_t width, const std::uint32_t height)
{
    DXGI_SWAP_CHAIN_DESC swapChainDescription{};
    swapChainDescription.BufferDesc.Width = width;
    swapChainDescription.BufferDesc.Height = height;
    swapChainDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDescription.SampleDesc.Count = 1;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.BufferCount = 2;
    swapChainDescription.OutputWindow = window;
    swapChainDescription.Windowed = TRUE;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    UINT deviceFlags = 0;
#if defined(_DEBUG)
    deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    constexpr std::array featureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selectedFeatureLevel{};

    const auto createDevice = [&](const D3D_FEATURE_LEVEL* levels, const UINT levelCount, const UINT flags) {
        return D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels, levelCount, D3D11_SDK_VERSION,
            &swapChainDescription, &swapChain_, &device_, &selectedFeatureLevel, &context_);
    };

    HRESULT result = createDevice(
        featureLevels.data(), static_cast<UINT>(featureLevels.size()), deviceFlags);

#if defined(_DEBUG)
    // The debug layer is optional in Windows. Keep Debug builds runnable when it is not installed.
    if (result == DXGI_ERROR_SDK_COMPONENT_MISSING)
    {
        deviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        result = createDevice(featureLevels.data(), static_cast<UINT>(featureLevels.size()), deviceFlags);
    }
#endif

    if (result == E_INVALIDARG)
    {
        result = createDevice(featureLevels.data() + 1, 1, deviceFlags);
    }
#if defined(_DEBUG)
    if (result == DXGI_ERROR_SDK_COMPONENT_MISSING)
    {
        deviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        result = createDevice(featureLevels.data() + 1, 1, deviceFlags);
    }
#endif
    ThrowIfFailed(result, "D3D11CreateDeviceAndSwapChain");
}

void Renderer::CreateRenderTargets(const std::uint32_t width, const std::uint32_t height)
{
    width_ = width;
    height_ = height;

    ComPtr<ID3D11Texture2D> backBuffer;
    ThrowIfFailed(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "Get swap-chain back buffer");
    ThrowIfFailed(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTargetView_),
                  "Create render-target view");

    D3D11_TEXTURE2D_DESC depthDescription{};
    depthDescription.Width = width;
    depthDescription.Height = height;
    depthDescription.MipLevels = 1;
    depthDescription.ArraySize = 1;
    depthDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDescription.SampleDesc.Count = 1;
    depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ThrowIfFailed(device_->CreateTexture2D(&depthDescription, nullptr, &depthTexture_),
                  "Create depth texture");
    ThrowIfFailed(device_->CreateDepthStencilView(depthTexture_.Get(), nullptr, &depthStencilView_),
                  "Create depth-stencil view");

    viewport_.TopLeftX = 0.0f;
    viewport_.TopLeftY = 0.0f;
    viewport_.Width = static_cast<float>(width);
    viewport_.Height = static_cast<float>(height);
    viewport_.MinDepth = 0.0f;
    viewport_.MaxDepth = 1.0f;
}

void Renderer::CreateShaders()
{
    const std::filesystem::path shaderPath = FindShaderPath();
    const ComPtr<ID3DBlob> vertexBytecode = CompileShader(shaderPath, "VSMain", "vs_5_0");
    const ComPtr<ID3DBlob> pixelBytecode = CompileShader(shaderPath, "PSMain", "ps_5_0");

    ThrowIfFailed(device_->CreateVertexShader(vertexBytecode->GetBufferPointer(),
                                               vertexBytecode->GetBufferSize(), nullptr, &vertexShader_),
                  "Create vertex shader");
    ThrowIfFailed(device_->CreatePixelShader(pixelBytecode->GetBufferPointer(),
                                              pixelBytecode->GetBufferSize(), nullptr, &pixelShader_),
                  "Create pixel shader");

    constexpr std::array inputElements{
        D3D11_INPUT_ELEMENT_DESC{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                                 offsetof(Vertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
        D3D11_INPUT_ELEMENT_DESC{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                                 offsetof(Vertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
        D3D11_INPUT_ELEMENT_DESC{"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                                 offsetof(Vertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
        D3D11_INPUT_ELEMENT_DESC{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                                 offsetof(Vertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ThrowIfFailed(device_->CreateInputLayout(inputElements.data(), static_cast<UINT>(inputElements.size()),
                                              vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
                                              &inputLayout_),
                  "Create input layout");

    // ZE-112: depth-only vertex shader for the directional shadow map.
    const ComPtr<ID3DBlob> shadowVs = CompileShader(ShaderFile(L"ShadowDepth.hlsl"), "VSMain", "vs_5_0");
    ThrowIfFailed(device_->CreateVertexShader(shadowVs->GetBufferPointer(), shadowVs->GetBufferSize(), nullptr, &shadowVertexShader_),
                  "Create shadow depth vertex shader");
}

void Renderer::CreateCube()
{
    auto mesh = std::make_shared<RenderMesh>();
    mesh->device = device_.Get();
    mesh->parts = {{0,36,0}};
    mesh->indexFormat = DXGI_FORMAT_R16_UINT;
    std::mt19937 randomEngine(std::random_device{}());
    std::uniform_real_distribution<float> colorChannel(0.2f, 1.0f);
    const auto randomColor = [&]() {
        return Float3{colorChannel(randomEngine), colorChannel(randomEngine), colorChannel(randomEngine)};
    };

    std::array<Vertex, 8> vertices{
        Vertex{{-1.0f, -1.0f, -1.0f}, {-0.577f, -0.577f, -0.577f}, randomColor()},
        Vertex{{-1.0f, +1.0f, -1.0f}, {-0.577f, +0.577f, -0.577f}, randomColor()},
        Vertex{{+1.0f, +1.0f, -1.0f}, {+0.577f, +0.577f, -0.577f}, randomColor()},
        Vertex{{+1.0f, -1.0f, -1.0f}, {+0.577f, -0.577f, -0.577f}, randomColor()},
        Vertex{{-1.0f, -1.0f, +1.0f}, {-0.577f, -0.577f, +0.577f}, randomColor()},
        Vertex{{-1.0f, +1.0f, +1.0f}, {-0.577f, +0.577f, +0.577f}, randomColor()},
        Vertex{{+1.0f, +1.0f, +1.0f}, {+0.577f, +0.577f, +0.577f}, randomColor()},
        Vertex{{+1.0f, -1.0f, +1.0f}, {+0.577f, -0.577f, +0.577f}, randomColor()},
    };

    constexpr std::array<std::uint16_t, 36> indices{
        0, 1, 2, 0, 2, 3,
        7, 6, 5, 7, 5, 4,
        4, 5, 1, 4, 1, 0,
        3, 2, 6, 3, 6, 7,
        1, 5, 6, 1, 6, 2,
        4, 0, 3, 4, 3, 7,
    };

    D3D11_BUFFER_DESC vertexDescription{};
    vertexDescription.ByteWidth = static_cast<UINT>(sizeof(vertices));
    vertexDescription.Usage = D3D11_USAGE_IMMUTABLE;
    vertexDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    const D3D11_SUBRESOURCE_DATA vertexData{vertices.data(), 0, 0};
    ThrowIfFailed(device_->CreateBuffer(&vertexDescription, &vertexData, &mesh->vertices),
                  "Create cube vertex buffer");

    D3D11_BUFFER_DESC indexDescription{};
    indexDescription.ByteWidth = static_cast<UINT>(sizeof(indices));
    indexDescription.Usage = D3D11_USAGE_IMMUTABLE;
    indexDescription.BindFlags = D3D11_BIND_INDEX_BUFFER;
    const D3D11_SUBRESOURCE_DATA indexData{indices.data(), 0, 0};
    ThrowIfFailed(device_->CreateBuffer(&indexDescription, &indexData, &mesh->indices),
                  "Create cube index buffer");

    D3D11_BUFFER_DESC constantDescription{};
    constantDescription.ByteWidth = static_cast<UINT>(sizeof(SceneConstants));
    constantDescription.Usage = D3D11_USAGE_DYNAMIC;
    constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_BUFFER_DESC lightDescription = constantDescription;
    lightDescription.ByteWidth = static_cast<UINT>(sizeof(LightConstants));
    ThrowIfFailed(device_->CreateBuffer(&lightDescription, nullptr, &lightConstantBuffer_), "Create light constant buffer");
    ThrowIfFailed(device_->CreateBuffer(&constantDescription, nullptr, &sceneConstantBuffer_),
                  "Create scene constant buffer");
    cube_ = std::move(mesh);
}

void Renderer::Resize(const std::uint32_t width, const std::uint32_t height)
{
    if (!swapChain_ || width == 0 || height == 0 || (width == width_ && height == height_))
    {
        return;
    }

    context_->OMSetRenderTargets(0, nullptr, nullptr);
    renderTargetView_.Reset();
    depthStencilView_.Reset();
    depthTexture_.Reset();
    ThrowIfFailed(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0),
                  "Resize swap-chain buffers");
    CreateRenderTargets(width, height);
}

void Renderer::CreateEditorGuides()
{
    std::vector<Vertex> lines;
    const auto line = [&](Float3 a, Float3 b, Float3 color) {
        lines.push_back({a, {0, 1, 0}, color}); lines.push_back({b, {0, 1, 0}, color});
    };
    const auto upload = [&](ComPtr<ID3D11Buffer>& buffer, UINT& count) {
        count = static_cast<UINT>(lines.size());
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(lines.size() * sizeof(Vertex));
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA data{lines.data(), 0, 0};
        ThrowIfFailed(device_->CreateBuffer(&description, &data, &buffer), "Create editor guide buffer");
    };
    for (int i = -10; i <= 10; ++i)
    {
        const float p = static_cast<float>(i);
        const Float3 color = i == 0 ? Float3{0.28f, 0.30f, 0.34f} : Float3{0.12f, 0.14f, 0.17f};
        line({p, 0, -10}, {p, 0, 10}, color); line({-10, 0, p}, {10, 0, p}, color);
    }
    upload(gridBuffer_, gridVertexCount_);
    lines.clear();
    D3D11_BUFFER_DESC handles{};
    handles.ByteWidth=1024*sizeof(Vertex); handles.Usage=D3D11_USAGE_DYNAMIC;
    handles.BindFlags=D3D11_BIND_VERTEX_BUFFER; handles.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    ThrowIfFailed(device_->CreateBuffer(&handles,nullptr,&axesBuffer_),"Create transform handle buffer");
    handles.ByteWidth=65536*sizeof(Vertex);ThrowIfFailed(device_->CreateBuffer(&handles,nullptr,&colliderBuffer_),"Create collider guide buffer");
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    ThrowIfFailed(device_->CreateDepthStencilState(&depth, &overlayDepth_), "Create editor overlay depth state");
}

void Renderer::CreateSpritePass()
{
    const std::filesystem::path shaderPath = ShaderFile(L"Sprite.hlsl");
    const ComPtr<ID3DBlob> vertexBytecode = CompileShader(shaderPath, "VSMain", "vs_5_0");
    const ComPtr<ID3DBlob> pixelBytecode = CompileShader(shaderPath, "PSMain", "ps_5_0");
    ThrowIfFailed(device_->CreateVertexShader(vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
                                              nullptr, &spriteVertexShader_), "Create sprite vertex shader");
    ThrowIfFailed(device_->CreatePixelShader(pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(),
                                             nullptr, &spritePixelShader_), "Create sprite pixel shader");

    constexpr std::array spriteElements{
        D3D11_INPUT_ELEMENT_DESC{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                                 offsetof(SpriteVertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
        D3D11_INPUT_ELEMENT_DESC{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                                 offsetof(SpriteVertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
        D3D11_INPUT_ELEMENT_DESC{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                                 offsetof(SpriteVertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ThrowIfFailed(device_->CreateInputLayout(spriteElements.data(), static_cast<UINT>(spriteElements.size()),
                                             vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
                                             &spriteInputLayout_), "Create sprite input layout");

    D3D11_BUFFER_DESC vertexDescription{};
    vertexDescription.ByteWidth = kSpriteVertexCapacity * static_cast<UINT>(sizeof(SpriteVertex));
    vertexDescription.Usage = D3D11_USAGE_DYNAMIC;
    vertexDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertexDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ThrowIfFailed(device_->CreateBuffer(&vertexDescription, nullptr, &spriteVertexBuffer_), "Create sprite vertex buffer");

    D3D11_BUFFER_DESC constantDescription{};
    constantDescription.ByteWidth = sizeof(SpriteConstants);
    constantDescription.Usage = D3D11_USAGE_DYNAMIC;
    constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ThrowIfFailed(device_->CreateBuffer(&constantDescription, nullptr, &spriteConstantBuffer_), "Create sprite constant buffer");

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ThrowIfFailed(device_->CreateBlendState(&blend, &spriteBlend_), "Create sprite blend state");

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    ThrowIfFailed(device_->CreateSamplerState(&sampler, &spriteSampler_), "Create sprite sampler");

    const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
    whiteHandle_ = UploadTexture(1, 1, white.data());
}

TextureHandle Renderer::UploadTexture(std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba)
{
    if (!device_) throw std::runtime_error("Initialize the renderer before uploading textures.");
    if (!rgba || width == 0 || height == 0 || width > 16384 || height > 16384)
        throw std::runtime_error("Texture dimensions are invalid or oversized.");

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{rgba, width * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    ThrowIfFailed(device_->CreateTexture2D(&description, &data, &texture), "Create 2D texture");
    auto result = std::make_shared<RenderTexture>();
    ThrowIfFailed(device_->CreateShaderResourceView(texture.Get(), nullptr, &result->view), "Create 2D texture view");
    result->width = width;
    result->height = height;
    result->device = device_.Get();
    return result;
}

TextureHandle Renderer::WhiteTexture()
{
    return whiteHandle_;
}

Float2 TextureSize(const TextureHandle& texture)
{
    if (!texture) return {0, 0};
    return {static_cast<float>(texture->width), static_cast<float>(texture->height)};
}

TextureHandle Renderer::UploadImage(const std::filesystem::path& file)
{
    if (!device_) throw std::runtime_error("Initialize the renderer before uploading images.");
    const auto size = std::filesystem::file_size(file);
    if (size == 0 || size > 64u * 1024 * 1024) throw std::runtime_error("Image file is empty or too large.");
    std::vector<BYTE> bytes(static_cast<std::size_t>(size));
    {
        std::ifstream in(file, std::ios::binary);
        if (!in || !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
            throw std::runtime_error("Cannot read image file: " + file.string());
    }
    ComPtr<IWICImagingFactory> factory;
    ThrowIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
                  "Create image decoder");
    ComPtr<IWICStream> stream;
    ThrowIfFailed(factory->CreateStream(&stream), "Create image stream");
    ThrowIfFailed(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size())), "Open image bytes");
    ComPtr<IWICBitmapDecoder> decoder;
    ThrowIfFailed(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder),
                  "Decode image (PNG, JPEG, BMP, TIFF or GIF)");
    ComPtr<IWICBitmapFrameDecode> frame;
    ThrowIfFailed(decoder->GetFrame(0, &frame), "Read image frame");
    UINT width = 0, height = 0;
    ThrowIfFailed(frame->GetSize(&width, &height), "Read image size");
    if (!width || !height || width > 32768 || height > 32768) throw std::runtime_error("Invalid image dimensions.");
    const double scale = std::min(1.0, 2048.0 / std::max(width, height));
    width = std::max(1u, static_cast<UINT>(width * scale));
    height = std::max(1u, static_cast<UINT>(height * scale));
    ComPtr<IWICBitmapScaler> scaler;
    ThrowIfFailed(factory->CreateBitmapScaler(&scaler), "Create image scaler");
    ThrowIfFailed(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeLinear), "Scale image");
    ComPtr<IWICFormatConverter> converter;
    ThrowIfFailed(factory->CreateFormatConverter(&converter), "Create image converter");
    ThrowIfFailed(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                        0.0, WICBitmapPaletteTypeCustom), "Convert image to RGBA");
    std::vector<BYTE> pixels(static_cast<std::size_t>(width) * height * 4);
    ThrowIfFailed(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()),
                  "Read image pixels");
    return UploadTexture(width, height, pixels.data());
}

std::vector<std::uint8_t> DecodeImageFileRGBA(const std::wstring& file, unsigned& width, unsigned& height)
{
    ComPtr<IWICImagingFactory> factory;
    ThrowIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
                  "Create image decoder");
    ComPtr<IWICBitmapDecoder> decoder;
    ThrowIfFailed(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, &decoder), "Open image file");
    ComPtr<IWICBitmapFrameDecode> frame;
    ThrowIfFailed(decoder->GetFrame(0, &frame), "Read image frame");
    UINT w = 0, h = 0;
    ThrowIfFailed(frame->GetSize(&w, &h), "Read image size");
    if (!w || !h || w > 8192 || h > 8192) throw std::runtime_error("Image is empty or larger than 8192 px.");
    ComPtr<IWICFormatConverter> converter;
    ThrowIfFailed(factory->CreateFormatConverter(&converter), "Create image converter");
    ThrowIfFailed(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                        0.0, WICBitmapPaletteTypeCustom), "Convert image to RGBA");
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4);
    ThrowIfFailed(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data()), "Read image pixels");
    width = w;
    height = h;
    return pixels;
}

MaterialHandle Renderer::UploadMaterial(TextureHandle albedo, Float4 tint, bool lit, float roughness, float specular)
{
    auto material = std::make_shared<RenderMaterial>();
    material->albedo = std::move(albedo);
    material->tint = tint;
    material->lit = lit;
    material->roughness = roughness;
    material->specular = specular;
    return material;
}

zengine::Vec2 Renderer::MeasureText(std::string_view text, float pixelHeight)
{
    if (!fontAtlas_.Valid())
    {
        fontAtlas_ = FontAtlas::Build(32);
        fontTexture_ = UploadTexture(static_cast<std::uint32_t>(fontAtlas_.Width()),
                                     static_cast<std::uint32_t>(fontAtlas_.Height()),
                                     fontAtlas_.Pixels().data());
    }
    return {fontAtlas_.Measure(text, pixelHeight), pixelHeight};
}

void Renderer::RenderSprites(const ViewportFrame& frame)
{
    if (frame.sprites.empty() && frame.texts.empty() && !frame.fps) return;

    std::vector<SpriteDraw> draws;
    draws.reserve(frame.sprites.size() + 64);

    if ((!frame.texts.empty() || frame.fps) && !fontAtlas_.Valid())
    {
        fontAtlas_ = FontAtlas::Build(32);
        fontTexture_ = UploadTexture(static_cast<std::uint32_t>(fontAtlas_.Width()),
                                     static_cast<std::uint32_t>(fontAtlas_.Height()),
                                     fontAtlas_.Pixels().data());
    }
    for (const auto& text : frame.texts)
    {
        auto glyphs = fontAtlas_.Layout(text.text, text.x, text.y, text.pixelHeight, text.color, fontTexture_);
        for (auto& glyph : glyphs) glyph.clip = text.clip;
        draws.insert(draws.end(), glyphs.begin(), glyphs.end());
    }
    if (frame.fps)
    {
        const std::string label = "FPS " + std::to_string(*frame.fps);
        const float height = 16;
        const float width = fontAtlas_.Measure(label, height);
        auto glyphs = fontAtlas_.Layout(label, static_cast<float>(width_) - width - 8, 8, height,
                                        Float4{0.92f, 0.95f, 1.0f, 1.0f}, fontTexture_);
        draws.insert(draws.end(), glyphs.begin(), glyphs.end());
    }
    draws.insert(draws.end(), frame.sprites.begin(), frame.sprites.end());
    if (draws.empty()) return;

    SpriteConstants constants{};
    constants.inverseScreen = {1.0f / static_cast<float>(width_), 1.0f / static_cast<float>(height_)};
    D3D11_MAPPED_SUBRESOURCE mappedConstants{};
    ThrowIfFailed(context_->Map(spriteConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedConstants), "Map sprite constants");
    *static_cast<SpriteConstants*>(mappedConstants.pData) = constants;
    context_->Unmap(spriteConstantBuffer_.Get(), 0);

    constexpr UINT stride = sizeof(SpriteVertex);
    constexpr UINT offset = 0;
    const float blendFactor[4]{0, 0, 0, 0};
    context_->IASetInputLayout(spriteInputLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(spriteVertexShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, spriteConstantBuffer_.GetAddressOf());
    context_->PSSetShader(spritePixelShader_.Get(), nullptr, 0);
    context_->PSSetSamplers(0, 1, spriteSampler_.GetAddressOf());
    context_->OMSetBlendState(spriteBlend_.Get(), blendFactor, 0xffffffff);
    context_->OMSetDepthStencilState(overlayDepth_.Get(), 0);
    context_->RSSetState(rasterizer_.Get());

    std::vector<SpriteVertex> batch;
    batch.reserve(1024);
    const RenderTexture* currentTexture = nullptr;
    bool haveTexture = false;

    const auto flush = [&] {
        if (batch.empty()) return;
        if (batch.size() > kSpriteVertexCapacity) throw std::runtime_error("Sprite batch overflowed the vertex buffer.");
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(context_->Map(spriteVertexBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map sprite vertices");
        std::memcpy(mapped.pData, batch.data(), batch.size() * sizeof(SpriteVertex));
        context_->Unmap(spriteVertexBuffer_.Get(), 0);
        ID3D11ShaderResourceView* view = (currentTexture ? currentTexture : whiteHandle_.get())->view.Get();
        context_->PSSetShaderResources(0, 1, &view);
        ID3D11Buffer* buffer = spriteVertexBuffer_.Get();
        context_->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
        context_->Draw(static_cast<UINT>(batch.size()), 0);
        batch.clear();
    };

    for (const auto& draw : draws)
    {
        const RenderTexture* texture = draw.texture ? draw.texture.get() : whiteHandle_.get();
        if (texture && texture->device != device_.Get())
            throw std::runtime_error("Sprite texture belongs to another render device.");
        if (haveTexture && texture != currentTexture) flush();
        currentTexture = texture;
        haveTexture = true;
        const float tw = texture ? static_cast<float>(texture->width) : 1.0f;
        const float th = texture ? static_cast<float>(texture->height) : 1.0f;
        if (batch.size() + 54 > kSpriteVertexCapacity) flush();
        AppendSprite(batch, draw, tw, th);
        ++lastSpriteCount_;
    }
    flush();

    ID3D11ShaderResourceView* noTexture = nullptr;
    context_->PSSetShaderResources(0, 1, &noTexture);
    context_->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    context_->OMSetDepthStencilState(nullptr, 0);
}

MeshHandle Renderer::UploadModel(const ModelData& model, std::vector<std::string>& warnings)
{
    if (!device_) throw std::runtime_error("Initialize the renderer before uploading meshes.");
    if (model.vertices.empty() || model.indices.empty() || model.parts.empty())
        throw std::runtime_error("Model contains no drawable geometry.");
    if (model.vertices.size() > UINT_MAX / sizeof(Vertex) || model.indices.size() > UINT_MAX / sizeof(std::uint32_t))
        throw std::runtime_error("Model is too large for a GPU buffer.");
    Float3 minimum = model.vertices.front().position;
    Float3 maximum = minimum;
    for (const auto& vertex : model.vertices)
    {
        for (float value : {vertex.position.x,vertex.position.y,vertex.position.z,
             vertex.normal.x,vertex.normal.y,vertex.normal.z,vertex.color.x,vertex.color.y,vertex.color.z,vertex.uv.x,vertex.uv.y})
            if (!std::isfinite(value)) throw std::runtime_error("Model contains nonfinite vertex data.");
        minimum.x = std::min(minimum.x, vertex.position.x); maximum.x = std::max(maximum.x, vertex.position.x);
        minimum.y = std::min(minimum.y, vertex.position.y); maximum.y = std::max(maximum.y, vertex.position.y);
        minimum.z = std::min(minimum.z, vertex.position.z); maximum.z = std::max(maximum.z, vertex.position.z);
    }
    const float extent = std::max({maximum.x - minimum.x, maximum.y - minimum.y, maximum.z - minimum.z});
    if (!std::isfinite(extent) || extent < 1.0e-12f) throw std::runtime_error("Model has invalid or zero-sized bounds.");
    for (const auto index : model.indices)
        if (index >= model.vertices.size()) throw std::runtime_error("Model contains an invalid vertex index.");
    for (const auto& part : model.parts)
        if (!part.indexCount || part.indexCount % 3 != 0 || part.material >= model.materials.size() || part.firstIndex > model.indices.size() ||
            part.indexCount > model.indices.size() - part.firstIndex)
            throw std::runtime_error("Model contains an invalid material or mesh range.");

    ComPtr<ID3D11Buffer> vertices, indices;
    D3D11_BUFFER_DESC description{};
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    description.ByteWidth = static_cast<UINT>(model.vertices.size() * sizeof(Vertex));
    const D3D11_SUBRESOURCE_DATA vertexData{model.vertices.data(), 0, 0};
    ThrowIfFailed(device_->CreateBuffer(&description, &vertexData, &vertices), "Upload model vertices");
    description.BindFlags = D3D11_BIND_INDEX_BUFFER;
    description.ByteWidth = static_cast<UINT>(model.indices.size() * sizeof(std::uint32_t));
    const D3D11_SUBRESOURCE_DATA indexData{model.indices.data(), 0, 0};
    ThrowIfFailed(device_->CreateBuffer(&description, &indexData, &indices), "Upload model indices");

    warnings.clear();
    std::vector<ComPtr<ID3D11ShaderResourceView>> textures(model.materials.size());
    std::uint64_t textureBytes = 0;
    for (std::size_t index = 0; index < model.materials.size(); ++index)
    {
        const auto& image = model.materials[index].image;
        if (image.empty()) continue;
        try
        {
            ComPtr<IWICImagingFactory> factory;
            ThrowIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&factory)), "Create image decoder");
            ComPtr<IWICStream> stream;
            ThrowIfFailed(factory->CreateStream(&stream), "Create image stream");
            ThrowIfFailed(stream->InitializeFromMemory(const_cast<BYTE*>(image.data()), static_cast<DWORD>(image.size())),
                          "Read albedo image");
            ComPtr<IWICBitmapDecoder> decoder;
            ThrowIfFailed(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder),
                          "Decode albedo (PNG, JPEG, BMP, TIFF or GIF required)");
            ComPtr<IWICBitmapFrameDecode> frame;
            ThrowIfFailed(decoder->GetFrame(0, &frame), "Read albedo frame");
            UINT width = 0, height = 0;
            ThrowIfFailed(frame->GetSize(&width, &height), "Read albedo dimensions");
            if (!width || !height || width > 32768 || height > 32768)
                throw std::runtime_error("Invalid or oversized image dimensions.");
            // Bound GPU allocations on low-spec machines. Keep the original image in the project.
            const double scale = std::min(1.0, 2048.0 / std::max(width, height));
            width = std::max(1u, static_cast<UINT>(width * scale));
            height = std::max(1u, static_cast<UINT>(height * scale));
            textureBytes += static_cast<std::uint64_t>(width) * height * 4;
            if (textureBytes > 128 * 1024 * 1024) throw std::runtime_error("Preview texture budget exceeded (128 MB).");
            ComPtr<IWICBitmapScaler> scaler;
            ThrowIfFailed(factory->CreateBitmapScaler(&scaler), "Create albedo scaler");
            ThrowIfFailed(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeLinear), "Scale albedo");
            ComPtr<IWICFormatConverter> converter;
            ThrowIfFailed(factory->CreateFormatConverter(&converter), "Create albedo converter");
            ThrowIfFailed(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom), "Convert albedo to RGBA");
            std::vector<BYTE> pixels(static_cast<std::size_t>(width) * height * 4);
            ThrowIfFailed(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()),
                          "Read albedo pixels");
            D3D11_TEXTURE2D_DESC textureDescription{};
            textureDescription.Width = width;
            textureDescription.Height = height;
            textureDescription.MipLevels = textureDescription.ArraySize = 1;
            textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            textureDescription.SampleDesc.Count = 1;
            textureDescription.Usage = D3D11_USAGE_IMMUTABLE;
            textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA pixelsData{pixels.data(), width * 4, 0};
            ComPtr<ID3D11Texture2D> texture;
            ThrowIfFailed(device_->CreateTexture2D(&textureDescription, &pixelsData, &texture), "Upload albedo");
            ThrowIfFailed(device_->CreateShaderResourceView(texture.Get(), nullptr, &textures[index]), "Create albedo view");
        }
        catch (const std::exception& issue)
        {
            warnings.emplace_back(issue.what()); // Missing/bad images use material color, not a failed scene load.
        }
    }
    auto mesh = std::make_shared<RenderMesh>();
    mesh->device = device_.Get();
    mesh->vertices = std::move(vertices);
    mesh->indices = std::move(indices);
    mesh->parts = model.parts;
    mesh->textures = std::move(textures);
    return mesh;
}

void Renderer::Render(const ViewportFrame& frame)
{
    lastMeshCount_ = 0;
    lastSpriteCount_ = 0;
    if (!renderTargetView_ || width_ == 0 || height_ == 0)
    {
        return;
    }

    const ViewportCamera camera(static_cast<float>(width_),static_cast<float>(height_),frame.camera);
    const float aspect=static_cast<float>(width_)/std::max(1.0f,static_cast<float>(height_));
    XMMATRIX view=camera.view,projection=camera.projection;
    if (frame.gameView) // look through a Camera GameObject instead of the orbit camera
    {
        const auto& gv=*frame.gameView;
        XMMATRIX world=TransformMatrix(gv.transform);
        if (gv.parentMatrix) world=world*XMLoadFloat4x4(&*gv.parentMatrix);
        if (std::abs(XMVectorGetX(XMMatrixDeterminant(world)))>1e-12f) view=XMMatrixInverse(nullptr,world);
        projection=XMMatrixPerspectiveFovLH(XMConvertToRadians(std::clamp(gv.fovY,1.0f,179.0f)),aspect,
                                            std::max(0.001f,gv.nearZ),std::max(gv.nearZ+0.01f,gv.farZ));
    }

    // ZE-74: the scene renders unlit unless there is at least one light.
    const int lightCount = std::min<int>(8, static_cast<int>(frame.lights.size()));
    {
        LightConstants lc{};
        for (int i = 0; i < lightCount; ++i)
        {
            const auto& l = frame.lights[static_cast<std::size_t>(i)];
            const float dl = std::sqrt(l.direction.x * l.direction.x + l.direction.y * l.direction.y + l.direction.z * l.direction.z);
            const float inv = dl > 1e-6f ? 1.0f / dl : 0.0f;
            lc.lights[i].posType = {l.position.x, l.position.y, l.position.z, static_cast<float>(l.type)};
            lc.lights[i].dirRange = {l.direction.x * inv, l.direction.y * inv, l.direction.z * inv, std::max(0.01f, l.range)};
            lc.lights[i].colorIntensity = {l.color.x * l.intensity, l.color.y * l.intensity, l.color.z * l.intensity, 0};
            lc.lights[i].spot = {l.spotCosInner, l.spotCosOuter, std::max(0.1f, l.falloff), l.fogScatter};
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        ThrowIfFailed(context_->Map(lightConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map light constants");
        *static_cast<LightConstants*>(m.pData) = lc;
        context_->Unmap(lightConstantBuffer_.Get(), 0);
    }

    // ZE-75: world camera position + the scene's Environment (fog) for the pixel shader.
    XMFLOAT3 cameraWorld{0, 0, 0};
    XMStoreFloat3(&cameraWorld, XMMatrixInverse(nullptr, view).r[3]);
    const EnvironmentData env = frame.environment.value_or(EnvironmentData{});

    // ZE-112: one directional shadow map for the brightest directional light.
    int shadowCaster = -1;
    float shadowBrightness = 0;
    for (int i = 0; i < lightCount; ++i)
    {
        const auto& l = frame.lights[static_cast<std::size_t>(i)];
        if (l.type != 0) continue;
        const float b = l.intensity * (l.color.x + l.color.y + l.color.z);
        if (b > shadowBrightness) { shadowBrightness = b; shadowCaster = i; }
    }
    XMMATRIX shadowMatrix = XMMatrixIdentity();
    if (shadowCaster >= 0 && !frame.meshes.empty())
    {
        XMFLOAT3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (const auto& d : frame.meshes)
        {
            XMFLOAT3 p;
            XMStoreFloat3(&p, XMVector3TransformCoord(XMVectorZero(),
                TransformMatrix(d.transform) * (d.parentMatrix ? XMLoadFloat4x4(&*d.parentMatrix) : XMMatrixIdentity())));
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        const XMVECTOR centre = XMVectorSet((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f, 1);
        float radius = 4.0f;
        radius = std::max(radius, 0.5f * std::sqrt((hi.x-lo.x)*(hi.x-lo.x) + (hi.y-lo.y)*(hi.y-lo.y) + (hi.z-lo.z)*(hi.z-lo.z)) + 6.0f);
        const auto& L = frame.lights[static_cast<std::size_t>(shadowCaster)];
        XMVECTOR dir = XMVector3Normalize(XMVectorSet(L.direction.x, L.direction.y, L.direction.z, 0));
        if (XMVectorGetX(XMVector3LengthSq(dir)) < 0.5f) dir = XMVectorSet(0, -1, 0, 0);
        XMVECTOR up = std::abs(XMVectorGetY(dir)) > 0.95f ? XMVectorSet(1, 0, 0, 0) : XMVectorSet(0, 1, 0, 0);
        const XMMATRIX lightView = XMMatrixLookToLH(centre - dir * (radius * 2.0f), dir, up);
        const XMMATRIX lightProj = XMMatrixOrthographicLH(radius * 2.0f, radius * 2.0f, 0.05f, radius * 4.0f);
        shadowMatrix = lightView * lightProj;

        // Depth-only pass into the shadow map.
        D3D11_VIEWPORT sv{0, 0, 2048.0f, 2048.0f, 0.0f, 1.0f};
        context_->OMSetRenderTargets(0, nullptr, shadowDsv_.Get());
        context_->ClearDepthStencilView(shadowDsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        context_->RSSetViewports(1, &sv);
        context_->RSSetState(rasterizer_.Get());
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(shadowVertexShader_.Get(), nullptr, 0);
        context_->VSSetConstantBuffers(0, 1, sceneConstantBuffer_.GetAddressOf());
        context_->PSSetShader(nullptr, nullptr, 0);
        context_->OMSetDepthStencilState(nullptr, 0);
        constexpr UINT stride0 = static_cast<UINT>(sizeof(Vertex)), offset0 = 0;
        for (const auto& d : frame.meshes)
        {
            if (!d.mesh) continue;
            SceneConstants sc{};
            const XMMATRIX w = TransformMatrix(d.transform) * (d.parentMatrix ? XMLoadFloat4x4(&*d.parentMatrix) : XMMatrixIdentity());
            XMStoreFloat4x4(&sc.worldViewProjection, XMMatrixTranspose(w * shadowMatrix));
            D3D11_MAPPED_SUBRESOURCE m{};
            ThrowIfFailed(context_->Map(sceneConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map shadow constants");
            *static_cast<SceneConstants*>(m.pData) = sc;
            context_->Unmap(sceneConstantBuffer_.Get(), 0);
            context_->IASetVertexBuffers(0, 1, d.mesh->vertices.GetAddressOf(), &stride0, &offset0);
            context_->IASetIndexBuffer(d.mesh->indices.Get(), d.mesh->indexFormat, 0);
            for (const auto& part : d.mesh->parts) context_->DrawIndexed(part.indexCount, part.firstIndex, 0);
        }
        // The main pass below rebinds render targets, viewport and shaders.
    }
    const float shadowStrength = shadowCaster >= 0 ? 0.85f : 0.0f;

    const auto setConstants = [&](const XMMATRIX& matrix, bool lit, Float4 tint = {1, 1, 1, 1}, Float2 spec = {0.5f, 0.0f}) {
        SceneConstants constants{};
        XMStoreFloat4x4(&constants.worldViewProjection, XMMatrixTranspose(matrix * view * projection));
        XMStoreFloat4x4(&constants.world, XMMatrixTranspose(matrix));
        const float determinant = XMVectorGetX(XMMatrixDeterminant(matrix));
        const XMMATRIX normals = std::abs(determinant) > 1.0e-20f
            ? XMMatrixTranspose(XMMatrixInverse(nullptr, matrix)) : XMMatrixIdentity();
        XMStoreFloat4x4(&constants.normalWorld, XMMatrixTranspose(normals));
        constants.materialTint = XMFLOAT4{tint.x, tint.y, tint.z, tint.w};
        constants.ambient = XMFLOAT4{0.10f, 0.10f, 0.12f, (lit && lightCount > 0) ? 1.0f : 0.0f};
        constants.lightParams = XMFLOAT4{static_cast<float>((lit && lightCount > 0) ? lightCount : 0), 0, 0, 0};
        constants.cameraPos = XMFLOAT4{cameraWorld.x, cameraWorld.y, cameraWorld.z, 0};
        constants.fogColorMode = XMFLOAT4{env.fogColor.x, env.fogColor.y, env.fogColor.z, static_cast<float>(env.fogMode)};
        constants.fogParams = XMFLOAT4{env.fogNear, env.fogFar, env.fogDensity, static_cast<float>(env.volumetricSteps)};
        constants.heightFog = XMFLOAT4{env.heightBase, env.heightFalloff, env.heightStrength, env.volumetric ? 1.0f : 0.0f};
        constants.materialSpec = XMFLOAT4{spec.x, spec.y, 0, 0};
        XMStoreFloat4x4(&constants.shadowMatrix, XMMatrixTranspose(shadowMatrix));
        constants.shadowParams = XMFLOAT4{(lit && lightCount > 0) ? shadowStrength : 0.0f, 1.0f / 2048.0f, 0.0015f, static_cast<float>(shadowCaster)};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(context_->Map(sceneConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map scene constants");
        *static_cast<SceneConstants*>(mapped.pData) = constants;
        context_->Unmap(sceneConstantBuffer_.Get(), 0);
    };

    constexpr float clearColor[]{0.025f, 0.035f, 0.055f, 1.0f};
    context_->ClearRenderTargetView(renderTargetView_.Get(), clearColor);
    context_->ClearDepthStencilView(depthStencilView_.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    context_->OMSetRenderTargets(1, renderTargetView_.GetAddressOf(), depthStencilView_.Get());
    context_->RSSetViewports(1, &viewport_);
    context_->RSSetState(rasterizer_.Get());
    context_->OMSetDepthStencilState(nullptr, 0);

    constexpr UINT stride = static_cast<UINT>(sizeof(Vertex));
    constexpr UINT offset = 0;
    context_->IASetInputLayout(inputLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, sceneConstantBuffer_.GetAddressOf());
    context_->VSSetConstantBuffers(1, 1, lightConstantBuffer_.GetAddressOf()); // ZE-74 lights
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, sceneConstantBuffer_.GetAddressOf()); // ZE-65: material tint read in PSMain
    context_->PSSetConstantBuffers(1, 1, lightConstantBuffer_.GetAddressOf()); // ZE-75: volumetric fog reads the lights
    context_->PSSetSamplers(0, 1, albedoSampler_.GetAddressOf());
    context_->PSSetShaderResources(1, 1, shadowSrv_.GetAddressOf()); // ZE-112 shadow map
    context_->PSSetSamplers(1, 1, shadowSampler_.GetAddressOf());
    const auto drawLines = [&](ID3D11Buffer* buffer, UINT count, const XMMATRIX& matrix) {
        setConstants(matrix, false); // editor guides are always unlit
        context_->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        context_->PSSetShaderResources(0, 1, whiteTexture_.GetAddressOf());
        context_->Draw(count, 0);
    };
    if (frame.showEditorGuides) drawLines(gridBuffer_.Get(), gridVertexCount_, XMMatrixIdentity());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (const auto& draw : frame.meshes)
    {
        if (!draw.mesh) continue;
        const auto& mesh = *draw.mesh;
        if (mesh.device != device_.Get()) throw std::runtime_error("Mesh belongs to another render device.");
        const RenderMaterial* material = draw.material.get();
        const Float4 tint = material ? material->tint : Float4{1, 1, 1, 1};
        const bool meshLit = draw.lit && (!material || material->lit);
        const Float2 spec = material ? Float2{material->roughness, material->specular} : Float2{0.5f, 0.0f};
        setConstants(TransformMatrix(draw.transform)*(draw.parentMatrix?XMLoadFloat4x4(&*draw.parentMatrix):XMMatrixIdentity()), meshLit, tint, spec);
        context_->IASetVertexBuffers(0, 1, mesh.vertices.GetAddressOf(), &stride, &offset);
        context_->IASetIndexBuffer(mesh.indices.Get(), mesh.indexFormat, 0);
        for (const auto& part : mesh.parts)
        {
            ID3D11ShaderResourceView* texture =
                material && material->albedo ? material->albedo->view.Get()
              : part.material < mesh.textures.size() && mesh.textures[part.material] ? mesh.textures[part.material].Get()
              : whiteTexture_.Get();
            context_->PSSetShaderResources(0, 1, &texture);
            context_->DrawIndexed(part.indexCount, part.firstIndex, 0);
        }
        ++lastMeshCount_;
    }
    RenderDecals(frame, view * projection); // ZE-76: projected decals over the lit meshes
    if(frame.showEditorGuides && (!frame.colliders.empty() || !frame.cameraGizmos.empty() || !frame.audioRanges.empty() || !frame.lightGizmos.empty())) {
        std::vector<Vertex> vertices;vertices.reserve(frame.colliders.size()*160+frame.cameraGizmos.size()*48+frame.audioRanges.size()*288+frame.lightGizmos.size()*160);const auto segment=[&](Float3 a,Float3 b,Float3 color){vertices.push_back({a,{0,1,0},color});vertices.push_back({b,{0,1,0},color});};
        for(const auto& lg:frame.lightGizmos){
            const Float3 p{lg.position.x,lg.position.y,lg.position.z};
            const Float3 col=lg.selected?Float3{1,.8f,.15f}:Float3{std::max(.3f,lg.color.x),std::max(.3f,lg.color.y),std::max(.3f,lg.color.z)};
            for(int a=0;a<3;++a){Float3 e0=p,e1=p;(&e0.x)[a]-=0.3f;(&e1.x)[a]+=0.3f;segment(e0,e1,col);} // position star
            const float dl=std::sqrt(lg.direction.x*lg.direction.x+lg.direction.y*lg.direction.y+lg.direction.z*lg.direction.z);
            const Float3 dir=dl>1e-4f?Float3{lg.direction.x/dl,lg.direction.y/dl,lg.direction.z/dl}:Float3{0,0,1};
            if(lg.type==0){ // directional: an arrow along the beam
                const Float3 tip{p.x+dir.x*1.6f,p.y+dir.y*1.6f,p.z+dir.z*1.6f};
                segment(p,tip,col);
            } else if(lg.type==1){ // point: a range sphere
                constexpr int slices=24;const float r=lg.range;
                for(int plane=0;plane<3;++plane)for(int i=0;i<slices;++i){const float a0=6.2831853f*i/slices,b0=6.2831853f*(i+1)/slices;auto pt=[&](float ang){const float c=std::cos(ang)*r,s=std::sin(ang)*r;return plane==0?Float3{p.x+c,p.y+s,p.z}:plane==1?Float3{p.x+c,p.y,p.z+s}:Float3{p.x,p.y+c,p.z+s};};segment(pt(a0),pt(b0),col);}
            } else { // spot: a cone to the outer angle
                const float len=lg.range;const float rad=std::tan(lg.spotOuterDeg*3.14159265f/180.0f)*len;
                Float3 up=std::abs(dir.y)<0.95f?Float3{0,1,0}:Float3{1,0,0};
                Float3 rt{up.y*dir.z-up.z*dir.y,up.z*dir.x-up.x*dir.z,up.x*dir.y-up.y*dir.x};
                const float rl=std::sqrt(rt.x*rt.x+rt.y*rt.y+rt.z*rt.z);rt={rt.x/rl,rt.y/rl,rt.z/rl};
                Float3 u2{dir.y*rt.z-dir.z*rt.y,dir.z*rt.x-dir.x*rt.z,dir.x*rt.y-dir.y*rt.x};
                const Float3 centre{p.x+dir.x*len,p.y+dir.y*len,p.z+dir.z*len};
                constexpr int slices=20;Float3 prev{};
                for(int i=0;i<=slices;++i){const float ang=6.2831853f*i/slices,c=std::cos(ang)*rad,s=std::sin(ang)*rad;
                    Float3 rim{centre.x+rt.x*c+u2.x*s,centre.y+rt.y*c+u2.y*s,centre.z+rt.z*c+u2.z*s};
                    if(i>0)segment(prev,rim,col); if(i%5==0)segment(p,rim,col); prev=rim;}
            }
        }
        for(const auto& range:frame.audioRanges){
            const auto world=TransformMatrix(range.transform)*(range.parentMatrix?XMLoadFloat4x4(&*range.parentMatrix):XMMatrixIdentity());
            XMFLOAT3 wp;XMStoreFloat3(&wp,XMVector3TransformCoord(XMVectorSet(0,0,0,1),world));
            const Float3 centre{wp.x,wp.y,wp.z};
            const auto sphere=[&](float radius,Float3 color){
                if(radius<=0.0001f)return;
                constexpr int slices=32;
                for(int plane=0;plane<3;++plane)for(int i=0;i<slices;++i){
                    const float a=2*3.14159265f*i/slices,b=2*3.14159265f*(i+1)/slices;
                    auto pt=[&](float ang){const float c=std::cos(ang)*radius,s=std::sin(ang)*radius;
                        return plane==0?Float3{centre.x+c,centre.y+s,centre.z}
                              :plane==1?Float3{centre.x+c,centre.y,centre.z+s}
                                       :Float3{centre.x,centre.y+c,centre.z+s};};
                    segment(pt(a),pt(b),color);
                }
            };
            const Float3 outer=range.selected?Float3{1,.8f,.15f}:Float3{.35f,.7f,.95f};
            sphere(range.maxDistance,outer);
            sphere(range.minDistance,Float3{outer.x*0.6f,outer.y*0.6f,outer.z*0.6f});
        }
        for(const auto& cam:frame.cameraGizmos){
            const auto world=TransformMatrix(cam.transform)*(cam.parentMatrix?XMLoadFloat4x4(&*cam.parentMatrix):XMMatrixIdentity());
            const auto point=[&](float x,float y,float z){XMFLOAT3 o;XMStoreFloat3(&o,XMVector3TransformCoord(XMVectorSet(x,y,z,1),world));return Float3{o.x,o.y,o.z};};
            const float tv=std::tan(XMConvertToRadians(std::clamp(cam.fovY,1.0f,179.0f))*0.5f),th=tv*aspect;
            const float n=std::max(0.001f,cam.nearZ),f=std::max(n+0.05f,std::min(cam.farZ,n+25.0f)); // clamp the drawn far plane so the gizmo stays readable
            Float3 c[8];for(int i=0;i<8;++i){const float z=(i<4)?n:f,sx=(i&1)?1.f:-1.f,sy=(i&2)?1.f:-1.f;c[i]=point(sx*z*th,sy*z*tv,z);}
            const Float3 color=cam.selected?Float3{1,.8f,.15f}:cam.main?Float3{.35f,.75f,1.f}:Float3{.55f,.6f,.7f};
            const int e[][2]={{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}};
            for(auto edge:e)segment(c[edge[0]],c[edge[1]],color);
            const Float3 apex=point(0,0,0);for(int i=0;i<4;++i)segment(apex,c[i],color); // lens cone back to the origin
        }
        for(const auto& draw:frame.colliders){const Float3 color=draw.selected?Float3{1,.8f,.15f}:draw.decal?Float3{.95f,.55f,.25f}:draw.audioZone?Float3{.55f,.4f,.9f}:Float3{.2f,.85f,.65f};const auto transform=XMMatrixScaling(draw.size.x,draw.size.y,draw.size.z)*XMMatrixTranslation(draw.offset.x,draw.offset.y,draw.offset.z)*TransformMatrix(draw.transform)*(draw.parentMatrix?XMLoadFloat4x4(&*draw.parentMatrix):XMMatrixIdentity());
            const auto point=[&](float x,float y,float z){XMFLOAT3 out;XMStoreFloat3(&out,XMVector3TransformCoord(XMVectorSet(x,y,z,1),transform));return Float3{out.x,out.y,out.z};};
            if(draw.shape==zengine::physics::ColliderShape::Box){const int edges[][2]={{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}};Float3 p[8];for(int i=0;i<8;++i)p[i]=point((i&1)?.5f:-.5f,(i&4)?.5f:-.5f,(i&2)?.5f:-.5f);for(auto edge:edges)segment(p[edge[0]],p[edge[1]],color);}
            else {constexpr int slices=24;const float radius=.5f;const float half=draw.shape==zengine::physics::ColliderShape::Sphere?0:.5f;for(int plane=0;plane<3;++plane)for(int i=0;i<slices;++i){const float a=2*3.14159265f*i/slices,b=2*3.14159265f*(i+1)/slices;auto circle=[&](float angle){const float c=std::cos(angle)*radius,s=std::sin(angle)*radius;if(draw.shape==zengine::physics::ColliderShape::Sphere)return plane==0?point(c,s,0):plane==1?point(c,0,s):point(0,c,s);return plane==0?point(c,s+(s>=0?half:-half),0):point(c,s>=0?half:-half,s);};segment(circle(a),circle(b),color);} }
        }
        if(vertices.size()>65536)throw std::runtime_error("Collider guide buffer overflow.");D3D11_MAPPED_SUBRESOURCE mapped{};ThrowIfFailed(context_->Map(colliderBuffer_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map collider guides");std::memcpy(mapped.pData,vertices.data(),vertices.size()*sizeof(Vertex));context_->Unmap(colliderBuffer_.Get(),0);drawLines(colliderBuffer_.Get(),static_cast<UINT>(vertices.size()),XMMatrixIdentity());
    }
    if (frame.showEditorGuides && frame.selectionTransform)
    {
        const auto parent=frame.selectionParent?XMLoadFloat4x4(&*frame.selectionParent):XMMatrixIdentity();
        auto localCamera=camera; localCamera.view=parent*camera.view;
        const auto shape=gizmo::Build(localCamera,*frame.selectionTransform,frame.tool);
        if (shape.lines.size()*2>1024) throw std::runtime_error("Transform handle buffer overflow.");
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(context_->Map(axesBuffer_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map transform handles");
        auto* vertices=static_cast<Vertex*>(mapped.pData);
        const Float3 colors[]={{1,.2f,.2f},{.2f,1,.3f},{.2f,.5f,1}};
        for (const auto& segment:shape.lines)
        {
            const auto color=segment.axis==frame.highlightedAxis?Float3{1,.85f,.15f}:colors[segment.axis];
            for (const auto p:{segment.a,segment.b}) *vertices++=Vertex{{p.x,p.y,p.z},{0,1,0},color};
        }
        context_->Unmap(axesBuffer_.Get(),0);
        axesVertexCount_=static_cast<UINT>(shape.lines.size()*2);
        context_->OMSetDepthStencilState(overlayDepth_.Get(), 0);
        drawLines(axesBuffer_.Get(), axesVertexCount_, parent);
    }
    // 2D / UI overlay (sprites + text + the FPS readout), in screen pixels.
    RenderSprites(frame);

    // Do not let pipeline bindings keep an otherwise released model alive in an empty scene.
    ID3D11Buffer* noBuffer = nullptr;
    ID3D11ShaderResourceView* noTexture = nullptr;
    context_->IASetVertexBuffers(0, 1, &noBuffer, &stride, &offset);
    context_->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
    context_->PSSetShaderResources(0, 1, &noTexture);
    ThrowIfFailed(swapChain_->Present(1, 0), "Present frame");
}

std::filesystem::path Renderer::ShaderFile(const wchar_t* name) const
{
    std::array<wchar_t, 32768> executablePath{};
    const DWORD length = GetModuleFileNameW(nullptr, executablePath.data(),
                                            static_cast<DWORD>(executablePath.size()));
    if (length == 0 || length == static_cast<DWORD>(executablePath.size()))
    {
        throw std::runtime_error("Could not determine the executable path.");
    }

    const std::filesystem::path shaderPath =
        std::filesystem::path(executablePath.data()).parent_path() / "shaders" / name;
    if (!std::filesystem::exists(shaderPath))
    {
        throw std::runtime_error("Shader file was not found: " + shaderPath.string());
    }
    return shaderPath;
}

std::filesystem::path Renderer::FindShaderPath() const
{
    return ShaderFile(L"ColorCube.hlsl");
}
