#pragma once
#include "vendor/cjson/cJSON.h"
#include <stddef.h>

// In-process residency for the WeChat main process.
//
// Everything here runs inside com.tencent.mm under WeChat's own identity and uses only
// public Android framework APIs through JNI: no dex is loaded, no class is defined at
// runtime and no ArtMethod is touched. Four things are provided:
//
//   * a resident status notification (low importance, ongoing) whose tap opens WeChat and
//     whose action button toggles a wake lock; the uptime on it is the system's own chronometer,
//     so nothing redraws it;
//   * a partial CPU wake lock plus a best-effort high-performance Wi-Fi lock, toggled by the
//     notification button or by POST /v1/internal/wakelock; the module also takes a ref-counted
//     hold around outbound work and sustains the Wi-Fi lock while a Satori client is attached,
//     mirroring satori-qq's WakeLockCtl;
//   * a periodic restart of WeChat's own core service, which keeps the main process at
//     SERVICE_ADJ instead of CACHED so the system freezer leaves it alone;
//   * the published state file (`files/satori-wx/state.json` in WeChat's data directory): the
//     module's own view of itself, rewritten whenever it changes, so a reader with root learns the
//     state even while the system has the process frozen and the HTTP port cannot answer.
//
// None of it polls. One thread sleeps on a condition variable and wakes for three reasons only:
// something it shows changed (the producers call NotifyStateChanged(), see protocol.h), the ten
// minute heartbeat (core-service restart, and a check that the notification and the channel are
// still there), or a deadline of its own (a send probe retry; an automatic hold expiring).
//
// The notification action button is a broadcast to the companion Zhiyan app
// (com.satori.wx/.keepalive.WakeToggleReceiver); the app is a normal Android app and is the
// only place a BroadcastReceiver can live, because the module deliberately ships no dex.
namespace satori {
struct Config;

// Starts the keeper thread. `vm` may be null (no JNI: only the state file is published, which is
// what the host tests exercise). `data_dir` is WeChat's private data directory; null or empty
// disables the state file. `instance` is 0 for the module; the hot-load development entry passes
// its generation (> 0) so its notification, channel, wake locks and state file never collide with
// the pinned module's, and it does not restart WeChat's core service.
void KeepaliveStart(void *vm, const Config &config, const char *data_dir, int instance = 0, bool enabled = true);
// Master switch: false cancels the notification, releases both locks, stops the core-service
// restart and publishes an "enabled":false state file. true resumes all of it on a later pass.
void KeepaliveSetEnabled(bool enabled);
bool KeepaliveEnabled();
void KeepaliveWakelock(int action, bool *held); // action: 0=off, 1=on, 2=toggle
// Ref-counted automatic hold for the duration of one outbound mutation. A wedged send cannot
// keep the CPU awake longer than the safety timeout; nested calls share one underlying lock.
// Unlike KeepaliveWakelock() this never redraws the notification (the user intent is unchanged).
void KeepaliveWakelockBegin();
void KeepaliveWakelockEnd();
// Adds the "keepalive" block to /v1/internal/status and /v1/internal/capabilities. Reads cached
// values only (no JNI), so it is cheap on the server thread; it also asks the keeper, at most every
// 30 seconds, to verify that the notification is still posted.
void KeepaliveStatus(cJSON *object);

// What the notification shows is a pure function of these.
struct KeepaliveInputs {
    bool online;    // at least one login is online
    bool listening; // the loopback listener is bound
    int clients;    // attached /v1/events WebSocket clients
    unsigned port;
    int send_state;             // SendState (wx_send.h)
    long long serving_since_ms; // wall clock when online && listening became true; 0 when it is not
    long long now_ms;           // wall clock, only to decide whether the start time needs a date
};
struct KeepaliveView {
    char title[96];
    char text[160]; // the collapsed row, under the title
    char big[320];  // the expanded body: `text`, when the service came up, and anything wrong with sending
    int color;      // accent color (ARGB)
    bool timer;     // show the system chronometer (uptime) counting from serving_since_ms
};
void KeepaliveRender(const KeepaliveInputs &in, KeepaliveView *out);
// The published state document (compact JSON, caller frees) for the given inputs. `changed_at_ms`
// < 0 leaves the timestamp out, which is the form the keeper compares to decide whether to rewrite.
// Exposed for tests.
char *KeepaliveStateJson(long long changed_at_ms);
} // namespace satori
