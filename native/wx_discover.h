#pragma once
#include <jni.h>

// Automatic adaptation to a WeChat build nobody has looked at yet.
//
// When the built-in name sets (wx_names.h) no longer fit the installed APK, the sender asks this to
// derive a set from the APK itself (wx_derive.cpp) on a background thread, cache it under the app
// data directory for the next starts, and install it (WxNamesInstall). Until it finishes, sends fail
// with a version-mismatch detail, which is what they did before this existed.
namespace satori {

// Remembers the VM and the app data directory (postAppSpecialize). No work is done here.
void DiscoverInit(JavaVM *vm, const char *data_dir);

// Called by the sender after every built-in set failed to resolve. Cheap and idempotent: it looks
// for a cache for the installed APK first, and starts the derivation thread only when there is none.
// `env` must be usable on the calling thread.
void DiscoverStart(JNIEnv *env);

} // namespace satori
