#include "wx_derive.h"

#include "wx_dex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace satori {
namespace {

constexpr int kMaxHits = 8192;
constexpr int kHashBits = 19;
constexpr int kHashSize = 1 << kHashBits;
constexpr int kMaxClasses = 400000;
constexpr int kMaxCandidates = 64;
constexpr int kMaxScratch = 512;

// One class definition in the APK: the descriptor is owned by the arena, as are the super class
// descriptor and the interface list, so nothing points into a dex file that may have been evicted.
struct ClassEntry {
    const char *desc = nullptr;
    const char *super = nullptr;
    const char **ifaces = nullptr;
    uint8_t iface_count = 0;
    int16_t dex = -1;
    uint32_t def = 0;
};

struct Mem {
    int cls;
    char name[96];
    char desc[256];
    uint32_t flags;
};

struct Hit {
    int anchor;
    const char *cls;
    const char *method;
    const char *proto;
    const char *field; // field an iput-object stores the constant into, or null
};

#ifdef WX_DERIVE_DEBUG
#define TRACE(...) fprintf(stderr, __VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

uint32_t Hash(const char *text) {
    uint32_t h = 2166136261u;
    while (*text) {
        h ^= static_cast<uint8_t>(*text++);
        h *= 16777619u;
    }
    return h;
}

void CopyTo(char *out, size_t size, const char *text) {
    if (!size) return;
    snprintf(out, size, "%s", text ? text : "");
}

bool StartsWith(const char *text, const char *prefix) { return strncmp(text, prefix, strlen(prefix)) == 0; }

// True when the prototype matches the pattern, where "L*;" stands for any class type. Everything else
// is literal (same rule as tools/wxnames.py shape()).
bool ShapeMatch(const char *pattern, const char *text) {
    const char *p = pattern;
    const char *t = text;
    while (*p) {
        if (p[0] == 'L' && p[1] == '*' && p[2] == ';') {
            if (*t != 'L') return false;
            const char *end = strchr(t, ';');
            if (!end) return false;
            t = end + 1;
            p += 3;
            continue;
        }
        if (*t != *p) return false;
        ++t;
        ++p;
    }
    return *t == 0;
}

// Parameter type descriptors of a prototype, up to `max`.
int Params(const char *proto, char out[][160], int max) {
    int count = 0;
    const char *p = proto;
    if (*p != '(') return 0;
    ++p;
    while (*p && *p != ')' && count < max) {
        const char *start = p;
        if (*p == '[') {
            while (*p == '[') ++p;
            if (*p == 'L') p = strchr(p, ';');
            if (p) ++p;
        } else if (*p == 'L') {
            p = strchr(p, ';');
            if (p) ++p;
        } else {
            ++p;
        }
        if (!p) break;
        const size_t n = static_cast<size_t>(p - start);
        const size_t copy = n < sizeof(out[0]) - 1 ? n : sizeof(out[0]) - 1;
        memcpy(out[count], start, copy);
        out[count][copy] = 0;
        ++count;
    }
    return count;
}

const char *Ret(const char *proto) {
    const char *end = strchr(proto, ')');
    return end ? end + 1 : "";
}

// The class part of a descriptor: "Lcom/x/Y;" -> "com.x.Y".
void Dotted(const char *desc, char *out, size_t size) {
    if (!size) return;
    if (*desc == 'L') ++desc;
    size_t n = 0;
    for (; *desc && *desc != ';' && n + 1 < size; ++desc) out[n++] = *desc == '/' ? '.' : *desc;
    out[n] = 0;
}

void Descriptor(const char *dotted, char *out, size_t size) {
    if (!size) return;
    snprintf(out, size, "L%s;", dotted ? dotted : "");
    for (char *p = out; *p; ++p)
        if (*p == '.') *p = '/';
}

// Strings the rules start from. The order is part of the file format of the hits array, so keep it.
enum Anchor {
    kCgi,
    kSendMsg,
    kRunOk,
    kAutoAuth,
    kNetQueue,
    kRevoke,
    kServiceManager,
    kBizHalf,
    kImgService,
    kRawImg,
    kForwardInfo,
    kAppMsg,
    kVideoService,
    kSubCoreVoice,
    kVoiceService,
    kC2CFile,
    kRoomKick,
    kRoomAdminAdd,
    kRoomAdminDel,
    kRunCgi,
    kSendPat,
    kPatExtension,
    kAnchorCount
};

const char *const kAnchors[kAnchorCount] = {
    "MicroMsg.Cgi",
    "MicroMsg.NetSceneSendMsg",
    "[+] Run successfully: %s, funcId: %s",
    "[+] MMAutoAuth was injected, run this scene now: %s, funcId: %s",
    "MicroMsg.NetSceneQueue",
    "/cgi-bin/micromsg-bin/revokemsg",
    "MicroMsg.ServiceManager",
    "MicroMsg.BizHalfScreenChattingUtil",
    "MicroMsg.ImgUpload.MsgImgFeatureService",
    "msg_raw_img_send",
    "ForwardInfo{forwardFromImgId=",
    "MicroMsg.AppMsgLogic",
    "MicroMsg.VideoMsg.VideoMsgSendFeatureService",
    "MicroMsg.SubCoreVoice",
    "MicroMsg.SceneVoiceService",
    "MicroMsg.C2CFileFeatureService",
    "/cgi-bin/micromsg-bin/delchatroommember",
    "/cgi-bin/micromsg-bin/addchatroomadmin",
    "/cgi-bin/micromsg-bin/delchatroomadmin",
    "MicroMsg.RunCgi",
    "/cgi-bin/micromsg-bin/sendpat",
    "MicroMsg.PatMsgExtension",
};

// Arena for the strings and lists the index keeps for the life of the run.
class Arena {
public:
    ~Arena() {
        while (head_) {
            Block *next = head_->next;
            free(head_);
            head_ = next;
        }
    }
    void *Alloc(size_t size, size_t align = 8) {
        size = (size + align - 1) & ~(align - 1);
        if (!head_ || head_->used + size > head_->size) {
            const size_t block = size > 65536 ? size : 262144;
            Block *next = static_cast<Block *>(malloc(sizeof(Block) + block));
            if (!next) return nullptr;
            next->next = head_;
            next->size = block;
            next->used = 0;
            head_ = next;
        }
        void *out = reinterpret_cast<uint8_t *>(head_) + sizeof(Block) + head_->used;
        head_->used += size;
        return out;
    }
    const char *Copy(const char *text) {
        const size_t n = strlen(text) + 1;
        char *out = static_cast<char *>(Alloc(n, 1));
        if (out) memcpy(out, text, n);
        return out;
    }

private:
    struct Block {
        Block *next;
        size_t size;
        size_t used;
    };
    Block *head_ = nullptr;
};

} // namespace
namespace {

// The rule engine. Its index is built once per APK, then the rules run in dependency order, each
// filling the keys it owns. A rule that cannot find its anchor leaves its keys empty and says why in
// `reason_` (reported by the host tool; the module only cares whether the set is complete).
class Engine {
public:
    bool Build(const char *path, char *error, size_t size);
    void Run(const WxNames &hints);

