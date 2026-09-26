#!/data/data/com.termux/files/usr/bin/bash
# Termux arm64 native build. No JDK, SDK, D8, DEX or shared C++ runtime.
set -euo pipefail
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
R=${SATORI_WX_ROOT:-$SCRIPT_DIR}
OUT=${SATORI_WX_OUT:-$R/build}
CXX=${CXX:-clang++}
CC=${CC:-clang}
MODE=${1:-server}
case "$MODE" in server|probe) ;; *) echo "usage: $0 [server|probe]" >&2; exit 2 ;; esac
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)
VERSION=$(sed -n 's/^version=//p' "$R/module.prop")
WORK=$(mktemp -d "$OUT/.native-XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
mkdir -p "$WORK/module/zygisk"
FLAGS=(-std=c++20 -O2 -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti
       -fno-threadsafe-statics -nostdinc++ -nostdlib++ -Wall -Wextra -Werror
       -I "$R/native")
if [ "$MODE" = server ]; then
    "$CC" -std=c11 -O2 -fPIC -fvisibility=hidden -DCJSON_HIDE_SYMBOLS -DCJSON_NESTING_LIMIT=16 \
        -c "$R/native/vendor/cjson/cJSON.c" -o "$WORK/cjson.o"
    "$CXX" "${FLAGS[@]}" -shared "$R/native/module.cpp" "$R/native/server.cpp" "$R/native/protocol.cpp" "$R/native/multipart.cpp" \
        "$R/native/webhook.cpp" "$R/native/wx_account.cpp" "$R/native/wx_adapter.cpp" "$R/native/wcdb.cpp" "$WORK/cjson.o" \
        -Wl,--no-undefined,-z,relro,-z,now -llog -o "$WORK/module/zygisk/arm64-v8a.so"
    "$CXX" "${FLAGS[@]}" "$R/tools/device_verify.cpp" "$R/native/server.cpp" "$R/native/protocol.cpp" \
        "$R/native/multipart.cpp" "$R/native/webhook.cpp" "$WORK/cjson.o" -Wl,--no-undefined -o "$OUT/satori-wx-check"
    "$CXX" "${FLAGS[@]}" "$R/tools/account_probe.cpp" "$R/native/wx_account.cpp" "$R/native/protocol.cpp" \
        "$WORK/cjson.o" -Wl,--no-undefined -o "$OUT/satori-wx-account"
    "$CXX" "${FLAGS[@]}" "$R/tools/wcdb_probe.cpp" "$R/native/wcdb.cpp" \
        -Wl,--no-undefined -ldl -o "$OUT/satori-wx-wcdb"
    cp "$R/module.prop" "$R/customize.sh" "$R/service.sh" "$WORK/module/"
    mkdir -p "$WORK/module/licenses"
    cp "$R/native/vendor/cjson/LICENSE" "$WORK/module/licenses/cJSON.txt"
else
    "$CXX" "${FLAGS[@]}" -shared "$R/native/probe.cpp" -Wl,--no-undefined,-z,relro,-z,now \
        -llog -o "$WORK/module/zygisk/arm64-v8a.so"
    sed 's/^id=.*/id=satori_wx_probe/; s/^name=.*/name=知言 native 边界探针/; s/^description=.*/description=可选 JNI 注册观测实验，不提供 Satori 服务。/' \
        "$R/module.prop" > "$WORK/module/module.prop"
fi
patchelf --remove-rpath "$WORK/module/zygisk/arm64-v8a.so"
if [ "$MODE" = server ]; then patchelf --remove-rpath "$OUT/satori-wx-check"; patchelf --remove-rpath "$OUT/satori-wx-account"; patchelf --remove-rpath "$OUT/satori-wx-wcdb"; fi
python3 "$R/tools/check-native.py" "$WORK/module/zygisk/arm64-v8a.so"
ZIP="$OUT/satori-wx-$MODE-$VERSION.zip"
(cd "$WORK/module" && zip -qr "$WORK/artifact.zip" .)
# Replace only owned build outputs; old versioned ZIPs remain available for rollback.
rm -rf -- "$OUT/module-$MODE"
mv "$WORK/module" "$OUT/module-$MODE"
mv "$WORK/artifact.zip" "$ZIP"
printf 'built: %s\n' "$ZIP"
