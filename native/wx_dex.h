#pragma once
#include <stddef.h>
#include <stdint.h>

// Read-only view of the dex files inside an APK, for the name-discovery rules (wx_derive.cpp).
//
// The APK is 400 MB on disk and its dex files hold 180 MB when inflated, so nothing is kept: the zip
// directory is read once, and a dex file is inflated on first use into a small cache (the least
// recently used one is freed). All access goes through offsets into that cache, so a caller must not
// hold a pointer from one File across a Load() of a different dex.
//
// This file is also built for the host (tests, tools/wxnames_check), so it uses libc only: no android
// logging, no JNI, no C++ library. zlib is opened at runtime (the module may not add libz to its
// dependencies), which the host build does too.
namespace satori {
namespace dex {

struct File {
    uint8_t *data = nullptr;
    size_t size = 0;
    uint32_t strings_off = 0, strings_count = 0;
    uint32_t types_off = 0, types_count = 0;
    uint32_t protos_off = 0, protos_count = 0;
    uint32_t fields_off = 0, fields_count = 0;
    uint32_t methods_off = 0, methods_count = 0;
    uint32_t classes_off = 0, classes_count = 0;
};

// A class definition as stored in the class_defs table.
struct ClassDef {
    uint32_t class_idx = 0;
    uint32_t access = 0;
    uint32_t super_idx = 0; // 0xffffffff: none
    uint32_t ifaces_off = 0;
    uint32_t class_data_off = 0;
    uint32_t static_values_off = 0;
};

// The class_defs entry at this index, and the number of interfaces it names (0 when malformed).
bool ReadClassDef(const File &f, uint32_t index, ClassDef *out, uint32_t *iface_count);
// Interface type index k (0 <= k < count from ReadClassDef).
uint32_t InterfaceType(const File &f, const ClassDef &def, uint32_t k);

const char *String(const File &f, uint32_t string_idx);
const char *Type(const File &f, uint32_t type_idx);
uint32_t TypeString(const File &f, uint32_t type_idx);

// Method and field id tables.
uint16_t MethodClass(const File &f, uint32_t idx);
uint16_t MethodProto(const File &f, uint32_t idx);
uint32_t MethodName(const File &f, uint32_t idx);
uint16_t FieldClass(const File &f, uint32_t idx);
uint16_t FieldType(const File &f, uint32_t idx);
uint32_t FieldName(const File &f, uint32_t idx);

// A prototype as the rules spell it: "(" + parameter type descriptors + ")" + return descriptor.
// Fails when it does not fit.
bool ProtoString(const File &f, uint32_t proto_idx, char *out, size_t size);
uint32_t ProtoReturnType(const File &f, uint32_t proto_idx);
uint32_t ProtoParamCount(const File &f, uint32_t proto_idx);
uint32_t ProtoParamType(const File &f, uint32_t proto_idx, uint32_t k);

// The members a class definition declares. A field callback gets (field_idx, flags); a method
// callback gets (method_idx, flags, code_off). Both are called for static and instance members
// alike; the flags say which. code_off is 0 for abstract and native methods.
typedef void (*FieldFn)(void *ctx, uint32_t field_idx, uint32_t flags);
typedef void (*MethodFn)(void *ctx, uint32_t method_idx, uint32_t flags, uint32_t code_off);
void Fields(const File &f, const ClassDef &def, void *ctx, FieldFn fn);
void Methods(const File &f, const ClassDef &def, void *ctx, MethodFn fn);

// The string constants a code item loads. field_idx is the field an iput-object stores that
// constant into when there is one close behind it (that is how the rules tell which field holds
// "msg_raw_img_send" without trusting the field name), or -1.
typedef void (*StringLoadFn)(void *ctx, uint32_t string_idx, int32_t field_idx);
void CodeStrings(const File &f, uint32_t code_off, void *ctx, StringLoadFn fn);

// The APK classes*.dex entries, inflated on demand.
class Apk {
public:
    Apk() = default;
    ~Apk();
    Apk(const Apk &) = delete;
    Apk &operator=(const Apk &) = delete;

    // Opens the file and reads its zip central directory. error (optional) says why it failed.
    bool Open(const char *path, char *error, size_t size);
    void Close();

    int Count() const { return count_; }
    const char *Name(int index) const;
    // The inflated dex, or null when the file is damaged. May free another dex.
    const File *Load(int index);

    struct Entry {
        char name[20];
        uint32_t local_off = 0;
        uint32_t compressed = 0;
        uint32_t uncompressed = 0;
        uint16_t method = 0;
    };

private:
    void Release(int slot);

    int fd_ = -1;
    uint64_t file_size_ = 0;
    Entry *entries_ = nullptr;
    int count_ = 0;
    File slots_[3];
    int slot_index_[3] = {-1, -1, -1};
    uint64_t slot_used_[3] = {0, 0, 0};
    size_t slot_bytes_ = 0;
    uint64_t clock_ = 0;
};

} // namespace dex
} // namespace satori
