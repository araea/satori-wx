#pragma once
#include <stdint.h>
#include "vendor/cjson/cJSON.h"
namespace satori {
struct EventBus;
struct Backend;
struct Config {
    uint16_t port = 5601;
    char token[129] = {};
    // Opt-in reflection sender. Off unless the config says `send=on`, and then only the
    // talkers in send_allow (semicolon separated) are accepted.
    bool send = false;
    char send_allow[512] = {};
};
// Optional backend-specific fields added to the /v1/internal/status and
// /v1/internal/capabilities objects. The module registers one; tests and standalone tools
// leave it null so their responses stay minimal.
using StatusProvider = void (*)(cJSON *object);
void SetStatusProvider(StatusProvider provider);
// fd is borrowed. Invalid/missing token prevents startup. No anonymous mode.
bool ReadConfig(int fd, Config *config);
int Listen(const Config &config); // Returns owned nonblocking loopback listener, or -1.
void Run(int listener, const Config &config, EventBus *bus = nullptr, const Backend *backend = nullptr); // Owns listener; blocking event loop.
} // namespace satori
