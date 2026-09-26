#pragma once
#include "protocol.h"

namespace satori {
struct Adapter;
// The adapter owns no file descriptors and never blocks the server loop. It scans the
// data directory on demand and turns snapshot changes into login events on the bus.
Adapter *CreateAdapter(const char *data_dir, EventBus *bus);
void DestroyAdapter(Adapter *adapter);
// Performs one read-only scan and publishes at most one login event.
// False means the scan could not be read (state is kept) or publication failed (retried next scan).
bool AdapterRefresh(Adapter *adapter);
} // namespace satori
