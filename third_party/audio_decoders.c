/* Single translation unit for the vendored dr_libs audio decoders (ZE-67).
   stb_vorbis is compiled from its own .c file. */
#include <stdlib.h>
#include <string.h>

#define DR_WAV_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#include "dr_libs/dr_wav.h"
#include "dr_libs/dr_mp3.h"
#include "dr_libs/dr_flac.h"

/* stb_vorbis (compiled separately) - just the one function we use. */
extern int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels,
                                    int* sample_rate, short** output);

#include "audio_decoders.h"

void zaudio_free(void* pointer) { free(pointer); }

int zaudio_decode(int format, const void* data, size_t size,
                  float** samples, unsigned* channels, unsigned* sampleRate,
                  unsigned long long* frameCount)
{
    *samples = NULL;
    *channels = 0;
    *sampleRate = 0;
    *frameCount = 0;

    if (format == ZAUDIO_WAV)
    {
        drwav_uint64 frames = 0;
        float* pcm = drwav_open_memory_and_read_pcm_frames_f32(data, size, channels, sampleRate, &frames, NULL);
        if (!pcm) return 0;
        *samples = pcm;
        *frameCount = frames;
        return 1;
    }
    if (format == ZAUDIO_MP3)
    {
        drmp3_config config;
        memset(&config, 0, sizeof(config));
        drmp3_uint64 frames = 0;
        float* pcm = drmp3_open_memory_and_read_pcm_frames_f32(data, size, &config, &frames, NULL);
        if (!pcm) return 0;
        *samples = pcm;
        *channels = config.channels;
        *sampleRate = config.sampleRate;
        *frameCount = frames;
        return 1;
    }
    if (format == ZAUDIO_FLAC)
    {
        drflac_uint64 frames = 0;
        float* pcm = drflac_open_memory_and_read_pcm_frames_f32(data, size, channels, sampleRate, &frames, NULL);
        if (!pcm) return 0;
        *samples = pcm;
        *frameCount = frames;
        return 1;
    }
    if (format == ZAUDIO_OGG)
    {
        int ch = 0, rate = 0;
        short* pcm16 = NULL;
        const int frames = stb_vorbis_decode_memory((const unsigned char*)data, (int)size, &ch, &rate, &pcm16);
        if (frames < 0 || !pcm16 || ch <= 0) { free(pcm16); return 0; }
        const size_t count = (size_t)frames * (size_t)ch;
        float* pcm = (float*)malloc(count * sizeof(float) + 1);
        if (!pcm) { free(pcm16); return 0; }
        for (size_t i = 0; i < count; ++i) pcm[i] = pcm16[i] / 32768.0f;
        free(pcm16);
        *samples = pcm;
        *channels = (unsigned)ch;
        *sampleRate = (unsigned)rate;
        *frameCount = (unsigned long long)frames;
        return 1;
    }
    return 0;
}
