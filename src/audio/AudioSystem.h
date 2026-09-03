#pragma once

#include "audio/AudioClip.h"
#include "audio/AudioEngine.h"
#include "audio/AudioSource.h"
#include "core/GameObject.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// ZE-67: drives every AudioSource in a scene. Owns the AudioEngine and a decoded
// clip cache, resolves 3D distance gain against a listener position, and reports
// started / looped / finished so the script layer can raise signals. Works with
// no audio device: a logical playback clock keeps loop / finish events flowing.
namespace zengine::audio
{
    class AudioSystem
    {
    public:
        // enableDevice=false forces the logical playback clock (tests / headless
        // tools): no XAudio2 voices are created and loop / finish still fire.
        explicit AudioSystem(bool enableDevice = true);

        // Resolves a project-relative clip path to a decoded Clip (throws on
        // failure). Unset => audio never starts.
        void SetClipLoader(std::function<Clip(const std::string&)> loader);
        void SetListener(Vec3 position) noexcept { listener_ = position; }
        bool DeviceReady() const { return engine_.Ready(); }

        // One frame. Honours Autoplay (once per source until StopAll), Play()/Stop()
        // requests, live volume / pitch / distance, and fills the event lists.
        void Update(ObjectStore& objects, float dt);

        const std::vector<GameObjectId>& Started() const noexcept { return started_; }
        const std::vector<GameObjectId>& Looped() const noexcept { return looped_; }
        const std::vector<GameObjectId>& Finished() const noexcept { return finished_; }

        // Stop every voice and forget autoplay state (scene change / leaving Play).
        void StopAll();

    private:
        struct Playing
        {
            std::shared_ptr<const Clip> clip;
            VoiceId engineVoice = 0;
            double logicalFrames = 0;   // used when the device is unavailable
            std::uint64_t lastLoopIndex = 0;
            bool loop = false;
        };
        std::shared_ptr<const Clip> ClipFor(const std::string& path);
        void StopVoice(AudioSource& source);

        AudioEngine engine_;
        std::function<Clip(const std::string&)> loader_;
        std::map<std::string, std::shared_ptr<const Clip>> clips_;
        std::map<GameObjectId, Playing> playing_;
        std::map<GameObjectId, bool> autoplayed_;
        Vec3 listener_{};
        bool device_ = true;
        VoiceId nextLocalId_ = 1;
        std::vector<GameObjectId> started_, looped_, finished_;
    };
}
