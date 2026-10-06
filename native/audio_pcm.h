#pragma once
#include <stddef.h>
#include <stdint.h>
// The pure audio helpers behind voice messages: read a WAV, bring any PCM to the 16 kHz mono 16-bit
// that WeChat's own voice encoder takes, and read the shape of a SILK file. No JNI, no I/O beyond
// the WAV read, so all of it is host-testable.
namespace satori {
// Interleaved 16-bit PCM at `rate` Hz with `channels` channels -> 16 kHz mono 16-bit (band-limited
// windowed-sinc resampling; channels are averaged). Returns a malloc'd buffer (free()) and its
// length in samples, or null on bad arguments / no memory. `frames` is the number of sample frames.
int16_t *ResampleTo16kMono(const int16_t *interleaved, size_t frames, int channels, int rate, size_t *out_count);
// A RIFF/WAVE file (PCM 8/16/24/32-bit integer or 32-bit float, any channel count and rate) read into
// 16 kHz mono 16-bit. At most `max_samples` output samples are produced; `truncated` is set when the
// file held more. False when the file is not a WAV this reader understands.
bool WavToPcm16k(const char *path, int16_t **out, size_t *count, size_t max_samples, bool *truncated);
struct SilkInfo {
    bool valid;    // a SILK_V3 stream whose packets tile the file exactly
    bool prefixed; // it opens with WeChat's 0x02 byte before "#!SILK_V3"
    unsigned packets;
    unsigned duration_ms; // 20 ms per packet, WeChat's packet size
};
// Reads the SILK container: "[0x02]#!SILK_V3" then packets of (uint16 little-endian size, payload),
// optionally ended by 0xFFFF.
bool SilkInspect(const unsigned char *data, size_t size, SilkInfo *info);
} // namespace satori
