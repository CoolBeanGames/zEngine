#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// ZE-67: a fully decoded audio clip - interleaved 32-bit float PCM plus its
// sample rate and channel count. Short sounds and music are loaded whole; there
// is no streaming yet. The decoders (WAV / MP3 / FLAC / OGG) are the vendored
// dr_libs + stb_vorbis, wrapped by third_party/audio_decoders.h.
namespace zengine::audio
{
    struct Clip
    {
        std::uint32_t sampleRate = 0;
        std::uint32_t channels = 0;
        std::vector<float> samples; // interleaved, nominally in [-1, 1]

        std::size_t Frames() const noexcept { return channels ? samples.size() / channels : 0; }
        double Seconds() const noexcept { return sampleRate ? static_cast<double>(Frames()) / sampleRate : 0.0; }
        bool Valid() const noexcept { return sampleRate > 0 && channels > 0 && !samples.empty(); }
    };

    // True for a path whose extension is a supported audio container.
    bool IsAudioFile(const std::filesystem::path& path);

    // Decode a file / an in-memory file. `extension` includes the leading dot and
    // selects the decoder. Both throw std::runtime_error on any I/O or decode
    // failure or if the result would exceed the in-memory size cap.
    Clip LoadFile(const std::filesystem::path& path);
    Clip LoadMemory(std::string_view bytes, std::string_view extension);
}
