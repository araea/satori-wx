#pragma once
#include "protocol.h"

namespace satori {
// Starts a background poller that reads the cipher spec captured by the optional probe
// (files/satori-wx-probe/key.log), opens the account message database read-only and
// publishes `message-created` events to the bus.
//
// Safety: only the spec that carries a cipher version (the setCipherKey path) is used to
// open the live database. Wrong keys are never tried against WeChat's live database, which
// previously corrupted a WAL mmap and crashed the host. If that single attempt fails, the
// store stays disabled.
bool StartLiveStore(const char *app_data_dir, EventBus *bus, int login_sn);
// The shared read-only store (null until opened) and its login sn. Queries are thread-safe.
struct Store;
Store *LiveStore();
int LiveLoginSn();
} // namespace satori
