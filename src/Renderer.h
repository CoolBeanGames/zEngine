#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <memory>
#include "ModelData.h"
#include "core/GameObject.h"
#include "RenderScene.h"

class Renderer final
{
public:
    Renderer() = default;
    ~Renderer() = default;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void Initialize(HWND window, std::uint32_t width, std::uint32_t height);
    void Resize(std::uint32_t width, std::uint32_t height);
    void Render(const ViewportFrame& frame = {});
    // Independent, immutable GPU resources. Upload failure cannot change existing objects.
    MeshHandle UploadModel(const ModelData& model, std::vector<std::string>& warnings);
    MeshHandle Cube() const noexcept { return cube_; }
    std::size_t LastMeshCount() const noexcept { return lastMeshCount_; }

private:
    using Vertex = MeshVertex;
    struct SceneConstants;

    void CreateDeviceAndSwapChain(HWND window, std::uint32_t width, std::uint32_t height);
    void CreateRenderTargets(std::uint32_t width, std::uint32_t height);
    void CreateShaders();
    void CreateCube();
    void CreateEditorGuides();
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
    Microsoft::WRL::ComPtr<ID3D11Buffer> sceneConstantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> whiteTexture_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> albedoSampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> gridBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> axesBuffer_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> overlayDepth_;
    UINT gridVertexCount_ = 0;
    UINT axesVertexCount_ = 0;
    MeshHandle cube_;
    std::size_t lastMeshCount_ = 0;

    D3D11_VIEWPORT viewport_{};
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};
