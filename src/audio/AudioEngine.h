#pragma once

#include "audio/AudioClip.h"

#include <cstdint>
#include <memory>

// ZE-67: the playback backend. A thin wrapper over XAudio2 - one mastering voice
// and a pool of source voices. 3D positioning is handled by the caller (it
// computes a distance gain and calls SetGain); this class only mixes.
//
// If XAudio2 fails to initialise (headless CI, no audio device) every call is a
// safe no-op and Ready() is false; the rest of the engine keeps running silently.
namespace zengine::audio
{
    using VoiceId = std::uint32_t; // 0 == invalid

    class AudioEngine
    {
    public:
        AudioEngine();
        ~AudioEngine();
        AudioEngine(const AudioEngine&) = delete;
        AudioEngine& operator=(const AudioEngine&) = delete;

        bool Ready() const noexcept;

        // Begins playing `clip` (kept alive until the voice ends). `gain` is a linear
        // 0..1 multiplier, `pitch` a positive frequency ratio (1 = original). Returns
        // 0 on failure.
        VoiceId Play(std::shared_ptr<const Clip> clip, float gain, float pitch, bool loop);

        void SetGain(VoiceId voice, float gain);
        void SetPitch(VoiceId voice, float pitch);
        void Stop(VoiceId voice);

        // Still producing sound (not stopped, and for a one-shot not yet finished).
        bool Active(VoiceId voice) const;
        // Sample frames consumed since the voice started (wraps naturally past the
        // clip length while looping).
        std::uint64_t PlayCursorFrames(VoiceId voice) const;

        // Reclaims finished one-shot voices. Call once per frame.
        void Pump();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
