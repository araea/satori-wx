#pragma once
#include <stddef.h>

// Read-only identity source for the WeChat adapter.
//
// The module runs inside the WeChat main process with WeChat's own uid, so it can
// read that app's private SharedPreferences files directly. This is deliberately
// hook-free: no JNI table patching, no Java classes, no database access, no write.
// It only observes what WeChat already persisted about the last login.
namespace satori {
struct Account {
    bool exists = false;   // an account identity is present (wxid + uin)
    bool online = false;   // WeChat's persisted isLogin flag, not a network probe
    char wxid[80] = {};
    char uin[24] = {};
    char alias[128] = {};
    char nickname[256] = {};
    char mobile[32] = {};
};

// Reads <data_dir>/shared_prefs/{com.tencent.mm_preferences,auth_info_key_prefs}.xml.
// False means the preferences could not be read at all; it does not mean "logged out".
// On success exists/online and the fields describe the best available snapshot.
bool ReadAccount(const char *data_dir, Account *account);

// Builds a complete Satori login event for the event bus.
// type is "login-added", "login-updated" or "login-removed".
// Returns a malloc'd JSON string, or null on allocation/format failure.
char *AccountEvent(const char *type, const Account &account, int sn);

// Identity is the platform user id; a change means remove + add rather than update.
bool SameIdentity(const Account &a, const Account &b);
// Same login means no snapshot field the protocol exposes has changed.
bool SameLogin(const Account &a, const Account &b);
} // namespace satori
