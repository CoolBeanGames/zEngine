#pragma once
#include "GameObject.h"
#include <algorithm>
#include <string>
#include <string_view>

namespace zengine
{
    // ZE-74: a light on a 3D GameObject. Native data only - the renderer reads
    // these plus the owner's transform (position, and forward for the direction).
    // No lighting for 2D. With zero lights in a scene, rendering stays unlit.
    class Light final : public Behavior
    {
    public:
        enum class Type { Directional, Point, Spot };

        explicit Light(ObjectCore& owner) : Behavior(owner) {}

        Type LightType() const noexcept { return type_; }
        void SetLightType(Type value) noexcept { type_ = value; }

        Vec3 Color() const noexcept { return color_; }
        void SetColor(Vec3 value) noexcept
        {
            color_ = {std::clamp(value.x, 0.0f, 1.0f), std::clamp(value.y, 0.0f, 1.0f), std::clamp(value.z, 0.0f, 1.0f)};
        }

        float Intensity() const noexcept { return intensity_; }
        void SetIntensity(float value) noexcept { intensity_ = value < 0 ? 0.0f : value > 32 ? 32.0f : value; }

        // Point / spot: distance at which the light reaches zero.
        float Range() const noexcept { return range_; }
        void SetRange(float value) noexcept { range_ = value < 0.01f ? 0.01f : value; }

        // Distance falloff exponent (1 = gentle, 2 = inverse-square-ish).
        float Falloff() const noexcept { return falloff_; }
        void SetFalloff(float value) noexcept { falloff_ = std::clamp(value, 0.1f, 8.0f); }

        // Spot cone half-angles in degrees; full brightness within inner, zero past outer.
        float SpotInner() const noexcept { return spotInner_; }
        float SpotOuter() const noexcept { return spotOuter_; }
        void SetSpotInner(float degrees) noexcept { spotInner_ = std::clamp(degrees, 0.0f, 89.0f); spotOuter_ = std::max(spotOuter_, spotInner_ + 0.5f); }
        void SetSpotOuter(float degrees) noexcept { spotOuter_ = std::clamp(degrees, 0.5f, 89.5f); spotInner_ = std::min(spotInner_, spotOuter_ - 0.5f); }

        // Static lights participate in the offline lightmap bake (ZE-74 baking).
        bool Static() const noexcept { return static_; }
        void SetStatic(bool value) noexcept { static_ = value; }

    private:
        Type type_ = Type::Directional;
        Vec3 color_{1, 1, 1};
        float intensity_ = 1.0f;
        float range_ = 10.0f;
        float falloff_ = 2.0f;
        float spotInner_ = 20.0f;
        float spotOuter_ = 35.0f;
        bool static_ = false;
    };

    inline const char* LightTypeName(Light::Type t)
    {
        switch (t) { case Light::Type::Point: return "point"; case Light::Type::Spot: return "spot"; default: return "directional"; }
    }
    inline bool ParseLightType(std::string_view name, Light::Type& out)
    {
        if (name == "directional") { out = Light::Type::Directional; return true; }
        if (name == "point") { out = Light::Type::Point; return true; }
        if (name == "spot") { out = Light::Type::Spot; return true; }
        return false;
    }
}
