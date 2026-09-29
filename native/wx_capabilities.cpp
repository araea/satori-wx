#include "wx_capabilities.h"

namespace satori {
namespace {
const char *const kFeatures[] = {
    "message.get", "message.list",
    "user.get", "friend.list",
    "guild.get", "guild.list",
    "guild.member.get", "guild.member.list",
    "guild.role.list", "guild.member.role.list",
    "channel.get", "channel.list",
    "user.channel.create",
    "upload.create",
    "message.create",
    "message.delete",
    "channel.delete",
    "guild.member.kick",
    "guild.member.role.set",
    "guild.member.role.unset",
    "login.get",
    // A WeChat group chat is the guild and its only channel: `guild.id` == `channel.id`.
    "guild.plain",
};
// Standard methods WeChat has no concept for. Reported so clients can mark them unusable
// instead of retrying: WeChat messages cannot be edited and carry no reactions, groups have
// no user-defined roles and no sub-channels, and there is no server-side mute at all (only
// the local "mute notifications" switch, which is not a moderation action). message.delete
// is implemented, so it is not listed here.
const char *const kUnsupported[] = {
    "message.update",
    "channel.create", "channel.mute",
    "guild.member.mute",
    "reaction.create", "reaction.delete", "reaction.clear", "reaction.list",
    "guild.role.create", "guild.role.update", "guild.role.delete",
};
} // namespace

const char *const *WeChatFeatures(size_t *count) {
    if (count) *count = sizeof(kFeatures) / sizeof(kFeatures[0]);
    return kFeatures;
}

const char *const *WeChatUnsupported(size_t *count) {
    if (count) *count = sizeof(kUnsupported) / sizeof(kUnsupported[0]);
    return kUnsupported;
}
} // namespace satori
