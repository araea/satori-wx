#include "wx_media.h"
#include "media.h"
#include "wx_live.h"
#include "wx_store.h"
#include <stdlib.h>
#include <string.h>

namespace satori {
bool WeChatMediaResolver(const char *user, const char *path, MediaFile *out) {
    // _msg/<kind>/<id>/<signature>
    if (!user || !path || strncmp(path, "_msg/", 5)) return false;
    const char *kind = path + 5;
    const char *slash = strchr(kind, '/');
    if (!slash) return false;
    const char *id = slash + 1;
    const char *slash2 = strchr(id, '/');
    if (!slash2) return false;
    const char *signature = slash2 + 1;
    char kind_buf[24], id_buf[24];
    const size_t kind_size = static_cast<size_t>(slash - kind), id_size = static_cast<size_t>(slash2 - id);
    if (!kind_size || kind_size >= sizeof(kind_buf) || !id_size || id_size >= sizeof(id_buf)) return false;
    memcpy(kind_buf, kind, kind_size); kind_buf[kind_size] = 0;
    memcpy(id_buf, id, id_size); id_buf[id_size] = 0;
    for (const char *p = id_buf; *p; ++p) if (*p < '0' || *p > '9') return false;
    // Verify first: everything after this touches the database and the filesystem.
    if (!MediaVerify(user, kind_buf, id_buf, signature)) return false;
    Store *store = LiveStore();
    if (!store) return false;
    // The link is bound to a login; refuse one that names somebody other than this account.
    const char *self = StoreSelfId(store);
    if (!self || !*self || strcmp(self, user)) return false;
    return StoreMediaFile(store, kind_buf, atoll(id_buf), out->path, sizeof(out->path), out->content_type, sizeof(out->content_type));
}
} // namespace satori