    DerivedNames out;
    char reason[kWxKeyCount][96] = {};
    // Used by the anchor scan (OnStringLoad), which is a plain function so it can be a C callback.
    const char *Copy(const char *text) { return arena_.Copy(text ? text : ""); }
    void AddHit(int anchor, const char *cls, const char *method, const char *proto, const char *field);

private:
    // -- index ------------------------------------------------------------------------------------
    dex::Apk apk_;
    Arena arena_;
    ClassEntry *classes_ = nullptr;
    int class_count_ = 0;
    int32_t *buckets_ = nullptr;
    Hit hits_[kMaxHits];
    int hit_count_ = 0;
    const WxNames *hints_ = nullptr;
    // Member() may need every field of a class (the content class has ~190, many of them strings);
    // the hint picks one of them, so the list cannot be truncated at the usual small size.
    Mem scratch_[kMaxScratch];

    int Find(const char *desc) const;
    void AddClass(const ClassEntry &entry);
    void ScanAnchors(const dex::File &file);

    // -- queries ----------------------------------------------------------------------------------
    int Supers(int cls, int *out, int max) const;
    bool IsA(int cls, const char *target) const;
    bool LoadClass(int cls, const dex::File **file, dex::ClassDef *def);
    int MethodsOf(int cls, const char *name, const char *pattern, int want_static, Mem *out, int max);
    int FieldsOf(int cls, const char *type, int want_static, Mem *out, int max);
    int ByProto(const char *proto, int want_static, bool require_final, Mem *out, int max);
    int StaticFieldsOf(const char *type, Mem *out, int max);
    bool Ctor(int cls, const char *pattern, char *desc, size_t size);
    int ServiceOf(int impl, char *desc, size_t size);

    // -- results ----------------------------------------------------------------------------------
    void Set(WxKey key, const char *value);
    void Fail(WxKey key, const char *why);
    void SetClass(WxKey key, const char *desc);
    bool Member(WxKey key, WxKey desc_key, int cls, char kind, const char *pattern, int want_static, int fallback);
    const char *Pick(WxKey key, const char *const *names, int count, int fallback);

    // -- anchors ----------------------------------------------------------------------------------
    int AnchorCount(int anchor) const;
    const Hit &AnchorAt(int anchor, int index) const;
    int AnchorClasses(int anchor, int *out, int max) const;
    const char *AnchorClass(int anchor) const;

