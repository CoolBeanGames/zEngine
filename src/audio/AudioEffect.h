#pragma once

#include "core/GameObject.h"

#include <string>
#include <string_view>

// ZE-109: an audio effect zone. Lives on a GameObject that also has an Area +
// Collider (the engine's trigger volume). While a 3D AudioSource's emitter is
// inside that collider, AudioSystem routes its sound through this effect (reverb
// for now), blending dry <-> wet across `BlendDistance` at the boundary.
namespace zengine::audio
{
    enum class EffectKind { Reverb };

    const char* EffectKindName(EffectKind k);
    bool ParseEffectKind(std::string_view name, EffectKind& out);

    class AudioEffect final : public Behavior
    {
    public:
        explicit AudioEffect(ObjectCore& owner) : Behavior(owner) {}

        EffectKind Kind() const noexcept { return kind_; }
        void SetKind(EffectKind value) noexcept { kind_ = value; }

        // Reverb: decay time in seconds and how much of the wet (reverberated)
        // signal is mixed in inside the zone (0..1).
        float Decay() const noexcept { return decay_; }
        void SetDecay(float value) noexcept { decay_ = value < 0.1f ? 0.1f : value > 20.0f ? 20.0f : value; }
        float WetMix() const noexcept { return wetMix_; }
        void SetWetMix(float value) noexcept { wetMix_ = value < 0 ? 0.0f : value > 1 ? 1.0f : value; }

        // World-space fade band at the collider boundary (0 = hard switch).
        float BlendDistance() const noexcept { return blendDistance_; }
        void SetBlendDistance(float value) noexcept { blendDistance_ = value < 0 ? 0.0f : value; }

    private:
        EffectKind kind_ = EffectKind::Reverb;
        float decay_ = 1.5f;
        float wetMix_ = 0.6f;
        float blendDistance_ = 1.0f;
    };
}
