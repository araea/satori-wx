#pragma once
// Captures WeChat's SQLCipher key at the app's own JNI RegisterNatives boundary.
//
// WeChat derives the database key at runtime; the only way to learn it is to observe
// com.tencent.wcdb.core.Database.setCipherKey / nativeSetKey. This installs an observer on
// env->functions->RegisterNatives (a writable data table, no code patching) and writes the
// captured spec to <app_data_dir>/files/satori-wx/key.log, which wx_live.cpp then reads to
// open EnMicroMsg.db read-only. It replaces the old optional probe module.
namespace satori {
// Starts the capture thread. Safe to call once from postAppSpecialize after the target check.
void KeyCaptureStart(void *vm, const char *app_data_dir);
} // namespace satori
