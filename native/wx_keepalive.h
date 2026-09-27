#pragma once
#include "vendor/cjson/cJSON.h"

// In-process residency helper for the WeChat main process.
//
// Everything here runs inside com.tencent.mm under WeChat's own identity and uses only
// public Android framework APIs through JNI: no dex is loaded, no class is defined at
// runtime and no ArtMethod is touched. Three things are provided:
//
//   * a resident status notification (low importance, ongoing) whose tap opens WeChat and
//     whose action button toggles a wake lock;
//   * a partial CPU wake lock plus a best-effort high-performance Wi-Fi lock, toggled by the
//     notification button or by POST /v1/internal/wakelock;
//   * a periodic restart of WeChat's own core service, which keeps the main process at
//     SERVICE_ADJ instead of CACHED so the system freezer leaves it alone.
//
// The notification action button is a broadcast to the companion Zhiyan app
// (com.satori.wx/.keepalive.WakeToggleReceiver); the app is a normal Android app and is the
// only place a BroadcastReceiver can live, because the module deliberately ships no dex.
namespace satori {
struct Config;
void KeepaliveStart(void *vm, const Config &config);
void KeepaliveWakelock(int action, bool *held); // action: 0=off, 1=on, 2=toggle
// Adds the "keepalive" block to /v1/internal/status and /v1/internal/capabilities.
void KeepaliveStatus(cJSON *object);
} // namespace satori
