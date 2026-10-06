#!/data/data/com.termux/files/usr/bin/bash
# 原生模块的主机侧测试：把真正的源码逐个编成测试程序再跑，与模块用同一套编译选项。
set -euo pipefail
R=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
OUT=$R/build/tests
mkdir -p "$OUT/tmp"

export SATORI_WCDB_LIB=${SATORI_WCDB_LIB:-/data/data/com.termux/files/usr/lib/libsqlite3.so}
export SATORI_ACCOUNT_TMP=$OUT/tmp SATORI_TMPROOT=$OUT/tmp SATORI_WATCH_TMP=$OUT/tmp
export SATORI_FIXTURES=$R/tests/fixtures

FLAGS=(-std=c++20 -O1 -g -fno-exceptions -fno-rtti -fno-threadsafe-statics -nostdinc++ -nostdlib++
       -Wall -Wextra -Werror -I "$R/native")
clang -std=c11 -O1 -g -DCJSON_NESTING_LIMIT=16 -c "$R/native/vendor/cjson/cJSON.c" -o "$OUT/cjson.o"

# build <程序名> <链接选项，没有写 -> <源文件>...
# 源文件写 native/ 下的裸名字；带斜杠的按仓库根路径（tests/x.cpp）；cjson 指预编译的 cJSON。
build() {
    local program=$1 libs=$2 source
    local sources=()
    shift 2
    for source in "$@"; do
        case $source in
            cjson) sources+=("$OUT/cjson.o") ;;
            */*) sources+=("$R/$source") ;;
            *) sources+=("$R/native/$source.cpp") ;;
        esac
    done
    local linker=()
    [ "$libs" = - ] || read -r -a linker <<<"$libs"
    clang++ "${FLAGS[@]}" "${sources[@]}" "${linker[@]}" -o "$OUT/$program"
}

build server - server protocol multipart upload_stream tempstore webhook wx_account wx_adapter wx_capabilities \
    tests/server_main.cpp cjson
python3 "$R/tests/server_test.py" "$OUT/server"

build account-test - tests/account_test.cpp wx_account wx_adapter protocol wx_capabilities cjson
"$OUT/account-test"

build wcdb-test -ldl tests/wcdb_test.cpp wcdb
"$OUT/wcdb-test"

build store-test -ldl tests/store_test.cpp wx_store wcdb wx_message media xml_lite wx_media protocol cjson
"$OUT/store-test"

python3 "$R/tests/protocol_test.py" "$OUT/server"

build events-test -ldl tests/events_test.cpp wx_events wx_store wcdb wx_message media xml_lite protocol cjson
"$OUT/events-test"

build media-test - tests/media_test.cpp media xml_lite
"$OUT/media-test"

build mp4-test - tests/mp4_test.cpp mp4_probe
"$OUT/mp4-test"

build audio-test - tests/audio_test.cpp audio_pcm
"$OUT/audio-test"

build message-test - tests/message_test.cpp wx_message media xml_lite
"$OUT/message-test"

build capabilities-test -llog tests/capabilities_test.cpp wx_capabilities wx_send server protocol multipart \
    upload_stream tempstore webhook wx_account wx_adapter cjson
"$OUT/capabilities-test"

build keepalive-test -llog tests/keepalive_test.cpp wx_keepalive protocol cjson
"$OUT/keepalive-test"

build content-test - tests/content_test.cpp protocol cjson
"$OUT/content-test"

build watch-test -lpthread tests/watch_test.cpp wx_watch
"$OUT/watch-test"

build tempstore-test - tests/tempstore_test.cpp tempstore
"$OUT/tempstore-test"

build upload-stream-test - tests/upload_stream_test.cpp upload_stream multipart tempstore
"$OUT/upload-stream-test"

build forward-test - tests/forward_test.cpp wx_forward protocol media xml_lite cjson
"$OUT/forward-test"

build backend-test -llog tests/backend_test.cpp wx_backend wx_send mp4_probe wx_capabilities protocol tempstore \
    media wx_forward cjson
"$OUT/backend-test"

python3 "$R/tests/account_e2e_test.py" "$OUT/server"
python3 "$R/tests/webhook_test.py" "$OUT/server"

# 需要 root：wxguard 的事件驱动解冻，对着一个一次性 cgroup 跑（没有 root 自己跳过）。
if command -v su >/dev/null 2>&1 && su -c 'test -w /sys/fs/cgroup/apps/cgroup.procs' >/dev/null 2>&1; then
    su -c "timeout 120 sh '$R/tests/wxguard_thaw_test.sh'"
else
    echo "wxguard thaw test: SKIP (no root)"
fi
