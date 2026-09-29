#!/data/data/com.termux/files/usr/bin/bash
# Termux arm64 native build. No JDK, SDK, D8, DEX or shared C++ runtime.
set -euo pipefail
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
R=${SATORI_WX_ROOT:-$SCRIPT_DIR}
OUT=${SATORI_WX_OUT:-$R/build}
CXX=${CXX:-clang++}
CC=${CC:-clang}
if [ $# -gt 1 ]; then echo "usage: $0" >&2; exit 2; fi
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)
VERSION=$(sed -n 's/^version=//p' "$R/module.prop")
WORK=$(mktemp -d "$OUT/.native-XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
mkdir -p "$WORK/module/zygisk"
FLAGS=(-std=c++20 -O2 -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti
       -fno-threadsafe-statics -nostdinc++ -nostdlib++ -Wall -Wextra -Werror
       -I "$R/native")
"$CC" -std=c11 -O2 -fPIC -fvisibility=hidden -DCJSON_HIDE_SYMBOLS -DCJSON_NESTING_LIMIT=16 \
    -c "$R/native/vendor/cjson/cJSON.c" -o "$WORK/cjson.o"
"$CXX" "${FLAGS[@]}" -shared "$R/native/module.cpp" "$R/native/server.cpp" "$R/native/protocol.cpp" "$R/native/multipart.cpp" "$R/native/upload_stream.cpp" \
    "$R/native/tempstore.cpp" "$R/native/webhook.cpp" "$R/native/wx_account.cpp" "$R/native/wx_adapter.cpp" "$R/native/wx_live.cpp" "$R/native/wx_watch.cpp" \
    "$R/native/wx_store.cpp" "$R/native/wx_backend.cpp" "$R/native/wx_capabilities.cpp" "$R/native/wx_send.cpp" "$R/native/wx_send_media.cpp" "$R/native/mp4_probe.cpp" "$R/native/wx_voice.cpp" "$R/native/audio_pcm.cpp" \
    "$R/native/wx_room.cpp" "$R/native/wx_pat.cpp" "$R/native/wx_message.cpp" "$R/native/wx_events.cpp" "$R/native/wx_media.cpp" "$R/native/media.cpp" "$R/native/xml_lite.cpp" \
    "$R/native/wx_keepalive.cpp" \
    "$R/native/wx_key.cpp" "$R/native/wcdb.cpp" "$WORK/cjson.o" \
    -Wl,--no-undefined,-z,relro,-z,now -llog -ldl -o "$WORK/module/zygisk/arm64-v8a.so"
"$CXX" "${FLAGS[@]}" "$R/tools/device_verify.cpp" "$R/native/server.cpp" "$R/native/protocol.cpp" \
    "$R/native/multipart.cpp" "$R/native/upload_stream.cpp" "$R/native/tempstore.cpp" "$R/native/webhook.cpp" "$WORK/cjson.o" -Wl,--no-undefined -o "$OUT/satori-wx-check"
"$CXX" "${FLAGS[@]}" "$R/tools/account_probe.cpp" "$R/native/wx_account.cpp" "$R/native/protocol.cpp" \
    "$R/native/wx_capabilities.cpp" "$WORK/cjson.o" -Wl,--no-undefined -o "$OUT/satori-wx-account"
"$CXX" "${FLAGS[@]}" "$R/tools/wcdb_probe.cpp" "$R/native/wcdb.cpp" \
    -Wl,--no-undefined -ldl -o "$OUT/satori-wx-wcdb"
cp "$R/module.prop" "$R/customize.sh" "$R/service.sh" "$R/action.sh" "$R/tools/wxguard.sh" "$WORK/module/"
chmod 0755 "$WORK/module/service.sh" "$WORK/module/action.sh" "$WORK/module/wxguard.sh"
mkdir -p "$WORK/module/licenses"
cp "$R/native/vendor/cjson/LICENSE" "$WORK/module/licenses/cJSON.txt"
patchelf --remove-rpath "$WORK/module/zygisk/arm64-v8a.so"
patchelf --remove-rpath "$OUT/satori-wx-check"; patchelf --remove-rpath "$OUT/satori-wx-account"; patchelf --remove-rpath "$OUT/satori-wx-wcdb"
python3 "$R/tools/check-native.py" "$WORK/module/zygisk/arm64-v8a.so"
ZIP="$OUT/satori-wx-server-$VERSION.zip"
(cd "$WORK/module" && zip -qr "$WORK/artifact.zip" .)
# Replace only owned build outputs; old versioned ZIPs remain available for rollback.
rm -rf -- "$OUT/module-server"
mv "$WORK/module" "$OUT/module-server"
mv "$WORK/artifact.zip" "$ZIP"
printf 'built: %s\n' "$ZIP"
