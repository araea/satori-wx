#pragma once
#include "protocol.h"

namespace satori {
// Satori Backend backed by the read-only WeChat store plus the opt-in sender.
const Backend *WeChatBackend();
} // namespace satori
