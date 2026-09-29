// Host-side tests for the audio helpers behind voice messages: resampling keeps the tone and drops
// what cannot survive, the WAV reader copes with the layouts real files come in, and the SILK
// container reader accepts exactly the streams WeChat plays.
#include "audio_pcm.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {
int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// A sine at `hz`, `seconds` long, `channels` identical channels, amplitude `amp`.
int16_t *Tone(double hz, double seconds, int rate, int channels, double amp, size_t *frames) {
    *frames = static_cast<size_t>(seconds * rate);
    auto *out = static_cast<int16_t *>(malloc(*frames * static_cast<size_t>(channels) * sizeof(int16_t)));
    for (size_t i = 0; i < *frames; ++i) {
        const int16_t v = static_cast<int16_t>(amp * sin(2 * M_PI * hz * static_cast<double>(i) / rate));
        for (int c = 0; c < channels; ++c) out[i * static_cast<size_t>(channels) + static_cast<size_t>(c)] = v;
    }
    return out;
}

double Rms(const int16_t *s, size_t n, size_t skip) {
    double sum = 0;
    size_t used = 0;
    for (size_t i = skip; i + skip < n; ++i, ++used) sum += static_cast<double>(s[i]) * s[i];
    return used ? sqrt(sum / static_cast<double>(used)) : 0;
}

// Frequency from rising zero crossings over the middle of the signal.
double Frequency(const int16_t *s, size_t n, int rate) {
    size_t crossings = 0, first = 0, last = 0;
    for (size_t i = 200; i + 200 < n; ++i) {
        if (s[i - 1] < 0 && s[i] >= 0) { if (!crossings) first = i; last = i; ++crossings; }
    }
    return crossings > 1 ? (crossings - 1) * static_cast<double>(rate) / static_cast<double>(last - first) : 0;
}

void Le(unsigned char *p, uint32_t v, int bytes) { for (int i = 0; i < bytes; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i)); }

// Writes a WAV with an optional LIST chunk in front of the data.
void WriteWav(const char *path, int format, int channels, int rate, int bits, const void *data, size_t bytes, bool list_chunk) {
    FILE *f = fopen(path, "wb");
    unsigned char h[44 + 20] = {};
    size_t n = 0;
    memcpy(h + n, "RIFF", 4); n += 4; Le(h + n, static_cast<uint32_t>(36 + bytes + (list_chunk ? 20 : 0)), 4); n += 4;
    memcpy(h + n, "WAVEfmt ", 8); n += 8; Le(h + n, 16, 4); n += 4;
    Le(h + n, static_cast<uint32_t>(format), 2); n += 2; Le(h + n, static_cast<uint32_t>(channels), 2); n += 2;
    Le(h + n, static_cast<uint32_t>(rate), 4); n += 4; Le(h + n, static_cast<uint32_t>(rate * channels * bits / 8), 4); n += 4;
    Le(h + n, static_cast<uint32_t>(channels * bits / 8), 2); n += 2; Le(h + n, static_cast<uint32_t>(bits), 2); n += 2;
    if (list_chunk) { memcpy(h + n, "LIST", 4); n += 4; Le(h + n, 12, 4); n += 4; memcpy(h + n, "INFOabcdefgh", 12); n += 12; }
    memcpy(h + n, "data", 4); n += 4; Le(h + n, static_cast<uint32_t>(bytes), 4); n += 4;
    fwrite(h, 1, n, f);
    fwrite(data, 1, bytes, f);
    fclose(f);
}
}

