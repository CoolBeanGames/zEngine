#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <memory>
#include <string_view>
#include <vector>
#include "ModelData.h"
#include "core/GameObject.h"
#include "RenderScene.h"
#include "FontAtlas.h"

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
    // ZE-77: meshes skipped by view-frustum culling in the last Render().
    std::size_t LastCulledMeshCount() const noexcept { return lastCulledCount_; }

    // Self-contained 2D texture path (ZE-60): RGBA8, row-major, `width * height * 4`
    // bytes. Independent of the future material / material-instance systems.
    TextureHandle UploadTexture(std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba);
    TextureHandle WhiteTexture();
    // ZE-65: decode an image file (PNG/JPEG/BMP/TIFF/GIF) to a sampleable texture,
    // and bundle a resolved Material Instance (albedo texture + tint).
    TextureHandle UploadImage(const std::filesystem::path& file);
    MaterialHandle UploadMaterial(TextureHandle albedo, Float4 tint, bool lit = true, float roughness = 0.5f, float specular = 0.0f);
    std::size_t LastSpriteCount() const noexcept { return lastSpriteCount_; }
    // Text extent in screen pixels, using the shared UI font atlas (built on first use).
    zengine::Vec2 MeasureText(std::string_view text, float pixelHeight);
    zengine::Vec2 ViewportSize() const noexcept { return {static_cast<float>(width_), static_cast<float>(height_)}; }

private:
    using Vertex = MeshVertex;
    struct SceneConstants;
    struct LightConstants;
    struct SpriteConstants;
    struct DecalConstants;

    void CreateDeviceAndSwapChain(HWND window, std::uint32_t width, std::uint32_t height);
    void CreateRenderTargets(std::uint32_t width, std::uint32_t height);
    void CreateShaders();
    void CreateCube();
    void CreateEditorGuides();
    void CreateSpritePass();
    void CreateDecalPass();
    void RenderDecals(const ViewportFrame& frame, const DirectX::XMMATRIX& viewProjection);
    void RenderSprites(const ViewportFrame& frame);
    [[nodiscard]] std::filesystem::path FindShaderPath() const;
    [[nodiscard]] std::filesystem::path ShaderFile(const wchar_t* name) const;

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
    Microsoft::WRL::ComPtr<ID3D11Buffer> lightConstantBuffer_; // ZE-74
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> whiteTexture_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> albedoSampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> gridBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> axesBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> colliderBuffer_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> overlayDepth_;
    // ZE-112: single directional shadow map.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> shadowTexture_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> shadowDsv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shadowSrv_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> shadowSampler_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> shadowVertexShader_;
    void CreateShadowResources();
    // ZE-76: projected decal pass.
    Microsoft::WRL::ComPtr<ID3D11VertexShader> decalVertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> decalPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> decalConstantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> decalBlend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> decalDepth_;
    UINT gridVertexCount_ = 0;
    UINT axesVertexCount_ = 0;
    MeshHandle cube_;
    std::size_t lastMeshCount_ = 0;
    std::size_t lastCulledCount_ = 0;

    // 2D sprite / UI pass.
    Microsoft::WRL::ComPtr<ID3D11VertexShader> spriteVertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> spritePixelShader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> spriteInputLayout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> spriteVertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> spriteConstantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> spriteBlend_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> spriteSampler_;
    TextureHandle whiteHandle_;
    TextureHandle fontTexture_;
    FontAtlas fontAtlas_;
    std::size_t lastSpriteCount_ = 0;
    static constexpr UINT kSpriteVertexCapacity = 24576;

    D3D11_VIEWPORT viewport_{};
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};
