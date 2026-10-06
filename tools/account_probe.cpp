// Read-only diagnostic for the WeChat account identity source.
// It never writes, hooks or talks to the network; it only prints what the adapter
// would derive from the app's SharedPreferences. Run it against the real data
// directory on device (needs the app uid or root) to validate the parsing.
#include "wx_account.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <app-data-dir>\n", argv[0]);
        return 2;
    }
    satori::Account account;
    if (!satori::ReadAccount(argv[1], &account)) {
        fprintf(stderr, "cannot read %s/shared_prefs/com.tencent.mm_preferences.xml\n", argv[1]);
        return 1;
    }
    printf("exists=%d online=%d\n", account.exists ? 1 : 0, account.online ? 1 : 0);
    printf("wxid=%s\nuin=%s\nalias=%s\nnick=%s\nmobile=%s\n", account.wxid, account.uin, account.alias,
           account.nickname, account.mobile);
    char *event = satori::AccountEvent("login-added", account, 1);
    if (event) {
        printf("event=%s\n", event);
        free(event);
    }
    return account.exists ? 0 : 3;
}
