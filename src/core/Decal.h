#pragma once
#include "GameObject.h"
#include <algorithm>
#include <string>

namespace zengine
{
    // ZE-76: a projected decal. The object's transform is an oriented box (the unit
    // cube in local space); the texture is projected down the box's local -Z onto
    // whatever solid geometry falls inside it - bullet holes, scorch marks, blood.
    // 3D only. The renderer owns the texture; `texture_` is a project-relative image.
    class Decal final : public Behavior
    {
    public:
        explicit Decal(ObjectCore& owner, std::string texture = {})
            : Behavior(owner), texture_(std::move(texture)) {}

        const std::string& Texture() const noexcept { return texture_; }
        void SetTexture(std::string texture) { texture_ = std::move(texture); }

        Vec3 Tint() const noexcept { return tint_; }
        void SetTint(Vec3 tint) noexcept { tint_ = tint; }

        float Opacity() const noexcept { return opacity_; }
        void SetOpacity(float opacity) noexcept { opacity_ = std::clamp(opacity, 0.0f, 1.0f); }

        // Surfaces whose normal is more than this many degrees from facing the
        // projector fade out (avoids stretched projection onto grazing walls).
        float AngleFade() const noexcept { return angleFade_; }
        void SetAngleFade(float degrees) noexcept { angleFade_ = std::clamp(degrees, 1.0f, 90.0f); }

    private:
        std::string texture_;
        Vec3 tint_{1, 1, 1};
        float opacity_ = 1.0f;
        float angleFade_ = 75.0f;
    };
}
