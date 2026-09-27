#!/data/data/com.termux/files/usr/bin/bash
# 真机设计冒烟：离屏渲染各状态的页面，检查触达面积、图标按钮名称、文字截断，并取回长页截图。
# 前置：已安装当前构建的 com.satori.wx（../../build.sh + 装机），本机有 root（su，用于安装与取图）。
# 不连服务、不调 su、不改配置、不点屏幕。
set -euo pipefail
APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="$APP_DIR/build/design-tests"
ANDROID_JAR=/data/data/com.termux/files/home/android/platform/android-35/android.jar
BT=/data/data/com.termux/files/home/android/android-sdk-tools/build-tools
KS="$APP_DIR/keystore/zhiyan.keystore"
[ -d "$APP_DIR/build/classes" ] || { echo "先跑一次 build.sh" >&2; exit 1; }

rm -rf "$OUT" && mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/design-review"
echo "== 1. javac（对照应用的类编译，但不把它们打进测试包）=="
javac -classpath "$ANDROID_JAR:$APP_DIR/build/classes" -source 8 -target 8 -encoding UTF-8 -nowarn -Xlint:-options \
  -d "$OUT/classes" "$APP_DIR/tests/ui/DesignSmoke.java"
echo "== 2. d8 =="
java -cp "$APP_DIR/libs/r8.jar" com.android.tools.r8.D8 --release --min-api 26 --lib "$ANDROID_JAR" \
  --classpath "$APP_DIR/build/classes" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class')
echo "== 3. package + sign（与应用同一把密钥）=="
"$BT/aapt" package -f -M "$APP_DIR/tests/ui/AndroidManifest.xml" -I /system/framework/framework-res.apk -F "$OUT/unsigned.apk" 2>/dev/null
( cd "$OUT/dex" && "$BT/aapt" add "$OUT/unsigned.apk" classes.dex >/dev/null )
"$BT/zipalign" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
apksigner sign --ks "$KS" --ks-pass pass:zhiyan-local --key-pass pass:zhiyan-local --out "$OUT/DesignSmoke.apk" "$OUT/aligned.apk"

echo "== 4. 安装并运行 =="
su -c "cp '$OUT/DesignSmoke.apk' /data/local/tmp/zhiyan-smoke.apk && pm install -r -d /data/local/tmp/zhiyan-smoke.apk >/dev/null && rm /data/local/tmp/zhiyan-smoke.apk"
# ColorOS 的 Hans 会冻结在后台停留几秒的应用进程，测试进程也不例外；被冻住时 am instrument 会一直等下去。
# 给它一个上限，并说清原因，而不是无声挂起。
if ! su -c "timeout 150 am instrument -w -r com.satori.wx.test/com.satori.wx.ui.DesignSmoke" > "$OUT/instrument.log"; then
  pid=$(su -c "pidof com.satori.wx" || true)
  if [ -n "$pid" ] && su -c "grep -q freezer /proc/$pid/wchan"; then
    echo "测试进程被系统冻结（ColorOS Hans），测试没能跑完。息屏后重跑，或在系统设置里允许「知言」后台运行。" >&2
  fi
  su -c "am force-stop com.satori.wx" || true
fi
grep -E "shot|PASS|FAIL|at com" "$OUT/instrument.log" || true
grep -q 'PASS: 知言界面设计冒烟' "$OUT/instrument.log" || { echo "设计冒烟未通过，见 $OUT/instrument.log" >&2; exit 1; }

echo "== 5. 取回截图 =="
for f in $(su -c "ls /data/data/com.satori.wx/files/design-review/"); do
  su -c "cat /data/data/com.satori.wx/files/design-review/$f" > "$OUT/design-review/$f"
done
ls "$OUT/design-review"
