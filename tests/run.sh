#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
R=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$R/build/tests"
clang -std=c11 -O1 -g -DCJSON_NESTING_LIMIT=16 -c "$R/native/vendor/cjson/cJSON.c" -o "$R/build/tests/cjson.o"
clang++ -std=c++20 -O1 -g -fno-exceptions -fno-rtti -nostdinc++ -nostdlib++ -Wall -Wextra -Werror \
    -I "$R/native" "$R/native/server.cpp" "$R/native/protocol.cpp" "$R/native/multipart.cpp" \
    "$R/native/tempstore.cpp" "$R/native/webhook.cpp" "$R/native/wx_account.cpp" "$R/native/wx_adapter.cpp" "$R/native/wx_capabilities.cpp" \
    "$R/tests/server_main.cpp" \
    "$R/build/tests/cjson.o" -o "$R/build/tests/server"
python3 "$R/tests/server_test.py" "$R/build/tests/server"
clang++ -std=c++20 -O1 -g -fno-exceptions -fno-rtti -fno-threadsafe-statics -nostdinc++ -nostdlib++ \
    -Wall -Wextra -Werror -I "$R/native" "$R/tests/account_test.cpp" "$R/native/wx_account.cpp" "$R/native/wx_adapter.cpp" \
    "$R/native/protocol.cpp" "$R/native/wx_capabilities.cpp" "$R/build/tests/cjson.o" -o "$R/build/tests/account-test"
mkdir -p "$R/build/tests/tmp"
SATORI_ACCOUNT_TMP="$R/build/tests/tmp" "$R/build/tests/account-test"
clang++ -std=c++20 -O1 -g -fno-exceptions -fno-rtti -nostdinc++ -nostdlib++ \
    -Wall -Wextra -Werror -I "$R/native" "$R/tests/wcdb_test.cpp" "$R/native/wcdb.cpp" -ldl -o "$R/build/tests/wcdb-test"
SATORI_WCDB_LIB="${SATORI_WCDB_LIB:-/data/data/com.termux/files/usr/lib/libsqlite3.so}" \
SATORI_ACCOUNT_TMP="$R/build/tests/tmp" "$R/build/tests/wcdb-test"
clang++ -std=c++20 -O1 -g -fno-exceptions -fno-rtti -nostdinc++ -nostdlib++ \
    -Wall -Wextra -Werror -I "$R/native" "$R/tests/store_test.cpp" "$R/native/wx_store.cpp" "$R/native/wcdb.cpp" \
    "$R/native/protocol.cpp" "$R/build/tests/cjson.o" -ldl -o "$R/build/tests/store-test"
SATORI_WCDB_LIB="${SATORI_WCDB_LIB:-/data/data/com.termux/files/usr/lib/libsqlite3.so}" \
SATORI_ACCOUNT_TMP="$R/build/tests/tmp" "$R/build/tests/store-test"
python3 "$R/tests/protocol_test.py" "$R/build/tests/server"
clang++ -std=c++20 -O1 -g -fno-exceptions -fno-rtti -nostdinc++ -nostdlib++ \
    -Wall -Wextra -Werror -I "$R/native" "$R/tests/capabilities_test.cpp" "$R/native/wx_capabilities.cpp" \
    "$R/native/wx_send.cpp" "$R/native/server.cpp" "$R/native/protocol.cpp" "$R/native/multipart.cpp" \
    "$R/native/tempstore.cpp" "$R/native/webhook.cpp" "$R/native/wx_account.cpp" "$R/native/wx_adapter.cpp" \
    "$R/build/tests/cjson.o" -llog -o "$R/build/tests/capabilities-test"
"$R/build/tests/capabilities-test"
clang++ -std=c++20 -O1 -g -fno-exceptions -fno-rtti -nostdinc++ -nostdlib++ \
    -Wall -Wextra -Werror -I "$R/native" "$R/tests/keepalive_test.cpp" "$R/native/wx_keepalive.cpp" \
    "$R/native/protocol.cpp" "$R/build/tests/cjson.o" -llog -o "$R/build/tests/keepalive-test"
"$R/build/tests/keepalive-test"
clang++ -std=c++20 -O1 -g -fno-exceptions -fno-rtti -fno-threadsafe-statics -nostdinc++ -nostdlib++ \
    -Wall -Wextra -Werror -I "$R/native" "$R/tests/content_test.cpp" "$R/native/protocol.cpp" \
    "$R/build/tests/cjson.o" -o "$R/build/tests/content-test"
"$R/build/tests/content-test"
clang++ -std=c++20 -O1 -g -fno-exceptions -fno-rtti -nostdinc++ -nostdlib++ \
    -Wall -Wextra -Werror -I "$R/native" "$R/tests/tempstore_test.cpp" "$R/native/tempstore.cpp" \
    -o "$R/build/tests/tempstore-test"
SATORI_TMPROOT="$R/build/tests/tmp" "$R/build/tests/tempstore-test"
python3 "$R/tests/account_e2e_test.py" "$R/build/tests/server"
python3 "$R/tests/webhook_test.py" "$R/build/tests/server"
