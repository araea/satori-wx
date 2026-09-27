#!/data/data/com.termux/files/usr/bin/bash
# 知言应用的 JVM 测试：配置与服务端 ReadConfig 的一致性、状态推导、Design Tokens 合约。
# 真机界面验收另见 tests/ui/run.sh。
set -euo pipefail
R=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NATIVE=$(CDPATH= cd -- "$R/../native" && pwd)
ANDROID_JAR=/data/data/com.termux/files/home/android/platform/android-35/android.jar
JSON_JAR=$R/libs/json.jar
OUT=${ZHIYAN_OUT:-$R/build}/tests
rm -rf "$OUT" && mkdir -p "$OUT/classes"

echo "== 0. 服务端 ReadConfig（native/server.cpp）=="
clang -std=c11 -O1 -c "$NATIVE/vendor/cjson/cJSON.c" -o "$OUT/cjson.o"
clang++ -std=c++20 -O1 -fno-exceptions -fno-rtti -nostdinc++ -nostdlib++ -Wall -Wextra -Werror \
  -I "$NATIVE" "$R/tests/conf_parity.cpp" "$NATIVE/server.cpp" "$NATIVE/protocol.cpp" "$NATIVE/multipart.cpp" \
  "$NATIVE/webhook.cpp" "$OUT/cjson.o" -o "$OUT/conf-parity"

echo "== 1. javac =="
AAPT=/data/data/com.termux/files/home/android/android-sdk-tools/build-tools/aapt
mkdir -p "$OUT/gen"
"$AAPT" package -f -m -J "$OUT/gen" -M "$R/AndroidManifest.xml" -I /system/framework/framework-res.apk -S "$R/res" 2>/dev/null
find "$R/src/com/satori/wx/core" "$R/tests" -maxdepth 1 -name '*.java' > "$OUT/sources.txt"
javac -classpath "$JSON_JAR" -encoding UTF-8 -nowarn -d "$OUT/classes" @"$OUT/sources.txt"
# 界面层只做编译检查（它依赖 android.jar，不在 JVM 上跑）。
find "$R/src" "$OUT/gen" -name '*.java' > "$OUT/all.txt"
javac -classpath "$ANDROID_JAR" -encoding UTF-8 -nowarn -d "$OUT/android-classes" @"$OUT/all.txt"

echo "== 2. run =="
CP="$JSON_JAR:$OUT/classes"
java -cp "$CP" -Dparity="$OUT/conf-parity" ConfTest
java -cp "$CP" StatusTest
java -cp "$CP" -Dapp="$R" DesignTokenTest
if su -c true >/dev/null 2>&1; then
  java -cp "$CP" RootScriptTest
else
  echo "   skip RootScriptTest（没有 su）"
fi
echo "== DONE =="
