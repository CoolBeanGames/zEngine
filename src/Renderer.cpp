#include "Renderer.h"
#include "RenderTransform.h"

#include <DirectXMath.h>
#include <d3dcompiler.h>
#include <wincodec.h>

#include <array>
#include <cstddef>
#include <random>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

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
    XMFLOAT4X4 normalWorld;
    XMFLOAT4 lightDirection;
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
    CreateEditorGuides();
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
    const Float3 red{1, 0.2f, 0.2f}, green{0.2f, 1, 0.3f}, blue{0.2f, 0.5f, 1};
    line({0, 0, 0}, {1, 0, 0}, red); line({1, 0, 0}, {0.8f, 0.1f, 0}, red); line({1, 0, 0}, {0.8f, -0.1f, 0}, red);
    line({0, 0, 0}, {0, 1, 0}, green); line({0, 1, 0}, {0.1f, 0.8f, 0}, green); line({0, 1, 0}, {-0.1f, 0.8f, 0}, green);
    line({0, 0, 0}, {0, 0, 1}, blue); line({0, 0, 1}, {0.1f, 0, 0.8f}, blue); line({0, 0, 1}, {-0.1f, 0, 0.8f}, blue);
    upload(axesBuffer_, axesVertexCount_);
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    ThrowIfFailed(device_->CreateDepthStencilState(&depth, &overlayDepth_), "Create editor overlay depth state");
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
    if (!renderTargetView_ || width_ == 0 || height_ == 0)
    {
        return;
    }

    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const float halfFov = std::min(XM_PI / 6.0f, std::atan(std::tan(XM_PI / 6.0f) * aspect));
    const float distance = 1.75f / std::sin(halfFov) * 1.1f;
    const XMMATRIX view = XMMatrixLookAtLH(
        XMVectorSet(0.0f, distance * 0.28f, -distance, 1.0f), XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(60.0f), static_cast<float>(width_) / static_cast<float>(height_), 0.1f, 100.0f);

    const auto setConstants = [&](const XMMATRIX& matrix, bool unlit) {
        SceneConstants constants{};
        XMStoreFloat4x4(&constants.worldViewProjection, XMMatrixTranspose(matrix * view * projection));
        const float determinant = XMVectorGetX(XMMatrixDeterminant(matrix));
        const XMMATRIX normals = std::abs(determinant) > 1.0e-20f
            ? XMMatrixTranspose(XMMatrixInverse(nullptr, matrix)) : XMMatrixIdentity();
        XMStoreFloat4x4(&constants.normalWorld, XMMatrixTranspose(normals));
        constants.lightDirection = XMFLOAT4{-0.4f, -0.8f, 0.5f, unlit ? 1.0f : 0.0f};
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
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetSamplers(0, 1, albedoSampler_.GetAddressOf());
    const auto drawLines = [&](ID3D11Buffer* buffer, UINT count, const XMMATRIX& matrix) {
        setConstants(matrix, true);
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
        setConstants(TransformMatrix(draw.transform), false);
        context_->IASetVertexBuffers(0, 1, mesh.vertices.GetAddressOf(), &stride, &offset);
        context_->IASetIndexBuffer(mesh.indices.Get(), mesh.indexFormat, 0);
        for (const auto& part : mesh.parts)
        {
            ID3D11ShaderResourceView* texture = part.material < mesh.textures.size() && mesh.textures[part.material]
                ? mesh.textures[part.material].Get() : whiteTexture_.Get();
            context_->PSSetShaderResources(0, 1, &texture);
            context_->DrawIndexed(part.indexCount, part.firstIndex, 0);
        }
        ++lastMeshCount_;
    }
    if (frame.showEditorGuides && frame.selectionTransform)
    {
        context_->OMSetDepthStencilState(overlayDepth_.Get(), 0);
        drawLines(axesBuffer_.Get(), axesVertexCount_, TransformMatrix(*frame.selectionTransform));
    }

    // Do not let pipeline bindings keep an otherwise released model alive in an empty scene.
    ID3D11Buffer* noBuffer = nullptr;
    ID3D11ShaderResourceView* noTexture = nullptr;
    context_->IASetVertexBuffers(0, 1, &noBuffer, &stride, &offset);
    context_->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
    context_->PSSetShaderResources(0, 1, &noTexture);
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
