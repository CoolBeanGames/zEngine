#include "VideoImport.h"
#include "ui/VideoClip.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace videoimport
{
using Microsoft::WRL::ComPtr;

namespace
{
    constexpr int kMaxFrames = 4096;                       // matches the .zvid frame cap
    constexpr std::uint64_t kMaxPixels = 16ull * 1024 * 1024; // per-frame pixel budget

    struct MediaFoundation
    {
        bool ok = false;
        MediaFoundation() { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); }
        ~MediaFoundation() { if (ok) MFShutdown(); }
    };

    void Fail(const char* what) { throw std::runtime_error(what); }

    const DWORD kFirstVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    const DWORD kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
}

bool IsVideoFile(const std::filesystem::path& path)
{
    auto ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".mp4" || ext == L".mov" || ext == L".m4v";
}

void ImportToZvid(const std::filesystem::path& source, const std::filesystem::path& destination)
{
    MediaFoundation mf;
    if (!mf.ok) Fail("Windows Media Foundation is not available on this system.");

    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 1)) ||
        FAILED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)))
        Fail("Could not configure the video reader.");

    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(source.c_str(), attributes.Get(), &reader)))
        Fail("Could not open the video file (unsupported container or missing codec).");

    // Video only.
    reader->SetStreamSelection(kAllStreams, FALSE);
    reader->SetStreamSelection(kFirstVideo, TRUE);

    ComPtr<IMFMediaType> rgb;
    if (FAILED(MFCreateMediaType(&rgb)) ||
        FAILED(rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
        FAILED(reader->SetCurrentMediaType(kFirstVideo, nullptr, rgb.Get())))
        Fail("This video's pixel format cannot be decoded to RGB.");

    ComPtr<IMFMediaType> actual;
    if (FAILED(reader->GetCurrentMediaType(kFirstVideo, &actual)))
        Fail("Could not read the video format.");

    UINT32 width = 0, height = 0;
    if (FAILED(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &width, &height)) || width == 0 || height == 0)
        Fail("The video has no usable frame size.");
    if (static_cast<std::uint64_t>(width) * height > kMaxPixels)
        Fail("Video frames are too large to import (16M pixel limit).");

    UINT32 fpsNum = 0, fpsDen = 0;
    float fps = 24.0f;
    if (SUCCEEDED(MFGetAttributeRatio(actual.Get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen)) && fpsDen != 0)
        fps = static_cast<float>(fpsNum) / static_cast<float>(fpsDen);
    if (!(fps > 0.0f) || fps > 240.0f) fps = 24.0f;

    LONG stride = static_cast<LONG>(width) * 4;
    {
        UINT32 declared = 0;
        if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &declared)) && declared != 0)
            stride = static_cast<LONG>(static_cast<INT32>(declared));
    }
    const bool bottomUp = stride < 0;
    const LONG absStride = bottomUp ? -stride : stride;
    if (absStride < static_cast<LONG>(width) * 4) Fail("The decoded video stride is invalid.");

    const std::size_t frameBytes = static_cast<std::size_t>(width) * height * 4;
    std::vector<std::vector<std::uint8_t>> frames;

    for (;;)
    {
        DWORD streamFlags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(kFirstVideo, 0, nullptr, &streamFlags, nullptr, &sample)))
            Fail("The video stream ended unexpectedly or is corrupt.");
        if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (streamFlags & MF_SOURCE_READERF_ERROR) Fail("A decode error occurred while reading the video.");
        if (!sample) continue; // a gap / format change with no data - skip

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) Fail("Could not access a decoded video frame.");

        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) Fail("Could not lock a decoded video frame.");
        if (length < static_cast<DWORD>(absStride) * height)
        {
            buffer->Unlock();
            Fail("A decoded video frame was smaller than expected.");
        }

        std::vector<std::uint8_t> rgba(frameBytes);
        for (UINT32 row = 0; row < height; ++row)
        {
            const UINT32 srcRow = bottomUp ? (height - 1 - row) : row;
            const BYTE* src = data + static_cast<std::size_t>(srcRow) * absStride;
            std::uint8_t* dst = rgba.data() + static_cast<std::size_t>(row) * width * 4;
            for (UINT32 x = 0; x < width; ++x)
            {
                dst[x * 4 + 0] = src[x * 4 + 2]; // R  (MF RGB32 is B,G,R,X)
                dst[x * 4 + 1] = src[x * 4 + 1]; // G
                dst[x * 4 + 2] = src[x * 4 + 0]; // B
                dst[x * 4 + 3] = 255;            // opaque - RGB32 carries no alpha
            }
        }
        buffer->Unlock();

        frames.push_back(std::move(rgba));
        if (static_cast<int>(frames.size()) >= kMaxFrames)
            Fail("This video is longer than the 4096-frame clip limit; trim it or lower its frame rate.");
    }

    if (frames.size() < 2) Fail("The video decoded to fewer than two frames.");

    const auto encoded = zengine::ui::VideoClip::Encode(static_cast<int>(width), static_cast<int>(height), fps, frames);
    std::ofstream stream(destination, std::ios::binary);
    stream.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    if (!stream) Fail("Could not write the .zvid file.");
}
}
