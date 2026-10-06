#include "audio_pcm.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace satori {
namespace {
constexpr int kTarget = 16000;
constexpr int kLobes = 8; // Lanczos lobes on each side of the centre
constexpr double kPi = 3.14159265358979323846;

double Sinc(double x) { return x == 0 ? 1.0 : sin(kPi * x) / (kPi * x); }
uint32_t Le32(const unsigned char *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint16_t Le16(const unsigned char *p) { return uint16_t(p[0] | p[1] << 8); }
} // namespace

int16_t *ResampleTo16kMono(const int16_t *interleaved, size_t frames, int channels, int rate, size_t *out_count) {
    if (!interleaved || !frames || channels < 1 || channels > 8 || rate < 1000 || rate > 384000 || !out_count)
        return nullptr;
    // Mixdown first: one float per frame.
    auto *mono = static_cast<float *>(malloc(frames * sizeof(float)));
    if (!mono) return nullptr;
    for (size_t i = 0; i < frames; ++i) {
        int sum = 0;
        for (int c = 0; c < channels; ++c)
            sum += interleaved[i * static_cast<size_t>(channels) + static_cast<size_t>(c)];
        mono[i] = static_cast<float>(sum) / static_cast<float>(channels);
    }
    const size_t count = static_cast<size_t>(static_cast<double>(frames) * kTarget / rate);
    auto *out = static_cast<int16_t *>(malloc((count ? count : 1) * sizeof(int16_t)));
    if (!out) {
        free(mono);
        return nullptr;
    }
    if (rate == kTarget) {
        for (size_t i = 0; i < count; ++i) out[i] = static_cast<int16_t>(mono[i]);
        free(mono);
        *out_count = count;
        return out;
    }
    const double step = static_cast<double>(rate) / kTarget; // input samples per output sample
    const double cutoff = step > 1.0 ? 1.0 / step : 1.0;     // low-pass below the new Nyquist when shrinking
    const double reach = kLobes / cutoff;                    // kernel half-width in input samples
    for (size_t i = 0; i < count; ++i) {
        const double centre = static_cast<double>(i) * step;
        long first = static_cast<long>(ceil(centre - reach)), last = static_cast<long>(floor(centre + reach));
        if (first < 0) first = 0;
        if (last >= static_cast<long>(frames)) last = static_cast<long>(frames) - 1;
        double sum = 0, weight = 0;
        for (long j = first; j <= last; ++j) {
            const double x = (static_cast<double>(j) - centre) * cutoff;
            const double w = Sinc(x) * Sinc(x / kLobes);
            sum += w * mono[j];
            weight += w;
        }
        double value = weight != 0 ? sum / weight : 0;
        if (value > 32767) value = 32767;
        if (value < -32768) value = -32768;
        out[i] = static_cast<int16_t>(lrint(value));
    }
    free(mono);
    *out_count = count;
    return out;
}

bool WavToPcm16k(const char *path, int16_t **out, size_t *count, size_t max_samples, bool *truncated) {
    if (!path || !out || !count) return false;
    *out = nullptr;
    *count = 0;
    if (truncated) *truncated = false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    unsigned char head[12];
    if (fread(head, 1, 12, file) != 12 || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4)) {
        fclose(file);
        return false;
    }
    int format = 0, channels = 0, rate = 0, bits = 0;
    bool have_format = false;
    long data_offset = -1;
    uint64_t data_size = 0;
    for (int guard = 0; guard < 64; ++guard) {
        unsigned char chunk[8];
        if (fread(chunk, 1, 8, file) != 8) break;
        const uint32_t size = Le32(chunk + 4);
        if (!memcmp(chunk, "fmt ", 4) && size >= 16) {
            unsigned char fmt[40] = {};
            const size_t take = size < sizeof(fmt) ? size : sizeof(fmt);
            if (fread(fmt, 1, take, file) != take) break;
            format = Le16(fmt);
            channels = Le16(fmt + 2);
            rate = static_cast<int>(Le32(fmt + 4));
            bits = Le16(fmt + 14);
            if (format == 0xFFFE && take >= 26) format = Le16(fmt + 24); // WAVE_FORMAT_EXTENSIBLE: the real tag
            have_format = true;
            if (size > take) fseek(file, static_cast<long>(size - take), SEEK_CUR);
        } else if (!memcmp(chunk, "data", 4)) {
            data_offset = ftell(file);
            data_size = size;
            break;
        } else {
            fseek(file, static_cast<long>(size), SEEK_CUR);
        }
        if (size & 1) fseek(file, 1, SEEK_CUR); // chunks are word-aligned
    }
    const bool pcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const bool ieee = format == 3 && bits == 32;
    if (!have_format || data_offset < 0 || !(pcm || ieee) || channels < 1 || channels > 8 || rate < 1000) {
        fclose(file);
        return false;
    }
    const size_t bytes_per_sample = static_cast<size_t>(bits / 8);
    const size_t frame_bytes = bytes_per_sample * static_cast<size_t>(channels);
    // Read at most what could produce max_samples (plus the resampler's margin).
    uint64_t frames = data_size / frame_bytes;
    const uint64_t needed = static_cast<uint64_t>(max_samples) * static_cast<uint64_t>(rate) / kTarget + 64;
    if (frames > needed) {
        frames = needed;
        if (truncated) *truncated = true;
    }
    if (!frames) {
        fclose(file);
        return false;
    }
    auto *raw = static_cast<unsigned char *>(malloc(static_cast<size_t>(frames) * frame_bytes));
    auto *pcm16 =
        static_cast<int16_t *>(malloc(static_cast<size_t>(frames) * static_cast<size_t>(channels) * sizeof(int16_t)));
    if (!raw || !pcm16) {
        free(raw);
        free(pcm16);
        fclose(file);
        return false;
    }
    fseek(file, data_offset, SEEK_SET);
    const size_t got = fread(raw, frame_bytes, static_cast<size_t>(frames), file);
    fclose(file);
    const size_t samples = got * static_cast<size_t>(channels);
    for (size_t i = 0; i < samples; ++i) {
        const unsigned char *p = raw + i * bytes_per_sample;
        int value;
        if (ieee) {
            float f;
            memcpy(&f, p, 4);
            value = static_cast<int>(lrintf(f * 32767.0f));
            if (value > 32767) value = 32767;
            if (value < -32768) value = -32768;
        } else if (bits == 8)
            value = (static_cast<int>(p[0]) - 128) << 8;
        else if (bits == 16)
            value = static_cast<int16_t>(Le16(p));
        else if (bits == 24)
            value = static_cast<int32_t>((uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24)) >> 16;
        else
            value = static_cast<int32_t>(Le32(p)) >> 16;
        pcm16[i] = static_cast<int16_t>(value);
    }
    free(raw);
    if (!got) {
        free(pcm16);
        return false;
    }
    size_t produced = 0;
    int16_t *result = ResampleTo16kMono(pcm16, got, channels, rate, &produced);
    free(pcm16);
    if (!result || !produced) {
        free(result);
        return false;
    }
    if (produced > max_samples) {
        produced = max_samples;
        if (truncated) *truncated = true;
    }
    *out = result;
    *count = produced;
    return true;
}

bool SilkInspect(const unsigned char *data, size_t size, SilkInfo *info) {
    if (!info) return false;
    *info = {};
    static const unsigned char magic[] = "#!SILK_V3";
    size_t pos = 0;
    if (size > 0 && data[0] == 0x02) {
        info->prefixed = true;
        pos = 1;
    }
    if (size < pos + 9 || memcmp(data + pos, magic, 9)) return false;
    pos += 9;
    unsigned packets = 0;
    while (pos + 2 <= size) {
        const unsigned n = Le16(data + pos);
        pos += 2;
        if (n == 0xFFFF) {
            pos = size;
            break;
        } // the optional end marker
        if (n == 0 || n > 2000 || pos + n > size) return false;
        pos += n;
        ++packets;
    }
    if (pos != size || !packets) return false;
    info->valid = true;
    info->packets = packets;
    info->duration_ms = packets * 20;
    return true;
}
} // namespace satori
