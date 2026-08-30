#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include "ModelData.h"

class Renderer final
{
public:
    Renderer() = default;
    ~Renderer() = default;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void Initialize(HWND window, std::uint32_t width, std::uint32_t height);
    void Resize(std::uint32_t width, std::uint32_t height);
    void Render();
    // Uploads transactionally: buffer failure leaves the existing preview intact.
    std::vector<std::string> SetModel(const ModelData& model);

private:
    using Vertex = MeshVertex;
    struct SceneConstants;

    void CreateDeviceAndSwapChain(HWND window, std::uint32_t width, std::uint32_t height);
    void CreateRenderTargets(std::uint32_t width, std::uint32_t height);
    void CreateShaders();
    void CreateCube();
    [[nodiscard]] std::filesystem::path FindShaderPath() const;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTargetView_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTexture_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthStencilView_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> sceneConstantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> whiteTexture_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> albedoSampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> albedoTextures_;
    std::vector<MeshPart> parts_{{0, 36, 0}};
    DXGI_FORMAT indexFormat_ = DXGI_FORMAT_R16_UINT;
    Float3 modelCenter_{};
    float modelScale_ = 1.0f;

    D3D11_VIEWPORT viewport_{};
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::chrono::steady_clock::time_point startTime_{};
};
