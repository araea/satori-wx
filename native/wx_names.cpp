#include "wx_names.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

namespace satori {
namespace {
const WxNames k79 = {"8.0.79",
                     {
#define X(key, v78, v79) v79,
                         WX_NAME_LIST(X)
#undef X
                     }};
const WxNames k78 = {"8.0.78",
                     {
#define X(key, v78, v79) v78,
                         WX_NAME_LIST(X)
#undef X
                     }};
const char *const kIds[kWxKeyCount] = {
#define X(key, v78, v79) #key,
    WX_NAME_LIST(X)
#undef X
};

// The discovered set is built once, published by a single pointer store and never modified again, so a
// sender that already holds a reference to an older one is not disturbed. A replaced set is leaked: it is
// a few kilobytes and replaced at most a few times per process.
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
const WxNames *volatile g_found = nullptr;
} // namespace

int WxNamesCount() { return g_found ? 3 : 2; }

const WxNames &WxNamesAt(int index) {
    const WxNames *found = g_found;
    if (found) {
        if (index == 0) return *found;
        --index;
    }
    return index == 0 ? k79 : k78;
}

const WxNames &WxNamesNewestBuiltin() { return k79; }

const WxNames *WxNamesByLabel(const char *label) {
    if (!label) return nullptr;
    for (int i = 0; i < WxNamesCount(); ++i) {
        const WxNames &set = WxNamesAt(i);
        if (set.label && !strcmp(set.label, label)) return &set;
    }
    return nullptr;
}

bool WxNamesInstall(const char *label, const char *const *values) {
    if (!values) return false;
    for (int i = 0; i < kWxKeyCount; ++i)
        if (!values[i] || !*values[i]) return false;
    WxNames *set = static_cast<WxNames *>(calloc(1, sizeof(WxNames)));
    if (!set) return false;
    set->label = strdup(label && *label ? label : "discovered");
    for (int i = 0; i < kWxKeyCount; ++i) set->value[i] = strdup(values[i]);
    pthread_mutex_lock(&g_mu);
    g_found = set;
    pthread_mutex_unlock(&g_mu);
    return true;
}

const char *WxKeyId(WxKey key) { return key >= 0 && key < kWxKeyCount ? kIds[key] : ""; }

int WxKeyByName(const char *id) {
    if (!id) return -1;
    for (int i = 0; i < kWxKeyCount; ++i)
        if (!strcmp(kIds[i], id)) return i;
    return -1;
}

const char *WxSlash(const char *dotted, char *out, size_t size) {
    if (!size) return out;
    size_t n = 0;
    for (; dotted && dotted[n] && n + 1 < size; ++n) out[n] = dotted[n] == '.' ? '/' : dotted[n];
    out[n] = 0;
    return out;
}

const char *WxClassDesc(const char *dotted, char *out, size_t size) {
    if (size < 4) {
        if (size) out[0] = 0;
        return out;
    }
    out[0] = 'L';
    WxSlash(dotted, out + 1, size - 2);
    const size_t n = strlen(out);
    out[n] = ';';
    out[n + 1] = 0;
    return out;
}
} // namespace satori
