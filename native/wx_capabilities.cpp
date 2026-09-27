#include "wx_capabilities.h"

namespace satori {
namespace {
volatile bool g_send_enabled = false;
const char *const kReadOnly[] = {
    "message.get", "message.list",
    "user.get", "friend.list",
    "guild.get", "guild.list",
    "channel.get", "channel.list",
};
const char *const kWithSend[] = {
    "message.get", "message.list",
    "user.get", "friend.list",
    "guild.get", "guild.list",
    "channel.get", "channel.list",
    "message.create",
};
} // namespace

void SetSendEnabled(bool enabled) { g_send_enabled = enabled; }
bool SendEnabled() { return g_send_enabled; }

const char *const *WeChatFeatures(size_t *count) {
    if (g_send_enabled) {
        if (count) *count = sizeof(kWithSend) / sizeof(kWithSend[0]);
        return kWithSend;
    }
    if (count) *count = sizeof(kReadOnly) / sizeof(kReadOnly[0]);
    return kReadOnly;
}
} // namespace satori
