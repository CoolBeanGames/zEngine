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
        explicit MeshRenderer(GameObject& owner, std::string asset = {})
            : Behavior(owner), asset_(std::move(asset)) {}
        const std::string& Asset() const noexcept { return asset_; }
        void SetAsset(std::string asset) { asset_ = std::move(asset); }
    private:
        std::string asset_;
    };
}
