#pragma once
#include <stddef.h>

// Canonical feature list shared by the account snapshot (wx_account.cpp) and the RPC
// backend (wx_backend.cpp). It must stay in one place: a method is only listed once the
// backend really implements it.
//
// The write methods (message.create and friends, sent through the reflection sender in
// wx_send.cpp) are always part of it: there is no switch. A client that must not send is a
// client that is not connected.
namespace satori {
const char *const *WeChatFeatures(size_t *count);
// Standard methods WeChat cannot express at all (message.update, reaction.*, custom roles).
const char *const *WeChatUnsupported(size_t *count);
} // namespace satori
