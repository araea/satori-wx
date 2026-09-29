#!/data/data/com.termux/files/usr/bin/bash
# Builds the hot-load library from the working tree and injects it into the running WeChat main
# process. It listens on 127.0.0.1:<port> (printed) with the production token, so the new code
# can be exercised with the same HTTP calls without restarting the phone. Root (su) required.
#   tools/dev/dev.sh            build + inject a fresh generation
#   tools/dev/dev.sh --port     just print the port of the last generation
set -euo pipefail
R=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=$R/build/dev
mkdir -p "$OUT"
GEN_FILE=$OUT/gen
[ "${1:-}" = "--port" ] && { echo $((5610 + $(cat "$GEN_FILE") % 80)); exit 0; }
GEN=$(( $(cat "$GEN_FILE" 2>/dev/null || echo 0) + 1 ))
PORT=$((5610 + GEN % 80))
CXX=${CXX:-clang++}; CC=${CC:-clang}
FLAGS=(-std=c++20 -O2 -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -fno-threadsafe-statics
       -nostdinc++ -nostdlib++ -Wall -Wextra -Werror -I "$R/native")
[ -f "$OUT/cjson.o" ] || "$CC" -std=c11 -O2 -fPIC -fvisibility=hidden -DCJSON_HIDE_SYMBOLS -DCJSON_NESTING_LIMIT=16 \
    -c "$R/native/vendor/cjson/cJSON.c" -o "$OUT/cjson.o"
[ -x "$OUT/inject" ] && [ "$OUT/inject" -nt "$R/tools/dev/inject.c" ] || "$CC" -O2 -Wall -o "$OUT/inject" "$R/tools/dev/inject.c"
SRC=(server protocol multipart upload_stream tempstore webhook wx_account wx_adapter wx_live wx_watch wx_store wx_backend wx_capabilities
     wx_send wx_room wx_pat wx_message wx_events wx_media media xml_lite wx_keepalive wcdb)
SRC+=(wx_send_media mp4_probe)
FILES=("$R/tools/dev/dev_entry.cpp"); for s in "${SRC[@]}"; do FILES+=("$R/native/$s.cpp"); done
"$CXX" "${FLAGS[@]}" -shared "${FILES[@]}" "$OUT/cjson.o" -Wl,--no-undefined,-z,relro,-z,now -llog -ldl -o "$OUT/satori-wx-dev.so"
echo $GEN > "$GEN_FILE"
APP=/data/user/0/com.tencent.mm
SO=$APP/files/satori-wx-dev/dev-$GEN.so
su -c "
set -e
U=\$(stat -c %u $APP/files); mkdir -p $APP/files/satori-wx-dev
cp $OUT/satori-wx-dev.so $SO
TOKEN=\$(sed -n 's/^token=//p' /data/adb/modules/satori_wx/satori-wx.conf)
printf 'port=$PORT\ntoken=%s\n' \"\$TOKEN\" > ${SO%.so}.conf
chown -R \$U:\$U $APP/files/satori-wx-dev; chmod 700 $APP/files/satori-wx-dev; chmod 755 $SO; chmod 600 ${SO%.so}.conf
chcon -R \$(ls -dZ $APP/files | cut -d' ' -f1) $APP/files/satori-wx-dev
find $APP/files/satori-wx-dev -name 'dev-*' -mmin +240 -delete 2>/dev/null || true
timeout 30 $OUT/inject \$(pidof com.tencent.mm | awk '{print \$1}') $SO
"
echo "dev generation $GEN on 127.0.0.1:$PORT (token = production token)"
