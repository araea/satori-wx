#pragma once
#include <stddef.h>
#include "protocol.h"

namespace satori {
// Satori Backend backed by the read-only WeChat store.
const Backend *WeChatBackend();
// RPC methods the WeChat backend implements; also used for the login `features` list.
const char *const *WeChatFeatures(size_t *count);
} // namespace satori
