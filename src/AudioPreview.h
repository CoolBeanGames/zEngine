#pragma once

#include "audio/AudioClip.h"
#include "audio/AudioEngine.h"

#include <windows.h>
#include <filesystem>
#include <memory>

// ZE-118: a tiny transport window that plays a single audio asset. Opened by
// double-clicking a .wav / .mp3 / .ogg / .flac in the Media Library. Playback
// stops when the window is closed or the Stop button is pressed.
class AudioPreview final
{
public:
    AudioPreview(HWND owner, std::filesystem::path file);
    ~AudioPreview();

    void Show();
    HWND Window() const { return window_; }
    const std::filesystem::path& File() const { return file_; }

    static constexpr int PlayStopButton = 5500;
    static constexpr UINT_PTR PumpTimer = 1;

private:
    static LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM);
    void Layout();
    void StartPlayback();
    void StopPlayback();
    void Tick();
    void RefreshTransport();

    std::filesystem::path file_;
    std::shared_ptr<const zengine::audio::Clip> clip_;
    zengine::audio::AudioEngine engine_;
    zengine::audio::VoiceId voice_ = 0;
    bool playing_ = false;
    HWND window_ = nullptr, info_ = nullptr, transport_ = nullptr, button_ = nullptr;
};
