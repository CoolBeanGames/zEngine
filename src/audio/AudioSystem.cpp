#include "audio/AudioSystem.h"

#include <algorithm>
#include <cmath>

namespace zengine::audio
{
    namespace
    {
        float Distance(Vec3 a, Vec3 b)
        {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }

    const char* AttenuationName(Attenuation a)
    {
        switch (a) { case Attenuation::None: return "none"; case Attenuation::Inverse: return "inverse"; default: return "linear"; }
    }
    bool ParseAttenuation(std::string_view name, Attenuation& out)
    {
        if (name == "none") { out = Attenuation::None; return true; }
        if (name == "linear") { out = Attenuation::Linear; return true; }
        if (name == "inverse") { out = Attenuation::Inverse; return true; }
        return false;
    }

    float DistanceGain(Attenuation model, float distance, float minDistance, float maxDistance)
    {
        if (model == Attenuation::None) return 1.0f;
        const float lo = std::max(0.0f, minDistance);
        const float hi = std::max(lo + 0.01f, maxDistance);
        if (distance <= lo) return 1.0f;
        if (distance >= hi) return 0.0f;
        if (model == Attenuation::Linear)
            return 1.0f - (distance - lo) / (hi - lo);
        // Inverse: full at lo, ~1/d falloff, forced to 0 by hi.
        const float inv = lo / distance;
        const float fade = 1.0f - (distance - lo) / (hi - lo); // taper to 0 at hi
        return std::clamp(inv * fade, 0.0f, 1.0f);
    }

    AudioSystem::AudioSystem(bool enableDevice) : device_(enableDevice) {}

    void AudioSystem::SetClipLoader(std::function<Clip(const std::string&)> loader)
    {
        loader_ = std::move(loader);
        clips_.clear();
    }

    std::shared_ptr<const Clip> AudioSystem::ClipFor(const std::string& path)
    {
        if (path.empty() || !loader_) return nullptr;
        if (const auto it = clips_.find(path); it != clips_.end()) return it->second;
        std::shared_ptr<const Clip> clip;
        try { clip = std::make_shared<const Clip>(loader_(path)); }
        catch (...) { clip = nullptr; }
        clips_.emplace(path, clip);
        return clip;
    }

    void AudioSystem::StopVoice(AudioSource& source)
    {
        const auto id = source.Owner().Id();
        if (const auto it = playing_.find(id); it != playing_.end())
        {
            if (it->second.engineVoice) engine_.Stop(it->second.engineVoice);
            playing_.erase(it);
        }
        source.voice = 0;
    }

    void AudioSystem::StopAll()
    {
        for (auto& [id, p] : playing_) if (p.engineVoice) engine_.Stop(p.engineVoice);
        playing_.clear();
        autoplayed_.clear();
        started_.clear(); looped_.clear(); finished_.clear();
    }

    void AudioSystem::Update(ObjectStore& objects, float dt)
    {
        started_.clear(); looped_.clear(); finished_.clear();

        for (std::size_t i = 0; i < objects.Size(); ++i)
        {
            auto& object = objects.At(i);
            auto* source = object.GetBehavior<AudioSource>();
            if (!source) continue;
            const auto id = object.Id();

            if (source->takeStop()) StopVoice(*source);

            const bool wantsAutoplay = source->Autoplay() && !autoplayed_[id];
            const std::uint32_t requests = source->requestCount();
            source->clearRequests();

            const bool enabled = source->Enabled();
            if (enabled && (requests > 0 || wantsAutoplay))
            {
                StopVoice(*source);
                autoplayed_[id] = true;
                if (auto clip = ClipFor(source->Clip()); clip && clip->Valid())
                {
                    Playing p;
                    p.clip = clip;
                    p.loop = source->Loop();
                    Vec3 pos{};
                    if (source->Spatial()) if (auto* g = As3D(&object)) pos = g->GetTransform().Position();
                    const float dist = Distance(pos, listener_);
                    const float gain = source->Volume() *
                        (source->Spatial() ? DistanceGain(source->AttenuationModel(), dist,
                                                          source->MinDistance(), source->MaxDistance()) : 1.0f);
                    p.engineVoice = device_ ? engine_.Play(clip, gain, source->Pitch(), p.loop) : 0;
                    playing_[id] = p;
                    source->voice = 1;
                    started_.push_back(id);
                }
            }

            const auto pit = playing_.find(id);
            if (pit == playing_.end()) { source->voice = 0; continue; }
            Playing& p = pit->second;
            const std::uint64_t clipFrames = p.clip ? p.clip->Frames() : 0;
            const std::uint32_t rate = p.clip ? p.clip->sampleRate : 44100;

            Vec3 pos{};
            if (source->Spatial()) if (auto* g = As3D(&object)) pos = g->GetTransform().Position();
            const float dist = Distance(pos, listener_);
            const float gain = source->Volume() *
                (source->Spatial() ? DistanceGain(source->AttenuationModel(), dist,
                                                  source->MinDistance(), source->MaxDistance()) : 1.0f);

            std::uint64_t cursor = 0;
            bool active = true;
            if (p.engineVoice && engine_.Ready())
            {
                engine_.SetGain(p.engineVoice, gain);
                engine_.SetPitch(p.engineVoice, source->Pitch());
                cursor = engine_.PlayCursorFrames(p.engineVoice);
                active = engine_.Active(p.engineVoice);
            }
            else // no device: advance a logical clock so events still fire
            {
                p.logicalFrames += static_cast<double>(dt) * rate * source->Pitch();
                cursor = static_cast<std::uint64_t>(p.logicalFrames);
                active = p.loop || (clipFrames && cursor < clipFrames);
            }

            if (p.loop && clipFrames)
            {
                const std::uint64_t loopIndex = cursor / clipFrames;
                if (loopIndex > p.lastLoopIndex) { looped_.push_back(id); p.lastLoopIndex = loopIndex; }
            }

            if (!active)
            {
                if (p.engineVoice) engine_.Stop(p.engineVoice);
                playing_.erase(pit);
                source->voice = 0;
                finished_.push_back(id);
            }
        }

        engine_.Pump();
    }
}
