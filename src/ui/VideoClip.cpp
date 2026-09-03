#include "ui/VideoClip.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace zengine::ui
{
    namespace
    {
        constexpr int kMaxDimension = 4096;
        constexpr long long kMaxBytes = 512LL * 1024 * 1024; // 512 MiB decoded ceiling
    }

    VideoClip VideoClip::LoadFromMemory(std::string_view bytes)
    {
        const auto newline = bytes.find('\n');
        if (newline == std::string_view::npos) throw std::runtime_error("Video clip is missing its header.");

        std::istringstream header{std::string(bytes.substr(0, newline))};
        std::string magic;
        long long width = 0, height = 0, frames = 0;
        double fps = 0;
        if (!(header >> magic >> width >> height >> fps >> frames) || magic != "ZVID1")
            throw std::runtime_error("Video clip header is malformed.");
        if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension)
            throw std::runtime_error("Video clip dimensions are out of range.");
        if (frames <= 0 || frames > 100000 || !(fps > 0) || fps > 1000)
            throw std::runtime_error("Video clip frame count / rate is out of range.");

        const long long frameBytes = width * height * 4;
        const long long total = frameBytes * frames;
        if (total > kMaxBytes) throw std::runtime_error("Video clip is larger than the 512 MiB limit.");

        const std::string_view body = bytes.substr(newline + 1);
        if (static_cast<long long>(body.size()) < total)
            throw std::runtime_error("Video clip is truncated.");

        VideoClip clip;
        clip.width_ = static_cast<int>(width);
        clip.height_ = static_cast<int>(height);
        clip.frameCount_ = static_cast<int>(frames);
        clip.fps_ = static_cast<float>(fps);
        clip.pixels_.resize(static_cast<std::size_t>(total));
        std::memcpy(clip.pixels_.data(), body.data(), static_cast<std::size_t>(total));
        return clip;
    }

    VideoClip VideoClip::LoadFile(const std::filesystem::path& path)
    {
        std::ifstream in{path, std::ios::binary};
        if (!in) throw std::runtime_error("Cannot open video clip: " + path.string());
        std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        return LoadFromMemory(bytes);
    }

    std::string VideoClip::Encode(int width, int height, float fps,
                                  const std::vector<std::vector<std::uint8_t>>& frames)
    {
        if (width <= 0 || height <= 0 || frames.empty() || !(fps > 0))
            throw std::runtime_error("Invalid arguments for VideoClip::Encode.");
        const std::size_t frameBytes = static_cast<std::size_t>(width) * height * 4;
        std::ostringstream out;
        out << "ZVID1 " << width << ' ' << height << ' ' << fps << ' ' << frames.size() << '\n';
        std::string result = out.str();
        for (const auto& frame : frames)
        {
            if (frame.size() != frameBytes) throw std::runtime_error("VideoClip::Encode frame size mismatch.");
            result.append(reinterpret_cast<const char*>(frame.data()), frame.size());
        }
        return result;
    }

    const std::uint8_t* VideoClip::Frame(int index) const noexcept
    {
        if (frameCount_ <= 0) return nullptr;
        index = std::clamp(index, 0, frameCount_ - 1);
        return pixels_.data() + static_cast<std::size_t>(index) * width_ * height_ * 4;
    }

    int VideoClip::FrameAt(double seconds, bool loop) const noexcept
    {
        if (frameCount_ <= 0) return 0;
        if (seconds < 0) seconds = 0;
        long long index = static_cast<long long>(std::floor(seconds * fps_));
        if (loop) index %= frameCount_;
        else index = std::min<long long>(index, frameCount_ - 1);
        return static_cast<int>(index);
    }
}
