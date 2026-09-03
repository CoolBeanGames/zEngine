#include "audio/AudioClip.h"
#include "audio/AudioSystem.h"
#include "audio/AudioEffect.h"
#include "physics/PhysicsBehavior.h"
#include "core/GameObject.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace zengine;
using namespace zengine::audio;

namespace
{
    void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
    bool Near(double a, double b, double eps = 0.02) { return std::fabs(a - b) <= eps; }

    void PutU32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xFF)); }
    void PutU16(std::string& s, std::uint16_t v) { for (int i = 0; i < 2; ++i) s.push_back(char((v >> (8 * i)) & 0xFF)); }

    // A canonical mono 16-bit PCM WAV: `frames` samples of a quiet sine at `rate` Hz.
    std::string MakeWav(std::uint32_t rate, std::uint32_t frames)
    {
        std::string data;
        for (std::uint32_t i = 0; i < frames; ++i)
        {
            const double t = static_cast<double>(i) / rate;
            const auto s = static_cast<std::int16_t>(std::sin(t * 2 * 3.14159265 * 220.0) * 8000.0);
            PutU16(data, static_cast<std::uint16_t>(s));
        }
        std::string wav = "RIFF";
        PutU32(wav, 36 + static_cast<std::uint32_t>(data.size()));
        wav += "WAVEfmt ";
        PutU32(wav, 16);          // fmt chunk size
        PutU16(wav, 1);           // PCM
        PutU16(wav, 1);           // channels
        PutU32(wav, rate);
        PutU32(wav, rate * 2);    // byte rate
        PutU16(wav, 2);           // block align
        PutU16(wav, 16);          // bits per sample
        wav += "data";
        PutU32(wav, static_cast<std::uint32_t>(data.size()));
        wav += data;
        return wav;
    }

    struct TempDir
    {
        std::filesystem::path path;
        TempDir()
        {
            path = std::filesystem::temp_directory_path() /
                   ("zaudio-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(std::rand()));
            std::filesystem::create_directories(path);
        }
        ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    };
}

