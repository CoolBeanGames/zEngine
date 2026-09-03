#pragma once

#include "core/GameObject.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

// ZE-67: an audio player attached to a GameObject (3D) or GameObject2D (2D).
// Native data only - AudioSystem owns the decoded clip and the playback voice.
namespace zengine::audio
{
    enum class Attenuation { None, Linear, Inverse };

    const char* AttenuationName(Attenuation a);
    bool ParseAttenuation(std::string_view name, Attenuation& out);

    // Distance gain multiplier (0..1). `min` = full volume within this radius,
    // `max` = silent beyond it. `None` ignores distance entirely.
    float DistanceGain(Attenuation model, float distance, float minDistance, float maxDistance);

    class AudioSource final : public Behavior
    {
    public:
        explicit AudioSource(ObjectCore& owner) : Behavior(owner) {}

        const std::string& Clip() const noexcept { return clip_; }
        void SetClip(std::string value) { clip_ = std::move(value); }

        bool Spatial() const noexcept { return spatial_; }              // true = 3D positional
        void SetSpatial(bool value) noexcept { spatial_ = value; }
        bool Autoplay() const noexcept { return autoplay_; }
        void SetAutoplay(bool value) noexcept { autoplay_ = value; }
        bool Loop() const noexcept { return loop_; }
        void SetLoop(bool value) noexcept { loop_ = value; }

        float Volume() const noexcept { return volume_; }
        void SetVolume(float value) noexcept { volume_ = value < 0 ? 0.0f : value > 1 ? 1.0f : value; }
        float Pitch() const noexcept { return pitch_; }
        void SetPitch(float value) noexcept { pitch_ = value < 0.05f ? 0.05f : value > 4.0f ? 4.0f : value; }

        Attenuation AttenuationModel() const noexcept { return attenuation_; }
        void SetAttenuationModel(Attenuation value) noexcept { attenuation_ = value; }
        float MinDistance() const noexcept { return minDistance_; }
        void SetMinDistance(float value) noexcept { minDistance_ = value < 0 ? 0.0f : value; }
        float MaxDistance() const noexcept { return maxDistance_; }
        void SetMaxDistance(float value) noexcept { maxDistance_ = value < 0.01f ? 0.01f : value; }

        // Runtime control (consumed by AudioSystem on the next Update).
        void Play() noexcept { playRequests_ += 1; }
        void Stop() noexcept { stopRequested_ = true; }

        // ----- AudioSystem-only runtime state -----
        std::uint32_t requestCount() const noexcept { return playRequests_; }
        void clearRequests() noexcept { playRequests_ = 0; }
        bool takeStop() noexcept { const bool s = stopRequested_; stopRequested_ = false; return s; }
        std::uint32_t voice = 0;
        std::uint64_t lastCursorFrames = 0;
        bool startedThisPlay = false;

    private:
        std::string clip_;
        bool spatial_ = true;
        bool autoplay_ = true;
        bool loop_ = false;
        float volume_ = 1.0f;
        float pitch_ = 1.0f;
        Attenuation attenuation_ = Attenuation::Linear;
        float minDistance_ = 1.0f;
        float maxDistance_ = 25.0f;
        std::uint32_t playRequests_ = 0;
        bool stopRequested_ = false;
    };
}
