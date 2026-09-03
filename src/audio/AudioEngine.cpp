#include "audio/AudioEngine.h"

#include <windows.h>
#include <xaudio2.h>
#include <xaudio2fx.h>
#include <wrl/client.h>

#include <algorithm>
#include <map>
#include <vector>

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
            unsigned channels = 1;
        };
        std::map<VoiceId, Voice> voices;
        VoiceId nextId = 1;

        IXAudio2SubmixVoice* reverb = nullptr;
        Microsoft::WRL::ComPtr<IUnknown> reverbApo;
        bool reverbEnabled = false;

        bool EnsureReverb()
        {
            if (reverb) return true;
            if (!xaudio || !master) return false;
            if (FAILED(XAudio2CreateReverb(reverbApo.GetAddressOf(), 0))) return false;
            const XAUDIO2_EFFECT_DESCRIPTOR desc{reverbApo.Get(), TRUE, 2};
            const XAUDIO2_EFFECT_CHAIN chain{1, const_cast<XAUDIO2_EFFECT_DESCRIPTOR*>(&desc)};
            XAUDIO2_VOICE_DETAILS md{};
            master->GetVoiceDetails(&md);
            if (FAILED(xaudio->CreateSubmixVoice(&reverb, 2, md.InputSampleRate, 0, 0, nullptr, &chain)))
            { reverb = nullptr; reverbApo.Reset(); return false; }
            ApplyReverb(1.5f, 0.6f);
            reverb->DisableEffect(0);
            return true;
        }

        // Fills the reverb APO with sensible defaults, overriding decay + wet mix.
        void ApplyReverb(float decay, float wetMix)
        {
            if (!reverb) return;
            XAUDIO2FX_REVERB_PARAMETERS p{};
            p.WetDryMix = std::clamp(wetMix, 0.0f, 1.0f) * 100.0f;
            p.ReflectionsDelay = XAUDIO2FX_REVERB_DEFAULT_REFLECTIONS_DELAY;
            p.ReverbDelay = XAUDIO2FX_REVERB_DEFAULT_REVERB_DELAY;
            p.RearDelay = XAUDIO2FX_REVERB_DEFAULT_REAR_DELAY;
            p.PositionLeft = XAUDIO2FX_REVERB_DEFAULT_POSITION;
            p.PositionRight = XAUDIO2FX_REVERB_DEFAULT_POSITION;
            p.PositionMatrixLeft = XAUDIO2FX_REVERB_DEFAULT_POSITION_MATRIX;
            p.PositionMatrixRight = XAUDIO2FX_REVERB_DEFAULT_POSITION_MATRIX;
            p.EarlyDiffusion = XAUDIO2FX_REVERB_DEFAULT_EARLY_DIFFUSION;
            p.LateDiffusion = XAUDIO2FX_REVERB_DEFAULT_LATE_DIFFUSION;
            p.LowEQGain = XAUDIO2FX_REVERB_DEFAULT_LOW_EQ_GAIN;
            p.LowEQCutoff = XAUDIO2FX_REVERB_DEFAULT_LOW_EQ_CUTOFF;
            p.HighEQGain = XAUDIO2FX_REVERB_DEFAULT_HIGH_EQ_GAIN;
            p.HighEQCutoff = XAUDIO2FX_REVERB_DEFAULT_HIGH_EQ_CUTOFF;
            p.RoomFilterFreq = XAUDIO2FX_REVERB_DEFAULT_ROOM_FILTER_FREQ;
            p.RoomFilterMain = XAUDIO2FX_REVERB_DEFAULT_ROOM_FILTER_MAIN;
            p.RoomFilterHF = XAUDIO2FX_REVERB_DEFAULT_ROOM_FILTER_HF;
            p.ReflectionsGain = XAUDIO2FX_REVERB_DEFAULT_REFLECTIONS_GAIN;
            p.ReverbGain = XAUDIO2FX_REVERB_DEFAULT_REVERB_GAIN;
            p.DecayTime = std::clamp(decay, 0.1f, 20.0f);
            p.Density = XAUDIO2FX_REVERB_DEFAULT_DENSITY;
            p.RoomSize = XAUDIO2FX_REVERB_DEFAULT_ROOM_SIZE;
            reverb->SetEffectParameters(0, &p, sizeof(p));
        }

        ~Impl()
        {
            for (auto& [id, v] : voices)
                if (v.source) { v.source->Stop(); v.source->DestroyVoice(); }
            voices.clear();
            if (reverb) { reverb->DestroyVoice(); reverb = nullptr; }
            reverbApo.Reset();
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
        if (impl_->EnsureReverb() && impl_->reverb)
        {
            XAUDIO2_SEND_DESCRIPTOR sends[2]{{0, impl_->master}, {0, impl_->reverb}};
            const XAUDIO2_VOICE_SENDS sendList{2, sends};
            if (FAILED(impl_->xaudio->CreateSourceVoice(&source, &format, 0, 4.0f, nullptr, &sendList))) source = nullptr;
        }
        if (!source && FAILED(impl_->xaudio->CreateSourceVoice(&source, &format, 0, 4.0f))) return 0;

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
        impl_->voices[id] = Impl::Voice{source, clip, loop, clip->channels};
        // Start fully dry - the reverb send is opened per frame by SetReverbSend.
        if (impl_->reverb)
        {
            std::vector<float> zero(static_cast<std::size_t>(clip->channels) * 2, 0.0f);
            source->SetOutputMatrix(impl_->reverb, clip->channels, 2, zero.data());
        }
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

    void AudioEngine::SetReverbEnabled(bool enabled)
    {
        if (!enabled)
        {
            if (impl_->reverb && impl_->reverbEnabled) { impl_->reverb->DisableEffect(0); impl_->reverbEnabled = false; }
            return;
        }
        if (!impl_->EnsureReverb()) return;
        if (!impl_->reverbEnabled) { impl_->reverb->EnableEffect(0); impl_->reverbEnabled = true; }
    }

    void AudioEngine::SetReverbParams(float decay, float wetMix)
    {
        if (!impl_->EnsureReverb()) return;
        impl_->ApplyReverb(decay, wetMix);
    }

    void AudioEngine::SetReverbSend(VoiceId voice, float wet)
    {
        const auto it = impl_->voices.find(voice);
        if (it == impl_->voices.end() || !it->second.source || !impl_->reverb) return;
        const float w = std::clamp(wet, 0.0f, 1.0f);
        const unsigned src = std::max(1u, it->second.channels);
        std::vector<float> matrix(static_cast<std::size_t>(src) * 2, 0.0f);
        for (unsigned c = 0; c < src; ++c) { matrix[static_cast<std::size_t>(0) * src + c] = w; matrix[static_cast<std::size_t>(1) * src + c] = w; }
        it->second.source->SetOutputMatrix(impl_->reverb, src, 2, matrix.data());
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
