#include "wx_adapter.h"
#include "wx_account.h"
#include <stdlib.h>
#include <string.h>

namespace satori {
struct Adapter {
    char data_dir[256];
    EventBus *bus;
    Account current;
    bool published;
    int sn;      // sn of the login currently in the hub snapshot
    int next_sn; // allocated on add so a re-login never reuses an old sn
};

Adapter *CreateAdapter(const char *data_dir, EventBus *bus) {
    if (!data_dir || !*data_dir || !bus || strlen(data_dir) >= sizeof(Adapter::data_dir)) return nullptr;
    auto *adapter = static_cast<Adapter *>(calloc(1, sizeof(Adapter)));
    if (!adapter) return nullptr;
    strcpy(adapter->data_dir, data_dir);
    adapter->bus = bus;
    adapter->next_sn = 1;
    return adapter;
}

void DestroyAdapter(Adapter *adapter) { free(adapter); }

namespace {
bool PublishLogin(Adapter *adapter, const char *type, const Account &account, int sn) {
    char *json = AccountEvent(type, account, sn);
    if (!json) return false;
    const bool ok = Publish(adapter->bus, json);
    free(json);
    return ok;
}
} // namespace

bool AdapterRefresh(Adapter *adapter) {
    if (!adapter) return false;
    Account next;
    if (!ReadAccount(adapter->data_dir, &next)) return false;
    if (next.exists) {
        if (!adapter->published) {
            const int sn = adapter->next_sn;
            if (!PublishLogin(adapter, "login-added", next, sn)) return false;
            adapter->current = next;
            adapter->published = true;
            adapter->sn = sn;
            ++adapter->next_sn;
        } else if (!SameIdentity(adapter->current, next)) {
            // A different account must not be folded into the old snapshot.
            if (!PublishLogin(adapter, "login-removed", adapter->current, adapter->sn)) return false;
            adapter->published = false;
            adapter->current = Account{};
        } else if (!SameLogin(adapter->current, next)) {
            if (!PublishLogin(adapter, "login-updated", next, adapter->sn)) return false;
            adapter->current = next;
        }
    } else if (adapter->published) {
        if (!PublishLogin(adapter, "login-removed", adapter->current, adapter->sn)) return false;
        adapter->published = false;
        adapter->current = Account{};
    }
    return true;
}
} // namespace satori
