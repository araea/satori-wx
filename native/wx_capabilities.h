#pragma once
#include <stddef.h>

// Canonical feature list shared by the account snapshot (wx_account.cpp) and the RPC
// backend (wx_backend.cpp). It must stay in one place: a method is only listed once the
// backend really implements it.
//
// "message.create" is the reflection-based sender (wx_send.cpp). It is opt-in through the
// module configuration, so the list is only stable within a process and only meaningful
// after SetSendEnabled() has been called for that process.
namespace satori {
void SetSendEnabled(bool enabled);
bool SendEnabled();
const char *const *WeChatFeatures(size_t *count);
// Standard methods WeChat cannot express at all (message.update, reaction.*, custom roles).
const char *const *WeChatUnsupported(size_t *count);
} // namespace satori