    // -- rules ------------------------------------------------------------------------------------
    void RuleSend();
    void RuleRecall();
    void RuleLocator();
    void RulePrepare();
    void RuleImage();
    void RuleAppMsg();
    void RuleVideo();
    void RuleVoice();
    void RuleRoom();
    void RulePat();
};

constexpr int kMaxSupers = 256;

int Engine::Find(const char *desc) const {
    if (!desc || !buckets_) return -1;
    uint32_t slot = Hash(desc) & (kHashSize - 1);
    for (int probe = 0; probe < 64; ++probe) {
        const int32_t value = buckets_[slot];
        if (!value) return -1;
        const int index = value - 1;
        if (!strcmp(classes_[index].desc, desc)) return index;
        slot = (slot + 1) & (kHashSize - 1);
    }
    return -1;
}

void Engine::AddClass(const ClassEntry &entry) {
    if (class_count_ >= kMaxClasses) return;
    if (Find(entry.desc) >= 0) return;
    uint32_t slot = Hash(entry.desc) & (kHashSize - 1);
    for (int probe = 0; probe < 64; ++probe) {
        if (!buckets_[slot]) {
            classes_[class_count_] = entry;
            buckets_[slot] = class_count_ + 1;
            ++class_count_;
            return;
        }
        slot = (slot + 1) & (kHashSize - 1);
    }
}

struct ScanCtx {
    Engine *engine;
    const dex::File *file;
    const char *cls;
    const char *method;
    const char *proto;
};

int AnchorOf(const char *text) {
    const uint32_t hash = Hash(text);
    for (int i = 0; i < kAnchorCount; ++i)
        if (Hash(kAnchors[i]) == hash && !strcmp(kAnchors[i], text)) return i;
    return -1;
}

void OnStringLoad(void *context, uint32_t string_idx, int32_t field_idx) {
    ScanCtx *ctx = static_cast<ScanCtx *>(context);
    const char *text = dex::String(*ctx->file, string_idx);
    if (!text || !*text) return;
    const int anchor = AnchorOf(text);
    if (anchor < 0) return;
    const char *field = nullptr;
    if (field_idx >= 0) {
        const char *name = dex::String(*ctx->file, dex::FieldName(*ctx->file, static_cast<uint32_t>(field_idx)));
        field = ctx->engine->Copy(name);
    }
    ctx->engine->AddHit(anchor, ctx->cls, ctx->method, ctx->proto, field);
}

void Engine::AddHit(int anchor, const char *cls, const char *method, const char *proto, const char *field) {
    if (hit_count_ >= kMaxHits) return;
    for (int i = 0; i < hit_count_; ++i) {
        const Hit &hit = hits_[i];
        if (hit.anchor == anchor && !strcmp(hit.cls, cls) && !strcmp(hit.method, method) && !strcmp(hit.proto, proto))
            return;
    }
    Hit &hit = hits_[hit_count_++];
    hit.anchor = anchor;
    hit.cls = Copy(cls);
    hit.method = Copy(method);
    hit.proto = Copy(proto);
    hit.field = field;
}

bool Engine::Build(const char *path, char *error, size_t size) {
    if (!apk_.Open(path, error, size)) return false;
    classes_ = static_cast<ClassEntry *>(arena_.Alloc(sizeof(ClassEntry) * kMaxClasses, 8));
    buckets_ = static_cast<int32_t *>(arena_.Alloc(sizeof(int32_t) * kHashSize, 4));
    if (!classes_ || !buckets_) {
        if (error && size) snprintf(error, size, "out of memory");
        return false;
    }
    memset(buckets_, 0, sizeof(int32_t) * kHashSize);
    for (int d = 0; d < apk_.Count(); ++d) {
        const dex::File *file = apk_.Load(d);
        if (!file) continue;
        for (uint32_t c = 0; c < file->classes_count; ++c) {
            dex::ClassDef def;
            uint32_t ifaces = 0;
            if (!dex::ReadClassDef(*file, c, &def, &ifaces)) continue;
            const char *desc = dex::Type(*file, def.class_idx);
            if (!desc || desc[0] != 'L') continue;
            ClassEntry entry;
            entry.desc = Copy(desc);
            entry.super = def.super_idx == 0xffffffff ? nullptr : Copy(dex::Type(*file, def.super_idx));
            if (ifaces) {
                const uint32_t keep = ifaces > 64 ? 64 : ifaces;
                const char **list = static_cast<const char **>(arena_.Alloc(sizeof(char *) * keep, 8));
                uint8_t n = 0;
                for (uint32_t k = 0; k < keep; ++k) {
                    const char *name = dex::Type(*file, dex::InterfaceType(*file, def, k));
                    if (name && name[0] == 'L') list[n++] = Copy(name);
                }
                entry.ifaces = list;
                entry.iface_count = n;
            }
            entry.dex = static_cast<int16_t>(d);
            entry.def = c;
            AddClass(entry);
        }
        ScanAnchors(*file);
    }
    return class_count_ > 0;
}

void Engine::ScanAnchors(const dex::File &file) {
    struct Ctx {
        Engine *engine;
        const dex::File *file;
        const char *cls;
    };
    for (uint32_t c = 0; c < file.classes_count; ++c) {
        dex::ClassDef def;
        if (!dex::ReadClassDef(file, c, &def, nullptr)) continue;
        const char *cls = dex::Type(file, def.class_idx);
        if (!cls || cls[0] != 'L') continue;
        Ctx outer{this, &file, cls};
        dex::Methods(file, def, &outer, [](void *p, uint32_t idx, uint32_t flags, uint32_t code) {
            (void)flags;
            if (!code) return;
            Ctx *o = static_cast<Ctx *>(p);
            ScanCtx scan{o->engine, o->file, o->cls, 0, 0};
            scan.method = dex::String(*o->file, dex::MethodName(*o->file, idx));
            char proto[256];
            if (!dex::ProtoString(*o->file, dex::MethodProto(*o->file, idx), proto, sizeof(proto))) return;
            scan.proto = proto;
            dex::CodeStrings(*o->file, code, &scan, OnStringLoad);
        });
    }
}

// -- queries --------------------------------------------------------------------------------------

struct FieldCtx {
    const dex::File *file;
    int cls;
    const char *type;
    int want_static;
    Mem *out;
    int count;
    int max;
};

void OnFieldQueried(void *p, uint32_t idx, uint32_t flags) {
    FieldCtx *ctx = static_cast<FieldCtx *>(p);
    if (ctx->want_static >= 0 && ((flags & 0x8) ? 1 : 0) != ctx->want_static) return;
    const char *type = dex::Type(*ctx->file, dex::FieldType(*ctx->file, idx));
    if (ctx->type && ctx->type[0] && !ShapeMatch(ctx->type, type)) return;
    if (ctx->count >= ctx->max) return;
    Mem &mem = ctx->out[ctx->count++];
    mem.cls = ctx->cls;
    CopyTo(mem.name, sizeof(mem.name), dex::String(*ctx->file, dex::FieldName(*ctx->file, idx)));
    CopyTo(mem.desc, sizeof(mem.desc), type);
    mem.flags = flags;
}

struct MethodCtx {
    const dex::File *file;
    int cls;
    const char *name;
    const char *pattern;
    const char *exact;
    int want_static;
    bool require_final;
    Mem *out;
    int count;
    int max;
};

void OnMethodQueried(void *p, uint32_t idx, uint32_t flags, uint32_t code) {
    (void)code;
    MethodCtx *ctx = static_cast<MethodCtx *>(p);
    if (ctx->want_static >= 0 && ((flags & 0x8) ? 1 : 0) != ctx->want_static) return;
    if (ctx->require_final && !(flags & 0x10)) return;
    const char *name = dex::String(*ctx->file, dex::MethodName(*ctx->file, idx));
    if (ctx->name && strcmp(ctx->name, name)) return;
    char proto[256];
    if (!dex::ProtoString(*ctx->file, dex::MethodProto(*ctx->file, idx), proto, sizeof(proto))) return;
    if (ctx->pattern && ctx->pattern[0] && !ShapeMatch(ctx->pattern, proto)) return;
    if (ctx->exact && strcmp(ctx->exact, proto)) return;
    if (ctx->count >= ctx->max) return;
    Mem &mem = ctx->out[ctx->count++];
    mem.cls = ctx->cls;
    CopyTo(mem.name, sizeof(mem.name), name);
    CopyTo(mem.desc, sizeof(mem.desc), proto);
    mem.flags = flags;
}

bool Engine::LoadClass(int cls, const dex::File **file, dex::ClassDef *def) {
    if (cls < 0 || cls >= class_count_) return false;
    const ClassEntry &entry = classes_[cls];
    const dex::File *loaded = apk_.Load(entry.dex);
    if (!loaded || !dex::ReadClassDef(*loaded, entry.def, def, nullptr)) return false;
    *file = loaded;
    return true;
}

int Engine::MethodsOf(int cls, const char *name, const char *pattern, int want_static, Mem *out, int max) {
    const dex::File *file = nullptr;
    dex::ClassDef def;
    if (!LoadClass(cls, &file, &def)) return 0;
    MethodCtx ctx{file, cls, name, pattern, nullptr, want_static, false, out, 0, max};
    dex::Methods(*file, def, &ctx, OnMethodQueried);
    return ctx.count;
}

int Engine::FieldsOf(int cls, const char *type, int want_static, Mem *out, int max) {
    const dex::File *file = nullptr;
    dex::ClassDef def;
    if (!LoadClass(cls, &file, &def)) return 0;
    FieldCtx ctx{file, cls, type, want_static, out, 0, max};
    dex::Fields(*file, def, &ctx, OnFieldQueried);
    return ctx.count;
}

int Engine::ByProto(const char *proto, int want_static, bool require_final, Mem *out, int max) {
    int count = 0;
    for (int d = 0; d < apk_.Count() && count < max; ++d) {
        const dex::File *file = apk_.Load(d);
        if (!file) continue;
        for (uint32_t c = 0; c < file->classes_count && count < max; ++c) {
            dex::ClassDef def;
            if (!dex::ReadClassDef(*file, c, &def, nullptr)) continue;
            const int cls = Find(dex::Type(*file, def.class_idx));
            if (cls < 0) continue;
            MethodCtx ctx{file, cls, nullptr, nullptr, proto, want_static, require_final, out + count, 0, max - count};
            dex::Methods(*file, def, &ctx, OnMethodQueried);
            count += ctx.count;
        }
    }
    return count;
}

int Engine::StaticFieldsOf(const char *type, Mem *out, int max) {
    int count = 0;
    for (int d = 0; d < apk_.Count() && count < max; ++d) {
        const dex::File *file = apk_.Load(d);
        if (!file) continue;
        for (uint32_t c = 0; c < file->classes_count && count < max; ++c) {
            dex::ClassDef def;
            if (!dex::ReadClassDef(*file, c, &def, nullptr)) continue;
            const char *desc = dex::Type(*file, def.class_idx);
            const int cls = Find(desc);
            if (cls < 0) continue;
            FieldCtx ctx{file, cls, type, 1, out + count, 0, max - count};
            dex::Fields(*file, def, &ctx, OnFieldQueried);
            count += ctx.count;
        }
    }
    return count;
}

int Engine::Supers(int cls, int *out, int max) const {
    int queue[kMaxSupers];
    int queued = 0;
    int count = 0;
    if (cls >= 0 && cls < class_count_ && max > 0) queue[queued++] = cls;
    while (queued && count < max) {
        const int c = queue[--queued];
        bool seen = false;
        for (int i = 0; i < count; ++i)
            if (out[i] == c) {
                seen = true;
                break;
            }
        if (seen) continue;
        out[count++] = c;
        const ClassEntry &entry = classes_[c];
        const int super = entry.super ? Find(entry.super) : -1;
        if (super >= 0 && queued < kMaxSupers) queue[queued++] = super;
        for (int k = 0; k < entry.iface_count && queued < kMaxSupers; ++k) {
            const int iface = Find(entry.ifaces[k]);
            if (iface >= 0) queue[queued++] = iface;
        }
    }
    return count;
}

bool Engine::IsA(int cls, const char *target) const {
    if (cls < 0 || !target || !*target) return false;
    int list[kMaxSupers];
    const int count = Supers(cls, list, kMaxSupers);
    for (int i = 0; i < count; ++i)
        if (!strcmp(classes_[list[i]].desc, target)) return true;
    return false;
}

bool Engine::Ctor(int cls, const char *pattern, char *desc, size_t size) {
    Mem list[kMaxCandidates];
    const int count = MethodsOf(cls, "<init>", pattern, -1, list, kMaxCandidates);
    int found = 0;
    for (int i = 0; i < count; ++i) {
        char params[16][160];
        const int n = Params(list[i].desc, params, 16);
        if (n && StartsWith(params[n - 1], "Lkotlin/jvm/internal/")) continue;
        if (desc) CopyTo(desc, size, list[i].desc);
        ++found;
        if (found > 1) break;
    }
    return found == 1;
}

int Engine::ServiceOf(int impl, char *desc, size_t size) {
    const char *root = Ret(out.value[kLocatorGetDesc]);
    if (!*root) return 0;
    if (impl < 0 || impl >= class_count_) return 0;
    const ClassEntry &entry = classes_[impl];
    int found = 0;
    for (int k = 0; k < entry.iface_count; ++k) {
        const char *iface = entry.ifaces[k];
        if (strcmp(iface, root) && IsA(Find(iface), root)) {
            if (desc) CopyTo(desc, size, iface);
            ++found;
        }
    }
    return found == 1 ? 1 : 0;
}

// -- results --------------------------------------------------------------------------------------

void Engine::Set(WxKey key, const char *value) { CopyTo(out.value[key], kWxValueSize, value); }

void Engine::Fail(WxKey key, const char *why) { CopyTo(reason[key], sizeof(reason[key]), why); }

void Engine::SetClass(WxKey key, const char *desc) {
    if (!desc || !*desc) return;
    char dotted[160];
    Dotted(desc, dotted, sizeof(dotted));
    Set(key, dotted);
}

const char *Engine::Pick(WxKey key, const char *const *names, int count, int fallback) {
    if (count <= 0) return nullptr;
    if (count == 1) return names[0];
    const char *hint = hints_ ? hints_->value[key] : nullptr;
    for (int i = 0; i < count; ++i)
        if (hint && *hint && !strcmp(names[i], hint)) return names[i];
    if (fallback == 1) return names[count - 1];
    if (fallback == 0) return names[0];
    return nullptr;
}

bool Engine::Member(WxKey key, WxKey desc_key, int cls, char kind, const char *pattern, int want_static, int fallback) {
    if (cls < 0) {
        Fail(key, "class not found");
        return false;
    }
    Mem *list = scratch_;
    const int count = kind == 'm' ? MethodsOf(cls, nullptr, pattern, want_static, list, kMaxScratch)
                                  : FieldsOf(cls, pattern, want_static, list, kMaxScratch);
    const char *names[kMaxScratch];
    int name_count = 0;
    for (int i = 0; i < count; ++i) {
        bool seen = false;
        for (int k = 0; k < name_count; ++k)
            if (!strcmp(names[k], list[i].name)) {
                seen = true;
                break;
            }
        if (!seen) names[name_count++] = list[i].name;
    }
    TRACE("member %s cls=%s candidates=%d hint=%s\n", WxKeyId(key),
          cls >= 0 && cls < class_count_ ? classes_[cls].desc : "-", name_count, hints_ ? hints_->value[key] : "-");
    const char *name = Pick(key, names, name_count, fallback);
    if (!name) {
        Fail(key, name_count ? "ambiguous member" : "no member");
        return false;
    }
    const char *desc = nullptr;
    for (int i = 0; i < count; ++i) {
        if (strcmp(list[i].name, name)) continue;
        if (desc && strcmp(desc, list[i].desc)) {
            Fail(key, "overloaded member");
            return false;
        }
        desc = list[i].desc;
    }
    Set(key, name);
    if (desc_key >= 0 && desc) Set(desc_key, desc);
    return true;
}

// -- anchors --------------------------------------------------------------------------------------

int Engine::AnchorCount(int anchor) const {
    int count = 0;
    for (int i = 0; i < hit_count_; ++i)
        if (hits_[i].anchor == anchor) ++count;
    return count;
}

const Hit &Engine::AnchorAt(int anchor, int index) const {
    int seen = 0;
    for (int i = 0; i < hit_count_; ++i) {
        if (hits_[i].anchor != anchor) continue;
        if (seen++ == index) return hits_[i];
    }
    return hits_[0];
}

int Engine::AnchorClasses(int anchor, int *out, int max) const {
    int count = 0;
    for (int i = 0; i < hit_count_; ++i) {
        if (hits_[i].anchor != anchor) continue;
        const int cls = Find(hits_[i].cls);
        if (cls < 0) continue;
        bool seen = false;
        for (int k = 0; k < count; ++k)
            if (out[k] == cls) {
                seen = true;
                break;
            }
        if (!seen && count < max) out[count++] = cls;
    }
    return count;
}

const char *Engine::AnchorClass(int anchor) const {
    for (int i = 0; i < hit_count_; ++i)
        if (hits_[i].anchor == anchor) return hits_[i].cls;
    return nullptr;
}

// -- rules ----------------------------------------------------------------------------------------

void Engine::RuleSend() {
    // doScene(dispatcher, callback) is declared on the scene base class; the cgi base and the text scene
    // both lead there.
    static const char kDoScene[] = "(L*;L*;)I";
    const char *protos[8];
    int proto_count = 0;
    const int starts[2] = {kCgi, kSendMsg};
    for (int a = 0; a < 2; ++a) {
        int classes[64];
        const int count = AnchorClasses(starts[a], classes, 64);
        for (int i = 0; i < count; ++i) {
            int supers[kMaxSupers];
            const int sc = Supers(classes[i], supers, kMaxSupers);
            for (int s = 0; s < sc; ++s) {
                Mem list[kMaxCandidates];
                const int mc = MethodsOf(supers[s], "doScene", kDoScene, -1, list, kMaxCandidates);
                for (int k = 0; k < mc; ++k) {
                    bool seen = false;
                    for (int j = 0; j < proto_count; ++j)
                        if (!strcmp(protos[j], list[k].desc)) {
                            seen = true;
                            break;
                        }
                    if (!seen && proto_count < 8) protos[proto_count++] = Copy(list[k].desc);
                }
            }
        }
    }
    if (proto_count != 1) {
        Fail(kDoSceneDesc, "doScene not unique");
        return;
    }
    Set(kDoSceneDesc, protos[0]);
    char params[8][160];
    if (Params(protos[0], params, 8) >= 2) {
        SetClass(kSceneDispatcherCls, params[0]);
        SetClass(kSceneCallbackCls, params[1]);
    }
    // The text scene: the class that logs as NetSceneSendMsg and takes (talker, text, type, flag, extra,
    // source).
    int scenes[64];
    const int scene_count = AnchorClasses(kSendMsg, scenes, 64);
    int scene = -1;
    for (int i = 0; i < scene_count; ++i)
        if (Ctor(scenes[i], "(Ljava/lang/String;Ljava/lang/String;IIJLjava/lang/String;)V", nullptr, 0))
            scene = scenes[i];
    if (scene < 0) {
        Fail(kSendSceneCls, "no text constructor");
        return;
    }
    SetClass(kSendSceneCls, classes_[scene].desc);
    Member(kSendSceneLocalIdName, static_cast<WxKey>(-1), scene, 'f', "J", 0, -1);
    // The callback object handed to doScene: the class that logs "Run successfully" and implements the
    // callback.
    char callback[160];
    Descriptor(out.value[kSceneCallbackCls], callback, sizeof(callback));
    int callbacks[64];
    const int callback_count = AnchorClasses(kRunOk, callbacks, 64);
    for (int i = 0; i < callback_count; ++i)
        if (IsA(callbacks[i], callback)) {
            SetClass(kNoopCallbackCls, classes_[callbacks[i]].desc);
            break;
        }
    // The push core, whose static accessor returns the remote dispatcher.
    char dispatcher[160];
    Descriptor(out.value[kSceneDispatcherCls], dispatcher, sizeof(dispatcher));
    int cores[16];
    const int core_count = AnchorClasses(kAutoAuth, cores, 16);
    for (int i = 0; i < core_count; ++i) {
        SetClass(kPushCoreCls, classes_[cores[i]].desc);
        Mem list[kMaxCandidates];
        const int mc = MethodsOf(cores[i], nullptr, "()L*;", 1, list, kMaxCandidates);
        int found = 0;
        Mem *pick = nullptr;
        for (int k = 0; k < mc; ++k)
            if (IsA(Find(Ret(list[k].desc)), dispatcher)) {
                ++found;
                pick = &list[k];
            }
        if (found == 1) {
            Set(kPushCoreDispatcherName, pick->name);
            Set(kPushCoreDispatcherDesc, pick->desc);
            break;
        }
    }
    // The net queue: holds the dispatcher and a static instance of itself.
    int queues[16];
    const int queue_count = AnchorClasses(kNetQueue, queues, 16);
    char getter[192];
    snprintf(getter, sizeof(getter), "()%s", dispatcher);
    for (int i = 0; i < queue_count; ++i) {
        Mem fields[kMaxCandidates];
        Mem methods[kMaxCandidates];
        const int fc = FieldsOf(queues[i], classes_[queues[i]].desc, 1, fields, kMaxCandidates);
        const int mc = MethodsOf(queues[i], nullptr, getter, 0, methods, kMaxCandidates);
        if (fc == 1 && mc == 1) {
            SetClass(kNetQueueCls, classes_[queues[i]].desc);
            Set(kNetQueueSingletonName, fields[0].name);
            Set(kNetQueueSingletonDesc, fields[0].desc);
            Set(kNetQueueDispatcherName, methods[0].name);
            Set(kNetQueueDispatcherDesc, methods[0].desc);
            break;
        }
    }
}

void Engine::RuleRecall() {
    // The revoke scene: the cgi path plus a constructor that takes (message, hint, ?).
    int scenes[16];
    const int count = AnchorClasses(kRevoke, scenes, 16);
    int revoke = -1;
    char ctor[256] = {};
    for (int i = 0; i < count; ++i)
        if (Ctor(scenes[i], "(L*;Ljava/lang/String;Ljava/lang/String;)V", ctor, sizeof(ctor))) {
            revoke = scenes[i];
            break;
        }
    if (revoke < 0) {
        Fail(kRevokeCls, "no revoke constructor");
        return;
    }
    SetClass(kRevokeCls, classes_[revoke].desc);
    Set(kRevokeCtorDesc, ctor);
    char params[8][160];
    if (Params(ctor, params, 8) < 1) return;
    SetClass(kMsgInfoCls, params[0]);
    // Whether this account sent the message is a field of the message base class (im.c8, a library class
    // that keeps its names), not a method.
    Set(kMsgInfoIsSendField, "field_isSend");
    const char *msg = params[0];
    char getter[288];
    snprintf(getter, sizeof(getter), "(Ljava/lang/String;J)%s", msg);
    // The message store: several final (talker, id) -> message getters, held by another class.
    Mem methods[256];
    const int mc = ByProto(getter, 0, true, methods, 256);
    int store = -1;
    int groups[64];
    int counts[64];
    int group_count = 0;
    for (int i = 0; i < mc; ++i) {
        int found = -1;
        for (int g = 0; g < group_count; ++g)
            if (groups[g] == methods[i].cls) {
                found = g;
                break;
            }
        if (found < 0 && group_count < 64) {
            groups[group_count] = methods[i].cls;
            counts[group_count] = 0;
            found = group_count++;
        }
        if (found >= 0) ++counts[found];
    }
    for (int g = 0; g < group_count; ++g) {
        if (counts[g] < 2) continue;
        Mem fields[64];
        const int fc = StaticFieldsOf(classes_[groups[g]].desc, fields, 64);
        bool held = false;
        for (int k = 0; k < fc; ++k)
            if (fields[k].cls != groups[g]) held = true;
        if (!held) continue;
        if (store >= 0) {
            store = -2;
            break;
        }
        store = groups[g];
    }
    if (store < 0) {
        Fail(kStoreCls, store == -2 ? "store candidates" : "no store");
        return;
    }
    SetClass(kStoreCls, classes_[store].desc);
    Mem holders[64];
    const int hc = StaticFieldsOf(classes_[store].desc, holders, 64);
    int holder = 0;
    Mem *held = nullptr;
    for (int k = 0; k < hc; ++k)
        if (holders[k].cls != store) {
            ++holder;
            held = &holders[k];
        }
    if (holder == 1) {
        SetClass(kStoreHolderCls, classes_[held->cls].desc);
        Set(kStoreHolderFieldName, held->name);
        Set(kStoreHolderFieldDesc, held->desc);
    }
    Member(kStoreGetName, kStoreGetDesc, store, 'm', getter, 0, -1);
}

void Engine::RuleLocator() {
    // The service registry: logs as ServiceManager and has the static lookup (Class) -> service.
    int classes[64];
    const int count = AnchorClasses(kServiceManager, classes, 64);
    int best = -1;
    int best_hits = -1;
    for (int i = 0; i < count; ++i) {
        Mem list[kMaxCandidates];
        const int mc = MethodsOf(classes[i], nullptr, "(Ljava/lang/Class;)L*;", 1, list, kMaxCandidates);
        int found = 0;
        for (int k = 0; k < mc; ++k)
            if (strcmp(Ret(list[k].desc), "Z")) ++found;
        if (found != 1) continue;
        int hits = 0;
        for (int h = 0; h < hit_count_; ++h)
            if (hits_[h].anchor == kServiceManager && !strcmp(hits_[h].cls, classes_[classes[i]].desc)) ++hits;
        if (hits > best_hits) {
            best = classes[i];
            best_hits = hits;
        }
    }
    if (best >= 0) {
        SetClass(kLocatorCls, classes_[best].desc);
        Member(kLocatorGetName, kLocatorGetDesc, best, 'm', "(Ljava/lang/Class;)L*;", 1, -1);
    }
}

void Engine::RulePrepare() {
    // The chat UI one-time preparation: a static ()V on the class that logs as BizHalfScreenChattingUtil.
    int classes[16];
    const int count = AnchorClasses(kBizHalf, classes, 16);
    for (int i = 0; i < count; ++i) {
        Mem list[kMaxCandidates];
        const int mc = MethodsOf(classes[i], nullptr, "()V", 1, list, kMaxCandidates);
        bool real = false;
        bool clinit = false;
        for (int k = 0; k < mc; ++k)
            if (!strcmp(list[k].name, "<clinit>"))
                clinit = true;
            else
                real = true;
        if (real && clinit) {
            SetClass(kPrepareCls, classes_[classes[i]].desc);
            Member(kPrepareName, static_cast<WxKey>(-1), classes[i], 'm', "()V", 1, -1);
            break;
        }
    }
}

namespace {

// The image service method the rules want: one class parameter, a flow of ... return value.
bool FlowMethod(const char *proto) {
    char params[8][160];
    if (Params(proto, params, 8) != 1 || params[0][0] != 'L') return false;
    const char *ret = Ret(proto);
    return StartsWith(ret, "Lkotlinx/coroutines/flow/") && ret[strlen(ret) - 1] == ';';
}

} // namespace

void Engine::RuleImage() {
    int impls[16];
    int impl_count = 0;
    int classes[64];
    const int count = AnchorClasses(kImgService, classes, 64);
    for (int i = 0; i < count; ++i) {
        Mem list[kMaxCandidates];
        const int mc = MethodsOf(classes[i], nullptr, nullptr, 0, list, kMaxCandidates);
        int flow = 0;
        for (int k = 0; k < mc; ++k)
            if (FlowMethod(list[k].desc)) ++flow;
        TRACE("image %s methods=%d flow=%d\n", classes_[classes[i]].desc, mc, flow);
        if (flow == 1) impls[impl_count++] = classes[i];
    }
    if (impl_count != 1) {
        Fail(kImageServiceCls, "image service candidates");
        return;
    }
    const int impl = impls[0];
    char service[160];
    if (ServiceOf(impl, service, sizeof(service))) SetClass(kImageServiceCls, service);
    Mem list[kMaxCandidates];
    const int mc = MethodsOf(impl, nullptr, nullptr, 0, list, kMaxCandidates);
    const Mem *send = nullptr;
    for (int k = 0; k < mc; ++k)
        if (FlowMethod(list[k].desc)) send = &list[k];
    if (!send) return;
    Set(kImageSendName, send->name);
    Set(kImageSendDesc, send->desc);
    char params[16][160];
    if (Params(send->desc, params, 16) < 1) return;
    SetClass(kImageParamsCls, params[0]);
    const char *params_cls = params[0];
    char ctor[256];
    if (Ctor(Find(params_cls), "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;L*;)V", ctor, sizeof(ctor))) {
        Set(kImageParamsCtorDesc, ctor);
        char cp[16][160];
        if (Params(ctor, cp, 16) >= 5) SetClass(kImageContextCls, cp[4]);
    }
    // The field the constructor fills with "msg_raw_img_send" is the source tag.
    const char *source = nullptr;
    int source_count = 0;
    for (int i = 0; i < hit_count_; ++i) {
        const Hit &hit = hits_[i];
        if (hit.anchor != kRawImg || !hit.field || strcmp(hit.cls, params_cls)) continue;
        if (source && !strcmp(source, hit.field)) continue;
        source = hit.field;
        ++source_count;
    }
    if (source_count == 1)
        Set(kImageParamsSourceName, source);
    else
        Member(kImageParamsSourceName, static_cast<WxKey>(-1), Find(params_cls), 'f', "Ljava/lang/String;", 0, 1);
    SetClass(kImageCallbackCls, AnchorClass(kForwardInfo));
    char context_desc[160];
    Descriptor(out.value[kImageContextCls], context_desc, sizeof(context_desc));
    const int context = Find(context_desc);
    if (context >= 0) {
        Member(kImageContextKindName, static_cast<WxKey>(-1), context, 'f', "I", 0, 0);
        char callback[160];
        Descriptor(out.value[kImageCallbackCls], callback, sizeof(callback));
        Member(kImageContextCallbackName, kImageContextCallbackDesc, context, 'f', callback, 0, -1);
    }
}

void Engine::RuleAppMsg() {
    static const char kSend[] =
        "(L*;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)Landroid/util/Pair;";
    int classes[16];
    const int count = AnchorClasses(kAppMsg, classes, 16);
    int logic = -1;
    for (int i = 0; i < count; ++i) {
        Mem list[kMaxCandidates];
        if (MethodsOf(classes[i], nullptr, kSend, 1, list, kMaxCandidates) == 1) logic = classes[i];
    }
    if (logic < 0) {
        Fail(kAppMsgCls, "app message candidates");
        return;
    }
    SetClass(kAppMsgCls, classes_[logic].desc);
    Member(kAppMsgAttachDirName, static_cast<WxKey>(-1), logic, 'm', "()Ljava/lang/String;", 1, -1);
    Member(kAppMsgFreePathName, static_cast<WxKey>(-1), logic, 'm',
           "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", 1, -1);
    if (!Member(kAppMsgSendName, kAppMsgSendDesc, logic, 'm', kSend, 1, -1)) return;
    char params[16][160];
    if (Params(out.value[kAppMsgSendDesc], params, 16) < 1) return;
    SetClass(kContentCls, params[0]);
    const int content = Find(params[0]);
    char parse[224];
    snprintf(parse, sizeof(parse), "(Ljava/lang/String;)%s", params[0]);
    Member(kContentParseName, kContentParseDesc, content, 'm', parse, 1, -1);
    Member(kContentTitleName, static_cast<WxKey>(-1), content, 'f', "Ljava/lang/String;", 0, -1);
    Member(kContentKindName, static_cast<WxKey>(-1), content, 'f', "I", 0, -1);
    char quote[160][160];
    int quote_count = 0;
    const char *hint = hints_ ? hints_->value[kQuoteItemCls] : nullptr;
    if (hint && *hint) {
        char hint_desc[160];
        Descriptor(hint, hint_desc, sizeof(hint_desc));
        Mem fields[kMaxCandidates];
        const int fc = FieldsOf(content, hint_desc, 0, fields, kMaxCandidates);
        if (fc == 1) CopyTo(quote[quote_count++], sizeof(quote[0]), hint_desc);
    }
    if (!quote_count) {
        Mem fields[kMaxCandidates];
        const int fc = FieldsOf(content, nullptr, 0, fields, kMaxCandidates);
        for (int k = 0; k < fc && quote_count < 160; ++k) {
            const size_t n = strlen(fields[k].desc);
            if (n > 14 && !strcmp(fields[k].desc + n - 14, "/MsgQuoteItem;"))
                CopyTo(quote[quote_count++], sizeof(quote[0]), fields[k].desc);
        }
    }
    if (quote_count != 1) return;
    SetClass(kQuoteItemCls, quote[0]);
    Member(kContentQuoteName, static_cast<WxKey>(-1), content, 'f', quote[0], 0, -1);
    static const struct {
        WxKey key;
        const char *type;
    } kFields[] = {{kQuoteItemTypeName, "I"},
                   {kQuoteItemSvrName, "J"},
                   {kQuoteItemFromName, "Ljava/lang/String;"},
                   {kQuoteItemChatName, "Ljava/lang/String;"},
                   {kQuoteItemDisplayName, "Ljava/lang/String;"},
                   {kQuoteItemSourceName, "Ljava/lang/String;"},
                   {kQuoteItemContentName, "Ljava/lang/String;"},
                   {kQuoteItemMergedName, "Ljava/lang/String;"},
                   {kQuoteItemStridName, "Ljava/lang/String;"},
                   {kQuoteItemCreatedName, "J"}};
    for (const auto &field : kFields) Member(field.key, static_cast<WxKey>(-1), Find(quote[0]), 'f', field.type, 0, -1);
}

void Engine::RuleVideo() {
    static const char kShape[] = "(L*;Ljava/lang/String;)Z";
    int classes[16];
    const int count = AnchorClasses(kVideoService, classes, 16);
    int impl = -1;
    char service[160] = {};
    for (int i = 0; i < count; ++i) {
        Mem list[kMaxCandidates];
        if (MethodsOf(classes[i], nullptr, kShape, 0, list, kMaxCandidates) == 1) {
            char found[160];
            if (ServiceOf(classes[i], found, sizeof(found))) {
                impl = classes[i];
                CopyTo(service, sizeof(service), found);
            }
        }
    }
    if (impl < 0) {
        Fail(kVideoServiceCls, "video service candidates");
        return;
    }
    SetClass(kVideoServiceCls, service);
    if (!Member(kVideoSendName, kVideoSendDesc, impl, 'm', kShape, 0, -1)) return;
    char params[16][160];
    if (Params(out.value[kVideoSendDesc], params, 16) < 1) return;
    SetClass(kVideoElementCls, params[0]);
    char ctor[256];
    if (!Ctor(Find(params[0]), "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ZIL*;L*;)V", ctor, sizeof(ctor)))
        return;
    Set(kVideoElementCtorDesc, ctor);
    char cp[16][160];
    if (Params(ctor, cp, 16) < 6) return;
    SetClass(kVideoCrossCls, cp[5]);
    char cross[256];
    if (Ctor(Find(cp[5]), nullptr, cross, sizeof(cross))) Set(kVideoCrossCtorDesc, cross);
}

void Engine::RuleVoice() {
    int subs[16];
    int services[16];
    const int sub_count = AnchorClasses(kSubCoreVoice, subs, 16);
    const int service_count = AnchorClasses(kVoiceService, services, 16);
    int sub = -1;
    int both = 0;
    for (int i = 0; i < sub_count; ++i)
        for (int k = 0; k < service_count; ++k)
            if (subs[i] == services[k]) {
                sub = subs[i];
                ++both;
            }
    if (both == 1) {
        SetClass(kVoiceSubCoreCls, classes_[sub].desc);
        Mem list[kMaxCandidates];
        const int mc = MethodsOf(sub, nullptr, "()L*;", 1, list, kMaxCandidates);
        int found = 0;
        const Mem *pick = nullptr;
        for (int k = 0; k < mc; ++k) {
            const int uploader = Find(Ret(list[k].desc));
            if (uploader < 0 || uploader == sub) continue;
            bool is_uploader = false;
            for (int s = 0; s < service_count; ++s)
                if (services[s] == uploader) is_uploader = true;
            if (!is_uploader) continue;
            ++found;
            pick = &list[k];
        }
        if (found == 1) {
            Set(kVoiceUploaderOfName, pick->name);
            Set(kVoiceUploaderOfDesc, pick->desc);
            SetClass(kVoiceUploaderCls, Ret(pick->desc));
            Member(kVoiceUploaderRunName, static_cast<WxKey>(-1), Find(Ret(pick->desc)), 'm', "()V", 0, -1);
        }
    }
    // Finishing a voice message: the static (file, length, kind, message, ?) -> boolean.
    if (out.value[kMsgInfoCls][0]) {
        char msg[160];
        Descriptor(out.value[kMsgInfoCls], msg, sizeof(msg));
        char finish[320];
        snprintf(finish, sizeof(finish), "(Ljava/lang/String;II%sLjava/lang/String;)Z", msg);
        Mem list[8];
        const int mc = ByProto(finish, 1, false, list, 8);
        if (mc == 1) {
            SetClass(kVoiceLogicCls, classes_[list[0].cls].desc);
            Set(kVoiceFinishName, list[0].name);
            Set(kVoiceFinishDesc, list[0].desc);
            Member(kVoiceStartName, static_cast<WxKey>(-1), list[0].cls, 'm',
                   "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", 1, -1);
        }
    }
    // The path service: the kind class is the one nearly every name builder takes first, but several
    // methods have that shape (message, app message, kind), so the kind class has to say which one it is:
    // it declares a static instance of its own type, next to its variant enum.
    int impls[16];
    const int impl_count = AnchorClasses(kC2CFile, impls, 16);
    char paths[160];
    if (impl_count == 1 && ServiceOf(impls[0], paths, sizeof(paths))) {
        SetClass(kVoicePathsCls, paths);
        Mem list[kMaxCandidates];
        const int mc =
            MethodsOf(impls[0], nullptr, "(L*;Ljava/lang/String;ZZ)Ljava/lang/String;", 0, list, kMaxCandidates);
        int picks = 0;
        const Mem *pick = nullptr;
        char kind[160] = {};
        for (int k = 0; k < mc; ++k) {
            char params[8][160];
            if (Params(list[k].desc, params, 8) < 1) continue;
            Mem own[8];
            if (FieldsOf(Find(params[0]), params[0], 1, own, 8) > 0) {
                ++picks;
                pick = &list[k];
                CopyTo(kind, sizeof(kind), params[0]);
            }
        }
        if (picks == 1) {
            Set(kVoicePathName, pick->name);
            Set(kVoicePathDesc, pick->desc);
            SetClass(kVoiceKindCls, kind);
            Member(kVoiceKindFieldName, kVoiceKindFieldDesc, Find(kind), 'f', kind, 1, -1);
        }
    }
}

void Engine::RuleRoom() {
    static const struct {
        WxKey key;
        int anchor;
    } kScenes[] = {{kRoomKickCls, kRoomKick}, {kRoomAdminAddCls, kRoomAdminAdd}, {kRoomAdminDelCls, kRoomAdminDel}};
    int scene = -1;
    for (const auto &item : kScenes) {
        int classes[8];
        if (AnchorClasses(item.anchor, classes, 8) == 1) {
            SetClass(item.key, classes_[classes[0]].desc);
            if (item.anchor == kRoomAdminAdd) scene = classes[0];
        }
    }
    // The admin scenes are cgi wrappers: their base class holds the request object (kick is a plain scene).
    int base = -1;
    if (scene >= 0) {
        int cgi[64];
        const int cgi_count = AnchorClasses(kCgi, cgi, 64);
        int supers[kMaxSupers];
        const int sc = Supers(scene, supers, kMaxSupers);
        for (int s = 1; s < sc && base < 0; ++s) {
            bool in_cgi = false;
            for (int k = 0; k < cgi_count; ++k)
                if (cgi[k] == supers[s]) in_cgi = true;
            if (!in_cgi) continue;
            Mem fields[kMaxCandidates];
            if (FieldsOf(supers[s], nullptr, 0, fields, kMaxCandidates) > 0) {
                base = supers[s];
                SetClass(kRoomCgiBaseCls, classes_[base].desc);
            }
        }
    }
    if (base >= 0) Member(kRoomCgiReqName, kRoomCgiReqDesc, base, 'f', "L*;", 0, -1);
    int runners[16];
    const int runner_count = AnchorClasses(kRunCgi, runners, 16);
    for (int i = 0; i < runner_count; ++i) {
        Mem list[8];
        if (MethodsOf(runners[i], nullptr, "(L*;L*;Z)L*;", 1, list, 8) == 1) {
            SetClass(kRoomRunnerCls, classes_[runners[i]].desc);
            Set(kRoomRunnerName, list[0].name);
            Set(kRoomRunnerDesc, list[0].desc);
            break;
        }
    }
}

void Engine::RulePat() {
    int scenes[8];
    const int scene_count = AnchorClasses(kSendPat, scenes, 8);
    for (int i = 0; i < scene_count; ++i)
        if (Ctor(scenes[i], "(Landroid/util/Pair;Ljava/lang/String;Ljava/lang/String;I)V", nullptr, 0)) {
            SetClass(kPatSceneCls, classes_[scenes[i]].desc);
            break;
        }
    static const char kShape[] =
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IJ)Landroid/util/Pair;";
    int managers[8];
    const int manager_count = AnchorClasses(kPatExtension, managers, 8);
    int manager = -1;
    for (int i = 0; i < manager_count; ++i) {
        Mem list[8];
        if (MethodsOf(managers[i], nullptr, kShape, 0, list, 8) == 1) manager = managers[i];
    }
    if (manager >= 0) {
        SetClass(kPatManagerCls, classes_[manager].desc);
        char service[160];
        if (ServiceOf(manager, service, sizeof(service))) SetClass(kPatServiceCls, service);
        Member(kPatInsertName, static_cast<WxKey>(-1), manager, 'm', kShape, 0, -1);
    }
}

