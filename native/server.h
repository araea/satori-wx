#pragma once
#include <stdint.h>
#include "vendor/cjson/cJSON.h"
namespace satori {
struct EventBus;
struct Backend;
struct Config {
    uint16_t port = 5601;
    char token[129] = {};
    // Opt-in reflection sender. Off unless the config says `send=on`. Once on, any talker is
    // accepted (the old send_allow whitelist was retired; the key is still accepted but
    // ignored so configs written before the change keep starting the server).
    bool send = false;
};
// Optional backend-specific fields added to the /v1/internal/status and
// /v1/internal/capabilities objects. `capabilities` is true for the latter, where a backend
// may also report things like the set of standard methods it cannot express. The module
// registers one; tests and standalone tools leave it null so their responses stay minimal.
using StatusProvider = void (*)(cJSON *object, bool capabilities);
void SetStatusProvider(StatusProvider provider);
// Optional hook for POST /v1/internal/wakelock: the in-process keeper applies the wake lock
// and redraws its notification. `action` is 0=off, 1=on, 2=toggle; `held` receives the
// resulting user intent. When null the endpoint reports 501.
using WakelockProvider = void (*)(int action, bool *held);
void SetWakelockProvider(WakelockProvider provider);
// fd is borrowed. Invalid/missing token prevents startup. No anonymous mode.
bool ReadConfig(int fd, Config *config);
int Listen(const Config &config); // Returns owned nonblocking loopback listener, or -1.
void Run(int listener, const Config &config, EventBus *bus = nullptr, const Backend *backend = nullptr); // Owns listener; blocking event loop.
} // namespace satori
