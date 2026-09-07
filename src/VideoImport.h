#pragma once

#include <filesystem>

// ZE-121: import an .mp4 (or other Media Foundation-decodable container) into the
// engine's uncompressed ".zvid" clip - the same format the image-sequence
// builder produces and that VideoTexture / the UI Video control consume. Any
// audio track in the source is ignored.
namespace videoimport
{
    bool IsVideoFile(const std::filesystem::path& path); // .mp4 / .mov / .m4v

    // Decode `source` and write `destination` (a .zvid file). Throws
    // std::runtime_error on any I/O, codec or limit failure.
    void ImportToZvid(const std::filesystem::path& source, const std::filesystem::path& destination);
}
