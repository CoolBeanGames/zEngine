#pragma once
#include "GameObject.h"
#include <algorithm>
#include <string>
#include <string_view>

namespace zengine
{
    // ZE-75: scene atmosphere. Attach to any 3D GameObject (a settings object, the
    // camera...). The renderer uses the first enabled Environment it finds - fog,
    // height fog and a cheap volumetric term. No effect for 2D.
    class Environment final : public Behavior
    {
    public:
        enum class FogMode { Off, Linear, Exp2 };

        explicit Environment(ObjectCore& owner) : Behavior(owner) {}

        FogMode Fog() const noexcept { return fog_; }
        void SetFog(FogMode value) noexcept { fog_ = value; }

        Vec3 FogColor() const noexcept { return fogColor_; }
        void SetFogColor(Vec3 v) noexcept
        { fogColor_ = {std::clamp(v.x,0.0f,1.0f), std::clamp(v.y,0.0f,1.0f), std::clamp(v.z,0.0f,1.0f)}; }

        // Linear fog: full clear before `near`, full fog past `far`.
        float FogNear() const noexcept { return fogNear_; }
        float FogFar() const noexcept { return fogFar_; }
        void SetFogNear(float v) noexcept { fogNear_ = std::max(0.0f, v); fogFar_ = std::max(fogFar_, fogNear_ + 0.1f); }
        void SetFogFar(float v) noexcept { fogFar_ = std::max(0.1f, v); fogNear_ = std::min(fogNear_, fogFar_ - 0.1f); }

        // Exp2 fog: thickness per world unit.
        float FogDensity() const noexcept { return fogDensity_; }
        void SetFogDensity(float v) noexcept { fogDensity_ = std::clamp(v, 0.0f, 2.0f); }

        // Height fog: extra density that fades from `heightBase` upward over `heightFalloff`.
        float HeightBase() const noexcept { return heightBase_; }
        void SetHeightBase(float v) noexcept { heightBase_ = v; }
        float HeightFalloff() const noexcept { return heightFalloff_; }
        void SetHeightFalloff(float v) noexcept { heightFalloff_ = std::max(0.0f, v); }
        float HeightStrength() const noexcept { return heightStrength_; }
        void SetHeightStrength(float v) noexcept { heightStrength_ = std::clamp(v, 0.0f, 4.0f); }

        // Cheap volumetric scatter: a short march toward each fogScatter-enabled light.
        bool Volumetric() const noexcept { return volumetric_; }
        void SetVolumetric(bool v) noexcept { volumetric_ = v; }
        int VolumetricSteps() const noexcept { return volumetricSteps_; }
        void SetVolumetricSteps(int v) noexcept { volumetricSteps_ = std::clamp(v, 2, 16); }

    private:
        FogMode fog_ = FogMode::Off;
        Vec3 fogColor_{0.55f, 0.60f, 0.68f};
        float fogNear_ = 8.0f;
        float fogFar_ = 60.0f;
        float fogDensity_ = 0.03f;
        float heightBase_ = 0.0f;
        float heightFalloff_ = 6.0f;
        float heightStrength_ = 0.0f;
        bool volumetric_ = false;
        int volumetricSteps_ = 6;
    };

    inline const char* FogModeName(Environment::FogMode m)
    { return m == Environment::FogMode::Linear ? "linear" : m == Environment::FogMode::Exp2 ? "exp2" : "off"; }
    inline bool ParseFogMode(std::string_view name, Environment::FogMode& out)
    {
        if (name == "off") { out = Environment::FogMode::Off; return true; }
        if (name == "linear") { out = Environment::FogMode::Linear; return true; }
        if (name == "exp2") { out = Environment::FogMode::Exp2; return true; }
        return false;
    }
}