int main() {
    // ---- resampling -------------------------------------------------------------------------------
    struct Case { int rate; int channels; double hz; };
    static const Case cases[] = {{44100, 2, 440}, {48000, 1, 1000}, {8000, 1, 300}, {22050, 2, 2000}, {16000, 1, 500}, {96000, 2, 700}};
    for (const Case &c : cases) {
        size_t frames = 0, produced = 0;
        int16_t *in = Tone(c.hz, 1.0, c.rate, c.channels, 10000, &frames);
        int16_t *out = satori::ResampleTo16kMono(in, frames, c.channels, c.rate, &produced);
        char what[120];
        snprintf(what, sizeof(what), "%d Hz x%d: a second in is a second out", c.rate, c.channels);
        Check(out && produced >= 15990 && produced <= 16010, what);
        if (out) {
            snprintf(what, sizeof(what), "%d Hz x%d: the %g Hz tone keeps its pitch", c.rate, c.channels, c.hz);
            const double f = Frequency(out, produced, 16000);
            Check(fabs(f - c.hz) < c.hz * 0.02, what);
            snprintf(what, sizeof(what), "%d Hz x%d: and its level", c.rate, c.channels);
            const double level = Rms(out, produced, 400) / (10000 / sqrt(2.0));
            Check(level > 0.95 && level < 1.05, what);
        }
        free(in); free(out);
    }
    {
        // A 12 kHz tone cannot exist at 16 kHz: it must be filtered out, not folded down to 4 kHz.
        size_t frames = 0, produced = 0;
        int16_t *in = Tone(12000, 1.0, 44100, 1, 10000, &frames);
        int16_t *out = satori::ResampleTo16kMono(in, frames, 1, 44100, &produced);
        Check(out && Rms(out, produced, 400) < 10000 / sqrt(2.0) * 0.05, "energy above the new Nyquist is removed, not aliased");
        free(in); free(out);
    }
    {
        size_t produced = 0;
        Check(!satori::ResampleTo16kMono(nullptr, 10, 1, 8000, &produced), "no input");
        int16_t one[4] = {1, 2, 3, 4};
        Check(!satori::ResampleTo16kMono(one, 4, 0, 8000, &produced), "no channels");
        Check(!satori::ResampleTo16kMono(one, 4, 1, 10, &produced), "an absurd rate");
        int16_t *out = satori::ResampleTo16kMono(one, 4, 2, 16000, &produced);
        Check(out && produced == 4 && out[0] == 1 && out[1] == 3, "at 16 kHz only the channels are mixed (two frames of L/R average, rounded down)");
        free(out);
    }

    // ---- WAV --------------------------------------------------------------------------------------
    const char *root = getenv("SATORI_TMPROOT");
    char path[600];
    snprintf(path, sizeof(path), "%s/audio-test.wav", root ? root : "/tmp");
    {
        size_t frames = 0;
        int16_t *tone = Tone(440, 1.0, 22050, 2, 8000, &frames);
        WriteWav(path, 1, 2, 22050, 16, tone, frames * 4, true);   // stereo 16-bit with a LIST chunk before the data
        int16_t *pcm = nullptr;
        size_t count = 0;
        bool truncated = true;
        Check(satori::WavToPcm16k(path, &pcm, &count, 100000, &truncated) && count >= 15990 && count <= 16010 && !truncated, "a stereo WAV with a LIST chunk reads");
        Check(pcm && fabs(Frequency(pcm, count, 16000) - 440) < 9, "and keeps its pitch");
        free(pcm);
        Check(satori::WavToPcm16k(path, &pcm, &count, 4000, &truncated) && count == 4000 && truncated, "a long WAV is cut at the limit and says so");
        free(pcm);
        // float32
        auto *floats = static_cast<float *>(malloc(frames * sizeof(float)));
        for (size_t i = 0; i < frames; ++i) floats[i] = static_cast<float>(0.25 * sin(2 * M_PI * 1000.0 * static_cast<double>(i) / 22050));
        WriteWav(path, 3, 1, 22050, 32, floats, frames * 4, false);
        Check(satori::WavToPcm16k(path, &pcm, &count, 100000, &truncated) && fabs(Frequency(pcm, count, 16000) - 1000) < 20, "float WAV");
        Check(pcm && Rms(pcm, count, 400) > 32767 * 0.25 / sqrt(2.0) * 0.9 && Rms(pcm, count, 400) < 32767 * 0.25 / sqrt(2.0) * 1.1, "float WAV level");
        free(pcm); free(floats);
        // 8-bit unsigned
        auto *bytes8 = static_cast<unsigned char *>(malloc(frames));
        for (size_t i = 0; i < frames; ++i) bytes8[i] = static_cast<unsigned char>(128 + 100 * sin(2 * M_PI * 500.0 * static_cast<double>(i) / 22050));
        WriteWav(path, 1, 1, 22050, 8, bytes8, frames, false);
        Check(satori::WavToPcm16k(path, &pcm, &count, 100000, &truncated) && fabs(Frequency(pcm, count, 16000) - 500) < 10, "8-bit WAV");
        free(pcm); free(bytes8); free(tone);
        // not a WAV / not a supported one
        FILE *f = fopen(path, "wb"); fputs("RIFF....WAVEjunk", f); fclose(f);
        Check(!satori::WavToPcm16k(path, &pcm, &count, 1000, &truncated), "a broken WAV is refused");
        Check(!satori::WavToPcm16k("/nonexistent.wav", &pcm, &count, 1000, &truncated), "a missing file is refused");
        int16_t silence[10] = {};
        WriteWav(path, 2, 1, 8000, 4, silence, 10, false);   // ADPCM: not something we decode
        Check(!satori::WavToPcm16k(path, &pcm, &count, 1000, &truncated), "compressed WAV is refused");
        unlink(path);
    }

    // ---- SILK container -----------------------------------------------------------------------------
    {
        unsigned char stream[400];
        size_t n = 0;
        stream[n++] = 0x02;
        memcpy(stream + n, "#!SILK_V3", 9); n += 9;
        for (int i = 0; i < 10; ++i) { stream[n++] = 12; stream[n++] = 0; memset(stream + n, 0xAB, 12); n += 12; }
        satori::SilkInfo info;
        Check(satori::SilkInspect(stream, n, &info) && info.valid && info.prefixed && info.packets == 10 && info.duration_ms == 200, "a WeChat SILK stream (0x02 prefix)");
        Check(satori::SilkInspect(stream + 1, n - 1, &info) && info.valid && !info.prefixed && info.duration_ms == 200, "the same without the prefix");
        stream[n++] = 0xFF; stream[n++] = 0xFF;
        Check(satori::SilkInspect(stream, n, &info) && info.valid && info.packets == 10, "an end marker is fine");
        Check(!satori::SilkInspect(stream, n - 5, &info), "packets that do not tile the file");
        stream[10] = 0; stream[11] = 0;   // a zero-size packet
        Check(!satori::SilkInspect(stream, n, &info), "a zero-size packet");
        Check(!satori::SilkInspect(reinterpret_cast<const unsigned char *>("#!AMR\n1234"), 10, &info), "AMR is not SILK");
        Check(!satori::SilkInspect(reinterpret_cast<const unsigned char *>("\x02#!SILK_V3"), 10, &info), "a header and nothing else");
        Check(!satori::SilkInspect(nullptr, 0, &info), "nothing");
    }

    if (failures) { fprintf(stderr, "%d audio test failure(s)\n", failures); return 1; }
    puts("audio tests passed");
    return 0;
}
