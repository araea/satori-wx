// Host-side tests for the ISO-BMFF probe: duration, picture size and track kinds from real
// (ffmpeg-made) containers, plus the shapes that must not confuse it.
#include "mp4_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
const char *Fixture(const char *name) {
    static char path[512];
    const char *root = getenv("SATORI_FIXTURES");
    snprintf(path, sizeof(path), "%s/%s", root ? root : "tests/fixtures", name);
    return path;
}
}

int main() {
    satori::Mp4Info info;
    // moov up front (faststart): 1 s, 64x48, with an audio track.
    Check(satori::Mp4Probe(Fixture("tiny.mp4"), &info), "tiny.mp4 reads");
    Check(info.container && info.has_video && info.has_audio, "tiny.mp4 has video and audio");
    Check(info.width == 64 && info.height == 48, "tiny.mp4 picture size");
    Check(info.duration_ms >= 950 && info.duration_ms <= 1100, "tiny.mp4 duration is about a second");

    // moov after mdat: found by seeking past the media data.
    Check(satori::Mp4Probe(Fixture("tiny-moovlast.mp4"), &info), "moov-last reads");
    Check(info.container && info.has_video && !info.has_audio, "moov-last: video only");
    Check(info.width == 64 && info.height == 48 && info.duration_ms >= 900 && info.duration_ms <= 1100, "moov-last size and duration");

    // audio-only container: not a video, however it is named.
    Check(satori::Mp4Probe(Fixture("tiny.m4a"), &info), "m4a reads");
    Check(info.container && !info.has_video && info.has_audio, "m4a is audio only");
    Check(info.duration_ms >= 1900 && info.duration_ms <= 2200, "m4a duration is about two seconds");

    // not a container at all
    const unsigned char text[] = "hello, this is plain text, not a movie at all";
    Check(satori::Mp4ProbeBytes(text, sizeof(text), &info) && !info.container, "plain text is not a container");
    const unsigned char png[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 1};
    Check(satori::Mp4ProbeBytes(png, sizeof(png), &info) && !info.container, "a PNG is not a container");
    // a truncated box header must not read past the end
    const unsigned char cut[] = {0, 0, 0, 24, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 2, 0, 'i', 's'};
    Check(satori::Mp4ProbeBytes(cut, sizeof(cut), &info) && info.container && !info.has_video, "truncated ftyp is tolerated");
    // an absurd box size must not loop or overflow
    const unsigned char huge[] = {0, 0, 0, 16, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 'm', 'o', 'o', 'v'};
    Check(satori::Mp4ProbeBytes(huge, sizeof(huge), &info) && info.container && !info.has_video, "oversized box is tolerated");
    Check(!satori::Mp4Probe("/nonexistent/file.mp4", &info), "a missing file is reported");

    if (failures) { fprintf(stderr, "%d mp4 test failure(s)\n", failures); return 1; }
    puts("mp4 probe tests passed");
    return 0;
}
