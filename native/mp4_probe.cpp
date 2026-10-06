#include "mp4_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace satori {
namespace {
constexpr size_t kMoovMax = 16u << 20; // a two-hour movie's sample tables are a few MiB

uint32_t Be32(const unsigned char *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint64_t Be64(const unsigned char *p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }

struct Box {
    uint32_t type;
    size_t header; // 8 or 16 bytes
    uint64_t size; // whole box including header; 0 = runs to the end
};

constexpr uint32_t Fourcc(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint8_t(s[3]);
}

// Reads a box header at `p` (at least 8 bytes available).
bool Header(const unsigned char *p, size_t available, Box *box) {
    if (available < 8) return false;
    box->size = Be32(p);
    box->type = Be32(p + 4);
    box->header = 8;
    if (box->size == 1) {
        if (available < 16) return false;
        box->size = Be64(p + 8);
        box->header = 16;
    }
    return box->size == 0 || box->size >= box->header;
}

void ParseTrak(const unsigned char *p, size_t size, Mp4Info *info, bool *first_video_done) {
    uint32_t width = 0, height = 0;
    uint32_t handler = 0;
    for (size_t pos = 0; pos < size;) {
        Box box;
        if (!Header(p + pos, size - pos, &box)) return;
        const size_t length = box.size ? static_cast<size_t>(box.size) : size - pos;
        if (length < box.header || length > size - pos) return;
        const unsigned char *body = p + pos + box.header;
        const size_t body_size = length - box.header;
        if (box.type == Fourcc("tkhd") && body_size >= 84) {
            // version 0: 84 bytes, width/height are the last two 16.16 fixed-point numbers.
            const size_t end = body[0] == 1 ? 96 : 84;
            if (body_size >= end) {
                width = Be32(body + end - 8) >> 16;
                height = Be32(body + end - 4) >> 16;
            }
        } else if (box.type == Fourcc("mdia")) {
            for (size_t inner = 0; inner < body_size;) {
                Box child;
                if (!Header(body + inner, body_size - inner, &child)) break;
                const size_t child_length = child.size ? static_cast<size_t>(child.size) : body_size - inner;
                if (child_length < child.header || child_length > body_size - inner) break;
                if (child.type == Fourcc("hdlr") && child_length >= child.header + 12)
                    handler = Be32(body + inner + child.header + 8);
                inner += child_length;
            }
        }
        pos += length;
    }
    if (handler == Fourcc("vide") && width && height) {
        info->has_video = true;
        if (!*first_video_done) {
            info->width = width;
            info->height = height;
            *first_video_done = true;
        }
    } else if (handler == Fourcc("soun")) {
        info->has_audio = true;
    }
}

void ParseMoov(const unsigned char *p, size_t size, Mp4Info *info) {
    bool first_video_done = false;
    for (size_t pos = 0; pos < size;) {
        Box box;
        if (!Header(p + pos, size - pos, &box)) return;
        const size_t length = box.size ? static_cast<size_t>(box.size) : size - pos;
        if (length < box.header || length > size - pos) return;
        const unsigned char *body = p + pos + box.header;
        const size_t body_size = length - box.header;
        if (box.type == Fourcc("mvhd") && body_size >= 20) {
            uint32_t timescale;
            uint64_t duration;
            if (body[0] == 1 && body_size >= 32) {
                timescale = Be32(body + 20);
                duration = Be64(body + 24);
            } else {
                timescale = Be32(body + 12);
                duration = Be32(body + 16);
            }
            if (timescale) {
                const uint64_t ms = duration * 1000 / timescale;
                info->duration_ms = ms > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(ms);
            }
        } else if (box.type == Fourcc("trak")) {
            ParseTrak(body, body_size, info, &first_video_done);
        }
        pos += length;
    }
}
} // namespace

bool Mp4ProbeBytes(const unsigned char *data, size_t size, Mp4Info *info) {
    if (!info) return false;
    *info = {};
    if (!data || size < 16) return false;
    Box first;
    if (!Header(data, size, &first)) return true;
    // A lone `moov` box is what Mp4Probe hands over after seeking past the media data.
    if (first.type == Fourcc("moov")) {
        const size_t length = first.size ? static_cast<size_t>(first.size) : size;
        info->container = true;
        if (length >= first.header && length <= size) ParseMoov(data + first.header, length - first.header, info);
        return true;
    }
    if (first.type != Fourcc("ftyp")) return true;
    info->container = true;
    for (size_t pos = 0; pos < size;) {
        Box box;
        if (!Header(data + pos, size - pos, &box)) break;
        const size_t length = box.size ? static_cast<size_t>(box.size) : size - pos;
        if (length < box.header) break;
        if (box.type == Fourcc("moov")) {
            const size_t end = length > size - pos ? size : pos + length;
            ParseMoov(data + pos + box.header, end - pos - box.header, info);
            break;
        }
        if (length > size - pos) break;
        pos += length;
    }
    return true;
}

bool Mp4Probe(const char *path, Mp4Info *info) {
    if (!info) return false;
    *info = {};
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    unsigned char head[16];
    if (fread(head, 1, sizeof(head), file) != sizeof(head)) {
        fclose(file);
        return true;
    }
    Box first;
    if (!Header(head, sizeof(head), &first) || first.type != Fourcc("ftyp")) {
        fclose(file);
        return true;
    }
    // Walk the top-level boxes by seeking: the `moov` box is often behind a large `mdat`.
    uint64_t offset = 0;
    bool ok = true;
    for (int guard = 0; guard < 64; ++guard) {
        if (fseeko(file, static_cast<off_t>(offset), SEEK_SET)) break;
        unsigned char header[16];
        const size_t got = fread(header, 1, sizeof(header), file);
        Box box;
        if (got < 8 || !Header(header, got, &box)) break;
        if (box.type == Fourcc("moov")) {
            uint64_t length = box.size;
            if (!length) {
                fseeko(file, 0, SEEK_END);
                length = static_cast<uint64_t>(ftello(file)) - offset;
            }
            if (length <= box.header || length > kMoovMax) break;
            auto *bytes = static_cast<unsigned char *>(malloc(static_cast<size_t>(length)));
            if (!bytes) {
                ok = false;
                break;
            }
            if (fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0 &&
                fread(bytes, 1, static_cast<size_t>(length), file) == length) {
                Mp4ProbeBytes(bytes, static_cast<size_t>(length), info);
            }
            free(bytes);
            break;
        }
        if (!box.size) break;
        offset += box.size;
    }
    fclose(file);
    info->container = true; // the head had an ftyp box
    return ok;
}
} // namespace satori
