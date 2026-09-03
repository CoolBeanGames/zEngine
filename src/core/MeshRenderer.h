#pragma once
#include "GameObject.h"

namespace zengine
{
    // Native behavior data only. The render system resolves assets and owns GPU resources.
    // Empty asset = no mesh; imported paths are relative to the project's Assets directory.
    class MeshRenderer final : public Behavior
    {
    public:
        static constexpr const char* CubeAsset = "builtin:cube";
        explicit MeshRenderer(ObjectCore& owner, std::string asset = {})
            : Behavior(owner), asset_(std::move(asset)) {}
        const std::string& Asset() const noexcept { return asset_; }
        void SetAsset(std::string asset) { asset_ = std::move(asset); }
        // ZE-65: project-relative ".material" asset. Empty = the renderer's default
        // (the model's own imported albedo, unlit-tinted white).
        const std::string& Material() const noexcept { return material_; }
        void SetMaterial(std::string material) { material_ = std::move(material); }
    private:
        std::string asset_;
        std::string material_;
    };
}
