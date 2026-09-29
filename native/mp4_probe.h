#pragma once
#include <stddef.h>
#include <stdint.h>
// A tiny ISO-BMFF (MP4 / MOV / 3GP / M4A) reader: enough to tell a video from an audio file and
// to read the duration and picture size without decoding anything. WeChat's video message wants
// the play length in whole seconds, and the choice between "video bubble" and "file" hangs on
// whether the container really holds a video track. Bounded: reads box headers by seeking, and
// only the `moov` box itself (capped) is loaded.
namespace satori {
struct Mp4Info {
    bool container;        // has an `ftyp` box in the first bytes: this is ISO-BMFF at all
    bool has_video;        // a `vide` track with a non-zero picture size
    bool has_audio;        // a `soun` track
    uint32_t duration_ms;  // movie duration, 0 when unknown
    uint32_t width, height; // of the first video track
};
// Reads `path`. False when the file cannot be read; `info->container` says whether it parsed.
bool Mp4Probe(const char *path, Mp4Info *info);
// The same over bytes already in memory (the whole file, or at least its `moov`).
bool Mp4ProbeBytes(const unsigned char *data, size_t size, Mp4Info *info);
} // namespace satori
