// Host-side tests for the canonical feature list, the opt-in sender gating and the
// send/send_allow configuration parsing. Nothing here talks to a device or to Java.
#include "server.h"
#include "wx_capabilities.h"
#include "wx_send.h"
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {
int failures = 0;

void Check(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

bool HasFeature(const char *const *list, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(list[i], name)) return true;
    return false;
}

// Feeds a configuration body through a pipe, exactly like the module reads the real file.
bool Parse(const char *body, satori::Config *config) {
    int fds[2];
    if (pipe(fds)) return false;
    const size_t size = strlen(body);
    if (write(fds[1], body, size) != static_cast<ssize_t>(size)) { close(fds[0]); close(fds[1]); return false; }
    close(fds[1]);
    const bool ok = satori::ReadConfig(fds[0], config);
    close(fds[0]);
    return ok;
}

void TestFeatures() {
    satori::SetSendEnabled(false);
    size_t count = 0;
    const char *const *list = satori::WeChatFeatures(&count);
    Check(count == 14, "read-only feature count is 14");
    Check(!HasFeature(list, count, "message.create"), "message.create absent while sender is off");
    Check(HasFeature(list, count, "message.list"), "message.list present");
    Check(HasFeature(list, count, "guild.member.list"), "guild.member.list present");
    Check(HasFeature(list, count, "guild.role.list"), "guild.role.list present");
    Check(HasFeature(list, count, "guild.member.role.list"), "guild.member.role.list present");
    Check(HasFeature(list, count, "user.channel.create"), "user.channel.create present");
    Check(HasFeature(list, count, "upload.create"), "upload.create present without the sender");

    satori::SetSendEnabled(true);
    list = satori::WeChatFeatures(&count);
    Check(count == 20, "feature count is 20 when the sender is on");
    Check(HasFeature(list, count, "message.create"), "message.create present when the sender is on");
    Check(HasFeature(list, count, "message.delete"), "message.delete present when the sender is on");
    Check(HasFeature(list, count, "channel.delete"), "channel.delete present when the sender is on");
    Check(HasFeature(list, count, "guild.member.kick"), "guild.member.kick present when the sender is on");
    Check(HasFeature(list, count, "guild.member.role.set"), "guild.member.role.set present when the sender is on");
    Check(HasFeature(list, count, "guild.member.role.unset"), "guild.member.role.unset present when the sender is on");
    Check(HasFeature(list, count, "upload.create"), "upload.create present when the sender is on");
    satori::SetSendEnabled(false);

    // Unsupported methods are WeChat-side impossibilities, not unimplemented features.
    size_t unsupported = 0;
    const char *const *no = satori::WeChatUnsupported(&unsupported);
    Check(unsupported == 8, "unsupported count is 8");
    Check(HasFeature(no, unsupported, "message.update"), "message.update declared unsupported");
    Check(HasFeature(no, unsupported, "reaction.create"), "reaction.create declared unsupported");
    Check(HasFeature(no, unsupported, "guild.role.create"), "guild.role.create declared unsupported");
    Check(!HasFeature(no, unsupported, "message.create"), "message.create is not unsupported");
    Check(!HasFeature(no, unsupported, "message.delete"), "message.delete is not unsupported");
    // A method must never be both implemented and unsupported.
    list = satori::WeChatFeatures(&count);
    for (size_t i = 0; i < unsupported; ++i) Check(!HasFeature(list, count, no[i]), "feature and unsupported sets are disjoint");
}

void TestConfig() {
    // TokenValid requires 32-128 characters, so every fixture uses a realistic token.
    const char *const token = "0123456789abcdef0123456789abcdef";
    char body[1024];
    satori::Config config;

    snprintf(body, sizeof(body), "port=5601\ntoken=%s\n", token);
    Check(Parse(body, &config), "baseline config parses");
    Check(!config.send, "send defaults to off");

    // The retired whitelist key is still accepted (and ignored) so older configs keep working.
    snprintf(body, sizeof(body), "# comment\nport=5601\ntoken=%s\nsend=on\nsend_allow=wxid_abc;123@chatroom\n", token);
    Check(Parse(body, &config), "legacy send_allow key still parses");
    Check(config.send, "send=on parsed");

    snprintf(body, sizeof(body), "token=%s\nsend=off\n", token);
    Check(Parse(body, &config), "send=off parses");
    Check(!config.send, "send=off means disabled");

    snprintf(body, sizeof(body), "token=%s\nsend=on\n", token);
    Check(Parse(body, &config), "send without a whitelist parses");
    Check(config.send, "send=on is enabled at parse time");

    snprintf(body, sizeof(body), "token=%s\nsend=maybe\n", token);
    Check(!Parse(body, &config), "invalid send value rejected");
    snprintf(body, sizeof(body), "token=%s\nfoo=bar\n", token);
    Check(!Parse(body, &config), "unknown key rejected");
    snprintf(body, sizeof(body), "token=%s\nsend_allow=a\nsend_allow=b\n", token);
    Check(!Parse(body, &config), "repeated legacy key rejected");
    Check(!Parse("token=invalid token!\n", &config), "invalid token rejected");
    Check(!Parse("port=5601\n", &config), "missing token rejected");
}

void TestSendGating() {
    satori::SetSendEnabled(false);
    satori::SendResult off = satori::SendText("wxid_abc", "hello");
    Check(!off.ok, "send refused while disabled");
    Check(strstr(off.detail, "disabled") != nullptr, "disabled reason reported");

    // Any talker is eligible now; without a JavaVM the attempt reaches the environment step.
    satori::SetSendEnabled(true);
    satori::SendResult no_vm = satori::SendText("wxid_abc", "hello");
    Check(!no_vm.ok, "send without a JavaVM fails");
    Check(strstr(no_vm.detail, "JavaVM") != nullptr, "missing JavaVM reported");

    satori::SendResult empty = satori::SendText("123@chatroom", "");
    Check(!empty.ok, "empty content refused");

    // There is no pacing any more: consecutive attempts all reach the environment step
    // instead of being refused by the sender's own policy.
    satori::SendResult again = satori::SendText("123@chatroom", "hello");
    Check(!again.ok && !again.rejected, "first follow-up attempt reaches the environment step");
    satori::SendResult second = satori::SendText("123@chatroom", "hello");
    Check(!second.ok && !second.rejected && strstr(second.detail, "JavaVM") != nullptr,
          "consecutive sends are not rate limited");

    satori::SetSendEnabled(false);
}

void TestSendStatus() {
    satori::SetSendEnabled(false);
    satori::SendStatus status{};
    satori::SendStatusGet(&status);
    Check(!status.enabled, "status: disabled by default");
    Check(!status.ready, "status: no JavaVM is wired without SendInit");
    Check(!status.resolved, "status: classes unresolved");
    Check(!status.dispatcher, "status: dispatcher unknown");
    Check(status.last_age_ms == -1, "status: no attempt recorded yet");
    const long long rejected_before = status.rejected;
    const long long failed_before = status.failed;

    const satori::SendResult denied = satori::SendText("wxid_x", "hi");
    Check(denied.rejected, "disabled send is marked rejected");
    satori::SendStatusGet(&status);
    Check(status.rejected == rejected_before + 1, "status: policy rejection counted");
    Check(!status.last_ok, "status: last attempt not ok");
    Check(!strcmp(status.last_target, "wxid_x"), "status: last target recorded");
    Check(strstr(status.last_error, "disabled") != nullptr, "status: last error recorded");
    Check(status.last_age_ms >= 0, "status: attempt age recorded");

    satori::SetSendEnabled(true);
    satori::SendStatusGet(&status);
    Check(status.enabled, "status: enabled follows configuration");

    // No JavaVM: an environment failure, not a policy rejection.
    const satori::SendResult no_vm = satori::SendText("wxid_x", "hi");
    Check(!no_vm.ok && !no_vm.rejected, "environment failure is not a policy rejection");
    satori::SendStatusGet(&status);
    Check(status.failed == failed_before + 1, "status: environment failure counted as failed");
    Check(status.rejected == rejected_before + 1, "status: rejection counter unchanged by it");
    Check(!status.resolved && !status.dispatcher, "status: still unresolved and undispatched");

    satori::SetSendEnabled(false);
}
} // namespace

int main() {
    TestFeatures();
    TestConfig();
    // Runs before any other send attempt so the "no attempt yet" state is observable.
    TestSendStatus();
    TestSendGating();
    if (failures) {
        fprintf(stderr, "%d capability test(s) failed\n", failures);
        return 1;
    }
    printf("capability tests: PASS\n");
    return 0;
}
