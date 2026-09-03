#include "audio/AudioClip.h"

#include "audio_decoders.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace zengine::audio
{
    namespace
    {
        constexpr std::size_t kMaxFileBytes = 128u * 1024 * 1024;   // 128 MiB compressed
        constexpr std::size_t kMaxSamples = 512u * 1024 * 1024 / sizeof(float); // 512 MiB decoded

        std::string Lower(std::string_view s)
        {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        int FormatFor(std::string_view extension)
        {
            const std::string ext = Lower(extension);
            if (ext == ".wav") return ZAUDIO_WAV;
            if (ext == ".mp3") return ZAUDIO_MP3;
            if (ext == ".flac") return ZAUDIO_FLAC;
            if (ext == ".ogg") return ZAUDIO_OGG;
            return -1;
        }
    }

    bool IsAudioFile(const std::filesystem::path& path)
    {
        return FormatFor(path.extension().string()) >= 0;
    }

    Clip LoadMemory(std::string_view bytes, std::string_view extension)
    {
        const int format = FormatFor(extension);
        if (format < 0) throw std::runtime_error("Unsupported audio format '" + std::string(extension) + "'.");
        if (bytes.empty()) throw std::runtime_error("Audio file is empty.");
        if (bytes.size() > kMaxFileBytes) throw std::runtime_error("Audio file is larger than 128 MiB.");

        float* decoded = nullptr;
        unsigned channels = 0, sampleRate = 0;
        unsigned long long frames = 0;
        if (!zaudio_decode(format, bytes.data(), bytes.size(), &decoded, &channels, &sampleRate, &frames) || !decoded)
            throw std::runtime_error("Could not decode the audio file.");

        const std::size_t count = static_cast<std::size_t>(frames) * channels;
        if (!channels || !sampleRate || !count || count > kMaxSamples)
        {
            zaudio_free(decoded);
            throw std::runtime_error("Decoded audio is empty or too large.");
        }

        Clip clip;
        clip.sampleRate = sampleRate;
        clip.channels = channels;
        clip.samples.assign(decoded, decoded + count);
        zaudio_free(decoded);
        return clip;
    }

    Clip LoadFile(const std::filesystem::path& path)
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) throw std::runtime_error("Cannot open audio file: " + path.string());
        if (size == 0 || size > kMaxFileBytes) throw std::runtime_error("Audio file is empty or larger than 128 MiB.");

        std::string bytes(static_cast<std::size_t>(size), '\0');
        std::ifstream in(path, std::ios::binary);
        if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
            throw std::runtime_error("Cannot read audio file: " + path.string());

        return LoadMemory(bytes, path.extension().string());
    }
}
