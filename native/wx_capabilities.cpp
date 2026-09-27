#include "wx_capabilities.h"

namespace satori {
namespace {
volatile bool g_send_enabled = false;
// Read surface backed by the read-only WeChat store (EnMicroMsg.db).
const char *const kReadOnly[] = {
    "message.get", "message.list",
    "user.get", "friend.list",
    "guild.get", "guild.list",
    "guild.member.get", "guild.member.list",
    "guild.role.list", "guild.member.role.list",
    "channel.get", "channel.list",
    "user.channel.create",
};
const char *const kWithSend[] = {
    "message.get", "message.list",
    "user.get", "friend.list",
    "guild.get", "guild.list",
    "guild.member.get", "guild.member.list",
    "guild.role.list", "guild.member.role.list",
    "channel.get", "channel.list",
    "user.channel.create",
    "message.create",
    "message.delete",
};
// Standard methods WeChat has no concept for. Reported so clients can mark them unusable
// instead of retrying: WeChat messages cannot be edited and carry no reactions, and groups
// have no user-defined roles. message.delete is implemented, so it is not listed here.
const char *const kUnsupported[] = {
    "message.update",
    "reaction.create", "reaction.delete", "reaction.clear", "reaction.list",
    "guild.role.create", "guild.role.update", "guild.role.delete",
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

const char *const *WeChatUnsupported(size_t *count) {
    if (count) *count = sizeof(kUnsupported) / sizeof(kUnsupported[0]);
    return kUnsupported;
}
} // namespace satori
