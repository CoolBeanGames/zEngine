#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace zengine::ui
{
    // A decoded video clip: a run of raw RGBA frames plus a playback rate. This is
    // the engine's own uncompressed container (extension ".zvid") - there is no
    // third-party codec dependency. The file is a one-line ASCII header
    //
    //     ZVID1 <width> <height> <fps> <frameCount>\n
    //
    // followed immediately by width*height*4*frameCount bytes of top-down RGBA.
    class VideoClip
    {
    public:
        VideoClip() = default;

        static VideoClip LoadFromMemory(std::string_view bytes);      // throws std::runtime_error on malformed input
        static VideoClip LoadFile(const std::filesystem::path& path); // throws on I/O or format error

        // Serializes `frames` (each width*height*4 bytes, RGBA) into the .zvid layout.
        static std::string Encode(int width, int height, float fps,
                                  const std::vector<std::vector<std::uint8_t>>& frames);

        int Width() const noexcept { return width_; }
        int Height() const noexcept { return height_; }
        float Fps() const noexcept { return fps_; }
        int FrameCount() const noexcept { return frameCount_; }
        bool Valid() const noexcept { return frameCount_ > 0; }

        // Frame at `index` (clamped to [0, FrameCount)). Returns nullptr if empty.
        const std::uint8_t* Frame(int index) const noexcept;

        // The frame index to show at playback time `seconds`.
        int FrameAt(double seconds, bool loop) const noexcept;

    private:
        int width_ = 0;
        int height_ = 0;
        int frameCount_ = 0;
        float fps_ = 0;
        std::vector<std::uint8_t> pixels_;
    };
}
