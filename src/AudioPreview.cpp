#include "AudioPreview.h"
#include "EditorStyle.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <stdexcept>
#include <string>

namespace
{
    std::wstring Clock(double seconds)
    {
        if (seconds < 0 || !std::isfinite(seconds)) seconds = 0;
        const int total = static_cast<int>(seconds + 0.5);
        wchar_t buffer[16];
        swprintf(buffer, 16, L"%d:%02d", total / 60, total % 60);
        return buffer;
    }
}

AudioPreview::AudioPreview(HWND owner, std::filesystem::path file)
    : file_(std::move(file))
{
    clip_ = std::make_shared<const zengine::audio::Clip>(zengine::audio::LoadFile(file_));
    if (!clip_->Valid()) throw std::runtime_error("That audio file could not be decoded.");

    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = Procedure;
    wc.lpszClassName = L"zEngineAudioPreview";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = editorStyle::Shared().panel;
    RegisterClassW(&wc);

    window_ = CreateWindowExW(0, wc.lpszClassName, L"Audio Preview",
                              (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME) | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 380, 168, owner, nullptr, wc.hInstance, this);
    if (!window_) throw std::runtime_error("Cannot open the audio preview window.");

    const auto ctl = [&](const wchar_t* cls, const wchar_t* text, int id, DWORD style) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), wc.hInstance, nullptr);
    };
    const std::wstring header = file_.filename().wstring() + L"\n" +
        std::to_wstring(clip_->sampleRate) + L" Hz  " +
        (clip_->channels == 1 ? std::wstring(L"mono") : clip_->channels == 2 ? std::wstring(L"stereo")
                                                                            : std::to_wstring(clip_->channels) + L" ch");
    info_ = ctl(L"STATIC", header.c_str(), 5490, 0);
    transport_ = ctl(L"STATIC", L"", 5491, 0);
    button_ = ctl(L"BUTTON", L"Stop", PlayStopButton, WS_TABSTOP | BS_PUSHBUTTON);

    editorStyle::AttachChildren(window_);
    Layout();
    SetWindowTextW(window_, (file_.filename().wstring() + L" - Audio Preview").c_str());
    StartPlayback();
}

AudioPreview::~AudioPreview()
{
    StopPlayback();
    if (IsWindow(window_)) DestroyWindow(window_);
}

void AudioPreview::Show()
{
    ShowWindow(window_, SW_SHOWNORMAL);
    SetForegroundWindow(window_);
    if (!playing_) StartPlayback();
}

void AudioPreview::Layout()
{
    if (!window_) return;
    RECT r; GetClientRect(window_, &r);
    MoveWindow(info_, 14, 12, r.right - 28, 40, TRUE);
    MoveWindow(transport_, 14, 60, r.right - 28, 22, TRUE);
    MoveWindow(button_, 14, r.bottom - 42, 96, 28, TRUE);
}

void AudioPreview::StartPlayback()
{
    StopPlayback();
    voice_ = engine_.Play(clip_, 1.0f, 1.0f, false);
    playing_ = voice_ != 0;
    SetTimer(window_, PumpTimer, 80, nullptr);
    RefreshTransport();
}

void AudioPreview::StopPlayback()
{
    if (window_) KillTimer(window_, PumpTimer);
    if (voice_) engine_.Stop(voice_);
    voice_ = 0;
    playing_ = false;
    engine_.Pump();
}

void AudioPreview::Tick()
{
    engine_.Pump();
    if (playing_ && voice_ && !engine_.Active(voice_))
    {
        voice_ = 0;
        playing_ = false;
        KillTimer(window_, PumpTimer);
    }
    RefreshTransport();
}

void AudioPreview::RefreshTransport()
{
    double position = 0;
    if (playing_ && voice_ && clip_->sampleRate)
        position = static_cast<double>(engine_.PlayCursorFrames(voice_)) / clip_->sampleRate;
    position = std::min(position, clip_->Seconds());
    SetWindowTextW(transport_, (Clock(position) + L" / " + Clock(clip_->Seconds())).c_str());
    SetWindowTextW(button_, playing_ ? L"Stop" : L"Play");
}

LRESULT CALLBACK AudioPreview::Procedure(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<AudioPreview*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (m == WM_NCCREATE)
    {
        self = static_cast<AudioPreview*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = w;
    }
    if (!self) return DefWindowProcW(w, m, wp, lp);

    switch (m)
    {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return editorStyle::ControlColor(m, wp);
    case WM_SIZE:
        self->Layout();
        return 0;
    case WM_TIMER:
        if (wp == PumpTimer) self->Tick();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == PlayStopButton && HIWORD(wp) == BN_CLICKED)
        {
            if (self->playing_) self->StopPlayback();
            else self->StartPlayback();
            self->RefreshTransport();
            return 0;
        }
        break;
    case WM_CLOSE:
        self->StopPlayback();
        ShowWindow(w, SW_HIDE);
        return 0;
    case WM_DESTROY:
        self->StopPlayback();
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}
