#include "audio/AudioEngine.h"

#include <windows.h>
#include <xaudio2.h>
#include <wrl/client.h>

#include <algorithm>
#include <map>

#pragma comment(lib, "xaudio2.lib")

namespace zengine::audio
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        float ClampGain(float g) { return std::clamp(g, 0.0f, 1.0f); }
        float ClampPitch(float p) { return std::clamp(p, XAUDIO2_MIN_FREQ_RATIO, 4.0f); }
    }

    struct AudioEngine::Impl
    {
        ComPtr<IXAudio2> xaudio;
        IXAudio2MasteringVoice* master = nullptr;
        bool comInitialised = false;

        struct Voice
        {
            IXAudio2SourceVoice* source = nullptr;
            std::shared_ptr<const Clip> clip;
            bool loop = false;
        };
        std::map<VoiceId, Voice> voices;
        VoiceId nextId = 1;

        ~Impl()
        {
            for (auto& [id, v] : voices)
                if (v.source) { v.source->Stop(); v.source->DestroyVoice(); }
            voices.clear();
            if (master) master->DestroyVoice();
            xaudio.Reset();
            if (comInitialised) CoUninitialize();
        }

        void Destroy(Voice& v)
        {
            if (v.source) { v.source->Stop(0); v.source->FlushSourceBuffers(); v.source->DestroyVoice(); v.source = nullptr; }
            v.clip.reset();
        }
    };

    AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>())
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        impl_->comInitialised = SUCCEEDED(co);
        if (FAILED(XAudio2Create(impl_->xaudio.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR)))
        {
            impl_->xaudio.Reset();
            return;
        }
        if (FAILED(impl_->xaudio->CreateMasteringVoice(&impl_->master)))
        {
            impl_->master = nullptr;
            impl_->xaudio.Reset();
        }
    }

    AudioEngine::~AudioEngine() = default;

    bool AudioEngine::Ready() const noexcept { return impl_->xaudio && impl_->master; }

    VoiceId AudioEngine::Play(std::shared_ptr<const Clip> clip, float gain, float pitch, bool loop)
    {
        if (!Ready() || !clip || !clip->Valid()) return 0;

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = static_cast<WORD>(clip->channels);
        format.nSamplesPerSec = clip->sampleRate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        IXAudio2SourceVoice* source = nullptr;
        if (FAILED(impl_->xaudio->CreateSourceVoice(&source, &format, 0, 4.0f))) return 0;

        XAUDIO2_BUFFER buffer{};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = static_cast<UINT32>(clip->samples.size() * sizeof(float));
        buffer.pAudioData = reinterpret_cast<const BYTE*>(clip->samples.data());
        buffer.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;

        if (FAILED(source->SubmitSourceBuffer(&buffer)) ||
            FAILED(source->SetVolume(ClampGain(gain))) ||
            FAILED(source->SetFrequencyRatio(ClampPitch(pitch))) ||
            FAILED(source->Start(0)))
        {
            source->DestroyVoice();
            return 0;
        }

        const VoiceId id = impl_->nextId++;
        impl_->voices[id] = Impl::Voice{source, std::move(clip), loop};
        return id;
    }

    void AudioEngine::SetGain(VoiceId voice, float gain)
    {
        if (const auto it = impl_->voices.find(voice); it != impl_->voices.end() && it->second.source)
            it->second.source->SetVolume(ClampGain(gain));
    }

    void AudioEngine::SetPitch(VoiceId voice, float pitch)
    {
        if (const auto it = impl_->voices.find(voice); it != impl_->voices.end() && it->second.source)
            it->second.source->SetFrequencyRatio(ClampPitch(pitch));
    }

    void AudioEngine::Stop(VoiceId voice)
    {
        if (const auto it = impl_->voices.find(voice); it != impl_->voices.end())
        {
            impl_->Destroy(it->second);
            impl_->voices.erase(it);
        }
    }

    bool AudioEngine::Active(VoiceId voice) const
    {
        const auto it = impl_->voices.find(voice);
        if (it == impl_->voices.end() || !it->second.source) return false;
        if (it->second.loop) return true;
        XAUDIO2_VOICE_STATE state{};
        it->second.source->GetState(&state);
        return state.BuffersQueued > 0;
    }

    std::uint64_t AudioEngine::PlayCursorFrames(VoiceId voice) const
    {
        const auto it = impl_->voices.find(voice);
        if (it == impl_->voices.end() || !it->second.source) return 0;
        XAUDIO2_VOICE_STATE state{};
        it->second.source->GetState(&state);
        return state.SamplesPlayed;
    }

    void AudioEngine::Pump()
    {
        for (auto it = impl_->voices.begin(); it != impl_->voices.end();)
        {
            if (it->second.loop || !it->second.source) { ++it; continue; }
            XAUDIO2_VOICE_STATE state{};
            it->second.source->GetState(&state);
            if (state.BuffersQueued == 0) { impl_->Destroy(it->second); it = impl_->voices.erase(it); }
            else ++it;
        }
    }
}
