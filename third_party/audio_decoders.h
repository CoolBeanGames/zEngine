/* ZE-67: a tiny unified front end over the vendored dr_libs + stb_vorbis
   decoders, so the engine only ever sees this header (not the ~1MB of
   single-file library source). */
#ifndef ZAUDIO_DECODERS_H
#define ZAUDIO_DECODERS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Container format of the bytes passed to zaudio_decode. */
enum { ZAUDIO_WAV = 0, ZAUDIO_MP3 = 1, ZAUDIO_FLAC = 2, ZAUDIO_OGG = 3 };

/* Decode a whole file (held in memory) to interleaved 32-bit float PCM.
   Returns 1 on success and 0 on failure. On success *samples is heap
   allocated (release with zaudio_free), holds (*frameCount * *channels)
   floats, and *channels / *sampleRate describe it. */
int zaudio_decode(int format, const void* data, size_t size,
                  float** samples, unsigned* channels, unsigned* sampleRate,
                  unsigned long long* frameCount);

void zaudio_free(void* pointer);

#ifdef __cplusplus
}
#endif

#endif
