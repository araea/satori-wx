#pragma once
#include "server.h"

namespace satori {
// The `/v1/proxy` resolver for received message media: `_msg/<kind>/<id>/<signature>` under
// `internal:wechat/<user>/`. Registered with SetMediaResolver by the module.
bool WeChatMediaResolver(const char *user, const char *path, MediaFile *out);
} // namespace satori
