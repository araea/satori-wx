#pragma once
#include <stdint.h>
#include "vendor/cjson/cJSON.h"
namespace satori {
struct EventBus;
struct Backend;
struct Config {
    uint16_t port = 5601;
    char token[129] = {};
    // There is no send switch any more: the sender is always available. `send=on|off` and
    // `send_allow=` are still accepted in the config file (and ignored) so files written by
    // earlier versions, and the app's own older builds, keep starting the server.
};
// Optional backend-specific fields added to the /v1/internal/status and
// /v1/internal/capabilities objects. `capabilities` is true for the latter, where a backend
// may also report things like the set of standard methods it cannot express. The module
// registers one; tests and standalone tools leave it null so their responses stay minimal.
using StatusProvider = void (*)(cJSON *object, bool capabilities);
void SetStatusProvider(StatusProvider provider);
// Optional storage directory for the built-in `/v1/upload.create` implementation and the
// `internal:.../_tmp/...` targets of `/v1/proxy`. Called by the module with its app data dir.
void SetTempDir(const char *dir);
// Optional hook for POST /v1/internal/wakelock: the in-process keeper applies the wake lock
// and redraws its notification. `action` is 0=off, 1=on, 2=toggle; `held` receives the
// resulting user intent. When null the endpoint reports 501.
using WakelockProvider = void (*)(int action, bool *held);
void SetWakelockProvider(WakelockProvider provider);
// Resolves a message-media route under `/v1/proxy/internal:<platform>/<user>/<path>` (any path
// other than `_tmp/...`) to a local file. The resolver owns the authorization of the link: the
// route needs no Authorization header, so it must reject anything that was not issued by this
// process' own signer. `path` is what follows `<user>/`. Registered by the module; when null,
// every such route answers 404.
struct MediaFile {
    char path[1400];
    char content_type[64];
};
using MediaResolver = bool (*)(const char *user, const char *path, MediaFile *out);
void SetMediaResolver(MediaResolver resolver);
// fd is borrowed. Invalid/missing token prevents startup. No anonymous mode.
bool ReadConfig(int fd, Config *config);
int Listen(const Config &config); // Returns owned nonblocking loopback listener, or -1.
void Run(int listener, const Config &config, EventBus *bus = nullptr, const Backend *backend = nullptr); // Owns listener; blocking event loop.
} // namespace satori
