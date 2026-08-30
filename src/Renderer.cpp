#include "Renderer.h"

#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <array>
#include <cstddef>
#include <random>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    void ThrowIfFailed(const HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(std::string(operation) + " failed (HRESULT 0x" +
                                     std::to_string(static_cast<unsigned long>(result)) + ").");
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

struct Renderer::Vertex
{
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT3 color;
};

struct Renderer::SceneConstants
{
    XMFLOAT4X4 worldViewProjection;
    XMFLOAT4X4 world;
    XMFLOAT4 lightDirection;
};

void Renderer::Initialize(HWND window, const std::uint32_t width, const std::uint32_t height)
{
    CreateDeviceAndSwapChain(window, width, height);
    CreateRenderTargets(width, height);
    CreateShaders();
    CreateCube();
    startTime_ = std::chrono::steady_clock::now();
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
    };
    ThrowIfFailed(device_->CreateInputLayout(inputElements.data(), static_cast<UINT>(inputElements.size()),
                                              vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
                                              &inputLayout_),
                  "Create input layout");
}

void Renderer::CreateCube()
{
    std::mt19937 randomEngine(std::random_device{}());
    std::uniform_real_distribution<float> colorChannel(0.2f, 1.0f);
    const auto randomColor = [&]() {
        return XMFLOAT3{colorChannel(randomEngine), colorChannel(randomEngine), colorChannel(randomEngine)};
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
    ThrowIfFailed(device_->CreateBuffer(&vertexDescription, &vertexData, &vertexBuffer_),
                  "Create cube vertex buffer");

    D3D11_BUFFER_DESC indexDescription{};
    indexDescription.ByteWidth = static_cast<UINT>(sizeof(indices));
    indexDescription.Usage = D3D11_USAGE_IMMUTABLE;
    indexDescription.BindFlags = D3D11_BIND_INDEX_BUFFER;
    const D3D11_SUBRESOURCE_DATA indexData{indices.data(), 0, 0};
    ThrowIfFailed(device_->CreateBuffer(&indexDescription, &indexData, &indexBuffer_),
                  "Create cube index buffer");

    D3D11_BUFFER_DESC constantDescription{};
    constantDescription.ByteWidth = static_cast<UINT>(sizeof(SceneConstants));
    constantDescription.Usage = D3D11_USAGE_DYNAMIC;
    constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ThrowIfFailed(device_->CreateBuffer(&constantDescription, nullptr, &sceneConstantBuffer_),
                  "Create scene constant buffer");
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

void Renderer::Render()
{
    if (!renderTargetView_ || width_ == 0 || height_ == 0)
    {
        return;
    }

    const float elapsedSeconds = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - startTime_).count();
    const XMMATRIX world = XMMatrixRotationY(elapsedSeconds) * XMMatrixRotationX(elapsedSeconds * 0.55f);
    const XMMATRIX view = XMMatrixLookAtLH(
        XMVectorSet(0.0f, 1.5f, -5.0f, 1.0f), XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(60.0f), static_cast<float>(width_) / static_cast<float>(height_), 0.1f, 100.0f);

    SceneConstants constants{};
    XMStoreFloat4x4(&constants.worldViewProjection, XMMatrixTranspose(world * view * projection));
    XMStoreFloat4x4(&constants.world, XMMatrixTranspose(world));
    constants.lightDirection = XMFLOAT4{-0.4f, -0.8f, 0.5f, 0.0f};

    D3D11_MAPPED_SUBRESOURCE mapped{};
    ThrowIfFailed(context_->Map(sceneConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                  "Map scene constants");
    *static_cast<SceneConstants*>(mapped.pData) = constants;
    context_->Unmap(sceneConstantBuffer_.Get(), 0);

    constexpr float clearColor[]{0.025f, 0.035f, 0.055f, 1.0f};
    context_->ClearRenderTargetView(renderTargetView_.Get(), clearColor);
    context_->ClearDepthStencilView(depthStencilView_.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    context_->OMSetRenderTargets(1, renderTargetView_.GetAddressOf(), depthStencilView_.Get());
    context_->RSSetViewports(1, &viewport_);

    constexpr UINT stride = static_cast<UINT>(sizeof(Vertex));
    constexpr UINT offset = 0;
    context_->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
    context_->IASetIndexBuffer(indexBuffer_.Get(), DXGI_FORMAT_R16_UINT, 0);
    context_->IASetInputLayout(inputLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, sceneConstantBuffer_.GetAddressOf());
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->DrawIndexed(36, 0, 0);

    ThrowIfFailed(swapChain_->Present(1, 0), "Present frame");
}

std::filesystem::path Renderer::FindShaderPath() const
{
    std::array<wchar_t, 32768> executablePath{};
    const DWORD length = GetModuleFileNameW(nullptr, executablePath.data(),
                                            static_cast<DWORD>(executablePath.size()));
    if (length == 0 || length == static_cast<DWORD>(executablePath.size()))
    {
        throw std::runtime_error("Could not determine the executable path.");
    }

    const std::filesystem::path shaderPath =
        std::filesystem::path(executablePath.data()).parent_path() / "shaders" / "ColorCube.hlsl";
    if (!std::filesystem::exists(shaderPath))
    {
        throw std::runtime_error("Shader file was not found: " + shaderPath.string());
    }
    return shaderPath;
}