int main()
{
    try
    {
        // ----- decode -----
        {
            const auto wav = MakeWav(8000, 4000);
            const auto clip = LoadMemory(wav, ".wav");
            Check(clip.Valid(), "decoded WAV is invalid");
            Check(clip.channels == 1 && clip.sampleRate == 8000, "WAV format wrong");
            Check(clip.Frames() == 4000, "WAV frame count wrong");
            Check(Near(clip.Seconds(), 0.5), "WAV duration wrong");

            bool threw = false;
            try { LoadMemory("not audio", ".ogg"); } catch (const std::exception&) { threw = true; }
            Check(threw, "garbage OGG should fail to decode");
            threw = false;
            try { LoadMemory(wav, ".xyz"); } catch (const std::exception&) { threw = true; }
            Check(threw, "unknown extension should be rejected");
            Check(IsAudioFile("music/theme.mp3") && IsAudioFile("a.OGG") && !IsAudioFile("a.txt"),
                  "IsAudioFile classification wrong");
        }

        // ----- distance gain -----
        {
            Check(DistanceGain(Attenuation::None, 1000, 1, 10) == 1.0f, "None ignores distance");
            Check(DistanceGain(Attenuation::Linear, 0.5f, 1, 11) == 1.0f, "Linear full inside min");
            Check(DistanceGain(Attenuation::Linear, 11, 1, 11) == 0.0f, "Linear silent past max");
            Check(Near(DistanceGain(Attenuation::Linear, 6, 1, 11), 0.5), "Linear halfway");
            Check(DistanceGain(Attenuation::Inverse, 20, 1, 10) == 0.0f, "Inverse silent past max");
            Check(DistanceGain(Attenuation::Inverse, 2, 1, 100) > DistanceGain(Attenuation::Inverse, 8, 1, 100),
                  "Inverse falls off with distance");
            Attenuation a{};
            Check(ParseAttenuation("inverse", a) && a == Attenuation::Inverse, "ParseAttenuation");
            Check(std::string(AttenuationName(Attenuation::Linear)) == "linear", "AttenuationName");
        }

        // ----- AudioSystem lifecycle -----
        {
            TempDir dir;
            { std::ofstream(dir.path / "beep.wav", std::ios::binary) << MakeWav(8000, 4000); } // 0.5s

            ObjectStore store;
            auto& obj = store.Create("Speaker");
            auto& src = obj.AddBehavior<AudioSource>();
            src.SetClip("beep.wav");
            src.SetSpatial(false);
            src.SetAutoplay(true);
            src.SetLoop(false);

            AudioSystem audio{false}; // logical clock - deterministic without a device
            audio.SetClipLoader([&](const std::string& p) { return LoadFile(dir.path / p); });

            audio.Update(store, 1.0f / 60);
            Check(audio.Started().size() == 1 && audio.Started()[0] == obj.Id(), "autoplay did not start the source");
            Check(audio.Finished().empty(), "one-shot finished immediately");

            // Advance past the 0.5s clip -> it reports finished exactly once.
            bool sawFinish = false;
            for (int i = 0; i < 60; ++i)
            {
                audio.Update(store, 1.0f / 60);
                for (auto id : audio.Finished()) if (id == obj.Id()) sawFinish = true;
            }
            Check(sawFinish, "one-shot never reported finished");

            // Explicit Play() restarts it.
            src.Play();
            audio.Update(store, 1.0f / 60);
            Check(audio.Started().size() == 1, "Play() did not restart the source");
        }

        // ----- looping + StopAll -----
        {
            TempDir dir;
            { std::ofstream(dir.path / "loop.wav", std::ios::binary) << MakeWav(8000, 800); } // 0.1s

            ObjectStore store;
            auto& obj = store.Create("Loop");
            auto& src = obj.AddBehavior<AudioSource>();
            src.SetClip("loop.wav"); src.SetSpatial(false); src.SetAutoplay(false); src.SetLoop(true);

            AudioSystem audio{false};
            audio.SetClipLoader([&](const std::string& p) { return LoadFile(dir.path / p); });
            src.Play();
            audio.Update(store, 0.001f);
            Check(audio.Started().size() == 1, "explicit Play on a loop did not start");

            int loops = 0;
            for (int i = 0; i < 30; ++i) { audio.Update(store, 0.05f); loops += static_cast<int>(audio.Looped().size()); }
            Check(loops >= 3, "looping clip did not report repeated loops");
            Check(audio.Finished().empty(), "looping clip should never finish on its own");

            audio.StopAll();
            audio.Update(store, 0.05f);
            Check(audio.Started().empty() && audio.Looped().empty(), "StopAll left a voice running");
        }

        // ----- ZE-109: reverb / effect zones on an Area -----
        {
            TempDir dir;
            { std::ofstream(dir.path / "amb.wav", std::ios::binary) << MakeWav(8000, 8000); } // 1s

            ObjectStore store;
            // A 10x10x10 audio-effect area centred at the origin.
            auto& zone = store.Create("Cave");
            auto& col = zone.AddBehavior<zengine::physics::Collider>();
            col.SetShape(zengine::physics::ColliderShape::Box);
            col.SetSize({10, 10, 10});
            zone.AddBehavior<zengine::physics::Area>();
            auto& fx = zone.AddBehavior<AudioEffect>();
            fx.SetWetMix(0.8f); fx.SetBlendDistance(2.0f);

            auto& speaker = store.Create("Speaker");
            auto& src = speaker.AddBehavior<AudioSource>();
            src.SetClip("amb.wav"); src.SetSpatial(true); src.SetAutoplay(true); src.SetLoop(true);
            speaker.GetTransform().SetPosition({0, 0, 0}); // deep inside

            AudioSystem audio{false};
            audio.SetClipLoader([&](const std::string& p) { return LoadFile(dir.path / p); });

            audio.SetListener({0, 0, 0}); // listener inside too
            audio.Update(store, 1.0f / 60);
            Check(audio.ActiveEffectArea() == zone.Id(), "listener inside the zone did not activate its effect");
            Check(Near(audio.ReverbSendOf(speaker.Id()), 0.8f, 0.01), "emitter deep inside should get the full wet mix");

            // Emitter near the boundary (x = 4, box half-extent 5, blend band 2) -> ~half wet.
            speaker.GetTransform().SetPosition({4, 0, 0});
            audio.Update(store, 1.0f / 60);
            Check(audio.ReverbSendOf(speaker.Id()) > 0.1f && audio.ReverbSendOf(speaker.Id()) < 0.75f,
                  "emitter near the boundary should get a blended wet send");

            // Listener leaves the zone -> no effect, everything dry.
            audio.SetListener({20, 0, 0});
            audio.Update(store, 1.0f / 60);
            Check(audio.ActiveEffectArea() == 0 && audio.ReverbSendOf(speaker.Id()) == 0.0f,
                  "leaving the zone did not return the audio to dry");

            // A disabled effect is ignored.
            audio.SetListener({0, 0, 0});
            fx.SetEnabled(false);
            audio.Update(store, 1.0f / 60);
            Check(audio.ActiveEffectArea() == 0, "a disabled AudioEffect still routed audio");
        }

        std::cout << "PASS: decode (wav), distance attenuation, AudioSystem start/finish/restart, looping, StopAll, reverb zones\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
