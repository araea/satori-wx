#include "wx_dex.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace satori {
namespace dex {
namespace {

// -- zlib, opened at run time ----------------------------------------------------------------------

// The parts of zlib the inflater needs. This layout has been stable for decades, and opening the
// library with dlopen keeps libz out of the module ELF dependencies (the Zygisk module may depend on
// libc, libm, libdl and liblog only; see tools/check-native.py).
typedef void *(*AllocFn)(void *opaque, unsigned int items, unsigned int size);
typedef void (*FreeFn)(void *opaque, void *address);
struct ZStream {
    const uint8_t *next_in;
    unsigned int avail_in;
    unsigned long total_in;
    uint8_t *next_out;
    unsigned int avail_out;
    unsigned long total_out;
    const char *msg;
    void *state;
    AllocFn zalloc;
    FreeFn zfree;
    void *opaque;
    int data_type;
    unsigned long adler;
    unsigned long reserved;
};
typedef int (*InitFn)(ZStream *, int, const char *, int);
typedef int (*InflateFn)(ZStream *, int);
typedef int (*EndFn)(ZStream *);
typedef const char *(*VersionFn)(void);

InitFn g_init = nullptr;
InflateFn g_inflate = nullptr;
EndFn g_end = nullptr;
const char *g_version = "";

bool LoadZlib() {
    if (g_inflate) return true;
    void *lib = dlopen("libz.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return false;
    VersionFn version = reinterpret_cast<VersionFn>(dlsym(lib, "zlibVersion"));
    InitFn init = reinterpret_cast<InitFn>(dlsym(lib, "inflateInit2_"));
    InflateFn inflate = reinterpret_cast<InflateFn>(dlsym(lib, "inflate"));
    EndFn end = reinterpret_cast<EndFn>(dlsym(lib, "inflateEnd"));
    if (!version || !init || !inflate || !end) return false;
    g_version = version(); // inflateInit2_ checks its stream against this ABI version
    g_init = init;
    g_inflate = inflate;
    g_end = end;
    return true;
}

bool InflateRaw(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size, size_t *written) {
    if (!LoadZlib()) return false;
    ZStream stream{};
    stream.next_in = in;
    stream.avail_in = static_cast<unsigned int>(in_size);
    stream.next_out = out;
    stream.avail_out = static_cast<unsigned int>(out_size);
    if (g_init(&stream, -15, g_version, sizeof(ZStream)) != 0) return false;
    const int rc = g_inflate(&stream, 4 /* Z_FINISH */);
    *written = out_size - stream.avail_out;
    g_end(&stream);
    return rc == 1 /* Z_STREAM_END */;
}

uint16_t U16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint32_t Uleb(const uint8_t *&p, const uint8_t *end) {
    uint32_t value = 0;
    int shift = 0;
    while (p < end) {
        const uint8_t byte = *p++;
        value |= static_cast<uint32_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
    }
    return value;
}

// Instruction width in 16-bit code units, indexed by opcode (same table as tools/dexlib.py).
uint8_t Width(uint8_t op) {
    static uint8_t table[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; ++i) table[i] = 1;
        const struct {
            uint8_t op;
            uint8_t width;
        } fixed[] = {{0x02, 2}, {0x03, 3}, {0x05, 2}, {0x06, 3}, {0x08, 2}, {0x09, 3}, {0x13, 2}, {0x14, 3}, {0x15, 2},
                     {0x16, 2}, {0x17, 3}, {0x18, 5}, {0x19, 2}, {0x1a, 2}, {0x1b, 3}, {0x1c, 2}, {0x1f, 2}, {0x20, 2},
                     {0x22, 2}, {0x23, 2}, {0x24, 3}, {0x25, 3}, {0x26, 3}, {0x29, 2}, {0x2a, 3}, {0x2b, 3}, {0x2c, 3}};
        for (const auto &item : fixed) table[item.op] = item.width;
        for (int op = 0x2d; op <= 0x3d; ++op) table[op] = 2;
        for (int op = 0x44; op <= 0x6d; ++op) table[op] = 2;
        for (int op = 0x6e; op <= 0x72; ++op) table[op] = 3;
        for (int op = 0x74; op <= 0x78; ++op) table[op] = 3;
        for (int op = 0x90; op <= 0xaf; ++op) table[op] = 2;
        for (int op = 0xd0; op <= 0xe2; ++op) table[op] = 2;
        table[0xfa] = table[0xfb] = 4;
        table[0xfc] = table[0xfd] = 3;
        table[0xfe] = table[0xff] = 2;
        ready = true;
    }
    return table[op];
}

// The dex tables must fit inside the inflated file; a damaged file yields empty tables instead of
// reads past its end.
bool Bounds(const File &f, uint32_t offset, uint32_t count, uint32_t item_size) {
    if (!count) return true;
    if (offset > f.size) return false;
    return static_cast<uint64_t>(offset) + static_cast<uint64_t>(count) * item_size <= f.size;
}

template <typename OnField, typename OnMethod>
void Members(const File &f, const ClassDef &def, OnField on_field, OnMethod on_method) {
    if (!def.class_data_off || def.class_data_off >= f.size) return;
    const uint8_t *p = f.data + def.class_data_off;
    const uint8_t *end = f.data + f.size;
    const uint32_t static_fields = Uleb(p, end);
    const uint32_t instance_fields = Uleb(p, end);
    const uint32_t direct_methods = Uleb(p, end);
    const uint32_t virtual_methods = Uleb(p, end);
    // Static and instance fields are two separately sorted lists, and each is diff-encoded from 0
    // (the same holds for direct and virtual methods). A single running index would run past the
    // table: WeChat's dex files rely on the reset.
    for (int part = 0; part < 2; ++part) {
        uint32_t index = 0;
        const uint32_t count = part == 0 ? static_fields : instance_fields;
        for (uint32_t i = 0; i < count && p < end; ++i) {
            index += Uleb(p, end);
            const uint32_t flags = Uleb(p, end);
            on_field(index, flags);
        }
    }
    for (int part = 0; part < 2; ++part) {
        uint32_t index = 0;
        const uint32_t count = part == 0 ? direct_methods : virtual_methods;
        for (uint32_t i = 0; i < count && p < end; ++i) {
            index += Uleb(p, end);
            const uint32_t flags = Uleb(p, end);
            const uint32_t code_off = Uleb(p, end);
            on_method(index, flags, code_off);
        }
    }
}

} // namespace

bool ReadClassDef(const File &f, uint32_t index, ClassDef *out, uint32_t *iface_count) {
    if (!out || index >= f.classes_count || !Bounds(f, f.classes_off, index + 1, 32)) return false;
    const uint8_t *p = f.data + f.classes_off + index * 32;
    out->class_idx = U32(p);
    out->access = U32(p + 4);
    out->super_idx = U32(p + 8);
    out->ifaces_off = U32(p + 12);
    out->class_data_off = U32(p + 24);
    out->static_values_off = U32(p + 28);
    uint32_t count = 0;
    if (out->ifaces_off && static_cast<uint64_t>(out->ifaces_off) + 4 <= f.size) count = U32(f.data + out->ifaces_off);
    if (iface_count) *iface_count = count;
    return true;
}

uint32_t InterfaceType(const File &f, const ClassDef &def, uint32_t k) {
    const uint64_t at = static_cast<uint64_t>(def.ifaces_off) + 4 + static_cast<uint64_t>(k) * 2;
    if (!def.ifaces_off || at + 2 > f.size) return 0;
    return U16(f.data + at);
}

const char *String(const File &f, uint32_t string_idx) {
    if (string_idx >= f.strings_count || !Bounds(f, f.strings_off, string_idx + 1, 4)) return "";
    const uint32_t at = U32(f.data + f.strings_off + string_idx * 4);
    if (!at || at >= f.size) return "";
    const uint8_t *p = f.data + at;
    const uint8_t *end = f.data + f.size;
    Uleb(p, end); // UTF-16 length, not the byte length
    const void *nul = memchr(p, 0, static_cast<size_t>(end - p));
    return nul ? reinterpret_cast<const char *>(p) : "";
}

uint32_t TypeString(const File &f, uint32_t type_idx) {
    if (type_idx >= f.types_count || !Bounds(f, f.types_off, type_idx + 1, 4)) return 0;
    return U32(f.data + f.types_off + type_idx * 4);
}

const char *Type(const File &f, uint32_t type_idx) { return String(f, TypeString(f, type_idx)); }

uint16_t MethodClass(const File &f, uint32_t idx) {
    return idx < f.methods_count ? U16(f.data + f.methods_off + idx * 8) : 0;
}
uint16_t MethodProto(const File &f, uint32_t idx) {
    return idx < f.methods_count ? U16(f.data + f.methods_off + idx * 8 + 2) : 0;
}
uint32_t MethodName(const File &f, uint32_t idx) {
    return idx < f.methods_count ? U32(f.data + f.methods_off + idx * 8 + 4) : 0;
}
uint16_t FieldClass(const File &f, uint32_t idx) {
    return idx < f.fields_count ? U16(f.data + f.fields_off + idx * 8) : 0;
}
uint16_t FieldType(const File &f, uint32_t idx) {
    return idx < f.fields_count ? U16(f.data + f.fields_off + idx * 8 + 2) : 0;
}
uint32_t FieldName(const File &f, uint32_t idx) {
    return idx < f.fields_count ? U32(f.data + f.fields_off + idx * 8 + 4) : 0;
}

uint32_t ProtoReturnType(const File &f, uint32_t proto_idx) {
    if (proto_idx >= f.protos_count) return 0;
    return U32(f.data + f.protos_off + proto_idx * 12 + 4);
}

uint32_t ProtoParamCount(const File &f, uint32_t proto_idx) {
    if (proto_idx >= f.protos_count) return 0;
    const uint32_t at = U32(f.data + f.protos_off + proto_idx * 12 + 8);
    if (!at || static_cast<uint64_t>(at) + 4 > f.size) return 0;
    return U32(f.data + at);
}

uint32_t ProtoParamType(const File &f, uint32_t proto_idx, uint32_t k) {
    if (proto_idx >= f.protos_count || k >= ProtoParamCount(f, proto_idx)) return 0;
    const uint32_t at = U32(f.data + f.protos_off + proto_idx * 12 + 8);
    const uint64_t item = static_cast<uint64_t>(at) + 4 + static_cast<uint64_t>(k) * 2;
    return item + 2 <= f.size ? U16(f.data + item) : 0;
}

bool ProtoString(const File &f, uint32_t proto_idx, char *out, size_t size) {
    if (!out || !size) return false;
    size_t used = 0;
    out[0] = 0;
    auto append = [&](const char *text) {
        const size_t n = strlen(text);
        if (used + n + 1 > size) return false;
        memcpy(out + used, text, n);
        used += n;
        out[used] = 0;
        return true;
    };
    if (!append("(")) return false;
    const uint32_t count = ProtoParamCount(f, proto_idx);
    for (uint32_t k = 0; k < count; ++k)
        if (!append(Type(f, ProtoParamType(f, proto_idx, k)))) return false;
    if (!append(")")) return false;
    return append(Type(f, ProtoReturnType(f, proto_idx)));
}

void Fields(const File &f, const ClassDef &def, void *ctx, FieldFn fn) {
    if (!fn) return;
    Members(f, def, [&](uint32_t idx, uint32_t flags) { fn(ctx, idx, flags); }, [](uint32_t, uint32_t, uint32_t) {});
}

void Methods(const File &f, const ClassDef &def, void *ctx, MethodFn fn) {
    if (!fn) return;
    Members(
        f, def, [](uint32_t, uint32_t) {},
        [&](uint32_t idx, uint32_t flags, uint32_t code) { fn(ctx, idx, flags, code); });
}

void CodeStrings(const File &f, uint32_t code_off, void *ctx, StringLoadFn fn) {
    if (!fn || !code_off || static_cast<uint64_t>(code_off) + 16 > f.size) return;
    const uint32_t units = U32(f.data + code_off + 12);
    if (static_cast<uint64_t>(code_off) + 16 + static_cast<uint64_t>(units) * 2 > f.size) return;
    const uint8_t *base = f.data + code_off + 16;

    struct Load {
        uint32_t string_idx;
        int32_t field_idx;
    } loads[512];
    struct Pending {
        int load;
        int ttl;
    } pending[256] = {}; // indexed by the destination register
    uint32_t count = 0;
    for (uint32_t i = 0; i < units;) {
        const uint8_t *insn = base + i * 2;
        const uint8_t op = insn[0];
        for (auto &entry : pending)
            if (entry.ttl > 0 && --entry.ttl == 0) entry.load = -1;
        if (op == 0 && insn[1] != 0) {
            const uint8_t kind = insn[1];
            uint32_t width = 1;
            if (kind == 1)
                width = 4 + U16(insn + 2) * 2;
            else if (kind == 2)
                width = 2 + U16(insn + 2) * 4;
            else if (kind == 3)
                width = 4 + ((U32(insn + 4) * U16(insn + 2) + 1) / 2) * 2;
            i += width;
            continue;
        }
        if (op == 0x1a || op == 0x1b) {
            const uint32_t string_idx = op == 0x1a ? U16(insn + 2) : U32(insn + 2);
            const uint8_t reg = insn[1];
            if (count < 512) {
                loads[count].string_idx = string_idx;
                loads[count].field_idx = -1;
                pending[reg].load = static_cast<int>(count);
                pending[reg].ttl = 7;
                ++count;
            }
        } else if (op == 0x5b) { // iput-object vA, vB, field@CCCC
            const uint8_t reg = insn[1] & 0x0f;
            if (pending[reg].ttl > 0 && pending[reg].load >= 0) {
                loads[pending[reg].load].field_idx = static_cast<int32_t>(U16(insn + 2));
                pending[reg].ttl = 0;
                pending[reg].load = -1;
            }
        }
        i += Width(op);
    }
    for (uint32_t k = 0; k < count; ++k) fn(ctx, loads[k].string_idx, loads[k].field_idx);
}

namespace {

// Three dex files at most (the largest is 14 MB): enough for the anchor pass, which walks one file at a
// time, and for the member lookups of one rule step, which stay within a package.
constexpr size_t kCacheBytes = size_t{56} << 20;

bool DexEntryName(const char *name, size_t length) {
    // classes.dex, classes2.dex ... classes18.dex
    if (length < 11 || length > 19) return false;
    if (memcmp(name, "classes", 7) != 0) return false;
    size_t i = 7;
    while (i < length - 4 && name[i] >= 0x30 && name[i] <= 0x39) ++i;
    return i == length - 4 && memcmp(name + length - 4, ".dex", 4) == 0;
}

int CompareEntries(const void *a, const void *b) {
    return strcmp(static_cast<const Apk::Entry *>(a)->name, static_cast<const Apk::Entry *>(b)->name);
}

bool ReadAt(int fd, void *out, size_t size, uint64_t offset) {
    uint8_t *p = static_cast<uint8_t *>(out);
    while (size) {
        const ssize_t n = pread(fd, p, size, static_cast<off_t>(offset));
        if (n <= 0) return false;
        p += n;
        offset += static_cast<uint64_t>(n);
        size -= static_cast<size_t>(n);
    }
    return true;
}

} // namespace

Apk::~Apk() { Close(); }

void Apk::Release(int slot) {
    if (slot < 0 || slot >= 3) return;
    free(slots_[slot].data);
    slots_[slot] = File{};
    slot_index_[slot] = -1;
    slot_used_[slot] = 0;
}

void Apk::Close() {
    for (int slot = 0; slot < 3; ++slot) Release(slot);
    slot_bytes_ = 0;
    free(entries_);
    entries_ = nullptr;
    count_ = 0;
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    file_size_ = 0;
}

const char *Apk::Name(int index) const { return index >= 0 && index < count_ ? entries_[index].name : ""; }

bool Apk::Open(const char *path, char *error, size_t size) {
    Close();
    auto fail = [&](const char *what) {
        if (error && size) snprintf(error, size, "%s: %s", what, path);
        Close();
        return false;
    };
    fd_ = open(path, O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) return fail("cannot open");
    struct stat st{};
    if (fstat(fd_, &st) != 0 || st.st_size < 22) return fail("cannot stat");
    const uint64_t file_size = static_cast<uint64_t>(st.st_size);
    file_size_ = file_size;
    const size_t tail = file_size < 65557 + 22 ? static_cast<size_t>(file_size) : 65557 + 22;
    uint8_t *buffer = static_cast<uint8_t *>(malloc(tail));
    if (!buffer) return fail("out of memory");
    if (!ReadAt(fd_, buffer, tail, file_size - tail)) {
        free(buffer);
        return fail("cannot read the zip directory");
    }
    // The end of central directory record is somewhere in the last 64 KB (its comment field is 16 bits).
    long eocd = -1;
    for (long i = static_cast<long>(tail) - 22; i >= 0; --i) {
        if (buffer[i] == 0x50 && buffer[i + 1] == 0x4b && buffer[i + 2] == 0x05 && buffer[i + 3] == 0x06) {
            const uint32_t comment = U16(buffer + i + 20);
            if (i + 22 + comment <= static_cast<long>(tail)) {
                eocd = i;
                break;
            }
        }
    }
    if (eocd < 0) {
        free(buffer);
        return fail("not a zip file");
    }
    const uint32_t entry_count = U16(buffer + eocd + 10);
    const uint32_t directory_size = U32(buffer + eocd + 12);
    const uint32_t directory_off = U32(buffer + eocd + 16);
    free(buffer);
    if (static_cast<uint64_t>(directory_off) + directory_size > file_size) return fail("damaged zip directory");
    if (!entry_count) return fail("no dex files");
    uint8_t *directory = static_cast<uint8_t *>(malloc(directory_size));
    entries_ = static_cast<Entry *>(calloc(entry_count, sizeof(Entry)));
    if (!directory || !entries_) {
        free(directory);
        return fail("out of memory");
    }
    if (!ReadAt(fd_, directory, directory_size, directory_off)) {
        free(directory);
        return fail("cannot read the zip directory");
    }
    const uint8_t *p = directory;
    const uint8_t *end = directory + directory_size;
    for (uint32_t i = 0; i < entry_count && p + 46 <= end; ++i) {
        if (p[0] != 0x50 || p[1] != 0x4b || p[2] != 0x01 || p[3] != 0x02) break;
        const uint16_t method = U16(p + 10);
        const uint32_t compressed = U32(p + 20);
        const uint32_t uncompressed = U32(p + 24);
        const uint16_t name_length = U16(p + 28);
        const uint16_t extra_length = U16(p + 30);
        const uint16_t comment_length = U16(p + 32);
        const uint32_t local_off = U32(p + 42);
        const char *name = reinterpret_cast<const char *>(p + 46);
        if (p + 46 + name_length > end) break;
        if (DexEntryName(name, name_length) && count_ < static_cast<int>(entry_count)) {
            Entry &entry = entries_[count_];
            const size_t copy = name_length < sizeof(entry.name) - 1 ? name_length : sizeof(entry.name) - 1;
            memcpy(entry.name, name, copy);
            entry.name[copy] = 0;
            entry.local_off = local_off;
            entry.compressed = compressed;
            entry.uncompressed = uncompressed;
            entry.method = method;
            ++count_;
        }
        p += 46 + name_length + extra_length + comment_length;
    }
    free(directory);
    if (!count_) return fail("no dex files");
    qsort(entries_, static_cast<size_t>(count_), sizeof(Entry), CompareEntries);
    return true;
}

const File *Apk::Load(int index) {
    if (index < 0 || index >= count_) return nullptr;
    for (int slot = 0; slot < 3; ++slot)
        if (slot_index_[slot] == index) {
            slot_used_[slot] = ++clock_;
            return &slots_[slot];
        }
    int slot = 0;
    for (int s = 0; s < 3; ++s) {
        if (slot_index_[s] < 0) {
            slot = s;
            break;
        }
        if (slot_index_[slot] >= 0 && slot_used_[s] < slot_used_[slot]) slot = s;
    }
    if (slot_index_[slot] >= 0) {
        slot_bytes_ -= slots_[slot].size;
        Release(slot);
    }
    const Entry &entry = entries_[index];
    // Make room before reading, so a 14 MB dex never sits next to three others.
    while (slot_bytes_ + entry.uncompressed > kCacheBytes) {
        int victim = -1;
        for (int s = 0; s < 3; ++s) {
            if (s == slot || slot_index_[s] < 0) continue;
            if (victim < 0 || slot_used_[s] < slot_used_[victim]) victim = s;
        }
        if (victim < 0) break;
        slot_bytes_ -= slots_[victim].size;
        Release(victim);
    }
    uint8_t header[30];
    if (!ReadAt(fd_, header, sizeof(header), entry.local_off)) return nullptr;
    if (header[0] != 0x50 || header[1] != 0x4b || header[2] != 0x03 || header[3] != 0x04) return nullptr;
    const uint64_t data_off = static_cast<uint64_t>(entry.local_off) + 30 + U16(header + 26) + U16(header + 28);
    if (data_off + entry.compressed > file_size_) return nullptr;
    uint8_t *data = static_cast<uint8_t *>(malloc(entry.uncompressed ? entry.uncompressed : 1));
    if (!data) return nullptr;
    if (entry.method == 0) {
        if (entry.compressed != entry.uncompressed || !ReadAt(fd_, data, entry.uncompressed, data_off)) {
            free(data);
            return nullptr;
        }
    } else if (entry.method == 8) {
        uint8_t *packed = static_cast<uint8_t *>(malloc(entry.compressed ? entry.compressed : 1));
        if (!packed || !ReadAt(fd_, packed, entry.compressed, data_off)) {
            free(packed);
            free(data);
            return nullptr;
        }
        size_t written = 0;
        const bool ok =
            InflateRaw(packed, entry.compressed, data, entry.uncompressed, &written) && written == entry.uncompressed;
        free(packed);
        if (!ok) {
            free(data);
            return nullptr;
        }
    } else {
        free(data);
        return nullptr;
    }
    if (entry.uncompressed < 0x70 || memcmp(data, "dex\n", 4) != 0) {
        free(data);
        return nullptr;
    }
    File &file = slots_[slot];
    file.data = data;
    file.size = entry.uncompressed;
    file.strings_count = U32(data + 0x38);
    file.strings_off = U32(data + 0x3c);
    file.types_count = U32(data + 0x40);
    file.types_off = U32(data + 0x44);
    file.protos_count = U32(data + 0x48);
    file.protos_off = U32(data + 0x4c);
    file.fields_count = U32(data + 0x50);
    file.fields_off = U32(data + 0x54);
    file.methods_count = U32(data + 0x58);
    file.methods_off = U32(data + 0x5c);
    file.classes_count = U32(data + 0x60);
    file.classes_off = U32(data + 0x64);
    const bool tables_ok =
        Bounds(file, file.strings_off, file.strings_count, 4) && Bounds(file, file.types_off, file.types_count, 4) &&
        Bounds(file, file.protos_off, file.protos_count, 12) && Bounds(file, file.fields_off, file.fields_count, 8) &&
        Bounds(file, file.methods_off, file.methods_count, 8) && Bounds(file, file.classes_off, file.classes_count, 32);
    if (!tables_ok) {
        file = File{};
        free(data);
        return nullptr;
    }
    slot_index_[slot] = index;
    slot_used_[slot] = ++clock_;
    slot_bytes_ += file.size;
    return &file;
}
} // namespace dex
} // namespace satori