void Engine::Run(const WxNames &hints) {
    hints_ = &hints;
    RuleSend();
    RuleRecall();
    RuleLocator();
    RulePrepare();
    RuleImage();
    RuleAppMsg();
    RuleVideo();
    RuleVoice();
    RuleRoom();
    RulePat();
    // Keys no rule derives: a library field, and a behaviour switch the next build inherits.
    if (!out.value[kMsgInfoIsSendField][0]) Set(kMsgInfoIsSendField, "field_isSend");
    if (!out.value[kThreadLooper][0]) Set(kThreadLooper, hints.value[kThreadLooper]);
}

} // namespace
bool DerivedNames::Complete() const {
    for (int i = 0; i < kWxKeyCount; ++i)
        if (!value[i][0]) return false;
    return true;
}

bool DeriveNames(const char *apk_path, const char *hints_label, DerivedNames *out, char *error, size_t size) {
    if (!out) return false;
    const WxNames *hints = hints_label ? WxNamesByLabel(hints_label) : nullptr;
    if (!hints) hints = &WxNamesNewestBuiltin();
    Engine *engine = static_cast<Engine *>(calloc(1, sizeof(Engine)));
    if (!engine) {
        if (error && size) snprintf(error, size, "out of memory");
        return false;
    }
    bool ok = engine->Build(apk_path, error, size);
    if (ok) {
        engine->Run(*hints);
        memcpy(out->value, engine->out.value, sizeof(out->value));
        memcpy(out->reason, engine->reason, sizeof(out->reason));
    }
    free(engine);
    return ok;
}

} // namespace satori
