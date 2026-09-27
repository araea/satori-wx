#!/data/data/com.termux/files/usr/bin/bash
# 知言应用的构建：生成物自检 → aapt R.java → javac → d8 → aapt 打包 → zipalign + 签名。
#
# 与模块（../build.sh）完全独立：模块是纯 native，不需要 JDK；应用需要 openjdk-17、aapt、
# zipalign、apksigner，以及 libs/ 下的 r8.jar 与 json.jar（被 gitignore，首次克隆后下载一次）：
#   curl -fsSL -o libs/r8.jar https://maven.google.com/com/android/tools/r8/8.9.35/r8-8.9.35.jar
#   curl -fsSL -o libs/json.jar https://repo1.maven.org/maven2/org/json/json/20250517/json-20250517.jar
#
# 签名密钥 keystore/zhiyan.keystore 只在缺失时生成一次。**不要删它**：换了密钥，已安装的应用就只能
# 卸载重装才能更新。
set -euo pipefail
R=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ANDROID_JAR=/data/data/com.termux/files/home/android/platform/android-35/android.jar
BT=/data/data/com.termux/files/home/android/android-sdk-tools/build-tools
FRAMEWORK=/system/framework/framework-res.apk
OUT=${ZHIYAN_OUT:-$R/build}
KS=$R/keystore/zhiyan.keystore
APK=$OUT/Zhiyan.apk

echo "== 0. 生成物自检 =="
python3 "$R/tools/make-tokens.py" --check
python3 "$R/tools/make-icons.py" --check

echo "== 1. aapt R.java =="
rm -rf "$OUT/gen" && mkdir -p "$OUT/gen"
"$BT/aapt" package -f -m -J "$OUT/gen" -M "$R/AndroidManifest.xml" -I "$FRAMEWORK" -S "$R/res"

echo "== 2. javac =="
rm -rf "$OUT/classes" && mkdir -p "$OUT/classes"
find "$R/src" "$OUT/gen" -name '*.java' > "$OUT/sources.txt"
javac -classpath "$ANDROID_JAR" -source 8 -target 8 -encoding UTF-8 -Xlint:-options \
  -d "$OUT/classes" @"$OUT/sources.txt"
echo "   compiled $(find "$OUT/classes" -name '*.class' | wc -l) classes"

echo "== 3. d8 =="
rm -rf "$OUT/dex" && mkdir -p "$OUT/dex"
find "$OUT/classes" -name '*.class' > "$OUT/classlist.txt"
java -cp "$R/libs/r8.jar" com.android.tools.r8.D8 --release --min-api 26 \
  --lib "$ANDROID_JAR" --output "$OUT/dex" @"$OUT/classlist.txt"

echo "== 4. aapt package =="
rm -f "$OUT/unsigned.apk" "$OUT/aligned.apk"
"$BT/aapt" package -f -M "$R/AndroidManifest.xml" -I "$FRAMEWORK" -S "$R/res" -F "$OUT/unsigned.apk"
( cd "$OUT/dex" && "$BT/aapt" add "$OUT/unsigned.apk" classes.dex >/dev/null )

echo "== 5. keystore（只在缺失时生成） =="
if [ ! -f "$KS" ]; then
  mkdir -p "$(dirname "$KS")"
  keytool -genkeypair -keystore "$KS" -alias zhiyan -storepass zhiyan-local -keypass zhiyan-local \
    -keyalg RSA -keysize 3072 -validity 10000 -dname "CN=Zhiyan" >/dev/null 2>&1
  echo "   generated $KS —— 不要删除"
fi

echo "== 6. zipalign + sign =="
"$BT/zipalign" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
apksigner sign --ks "$KS" --ks-pass pass:zhiyan-local --key-pass pass:zhiyan-local --out "$APK" "$OUT/aligned.apk"
rm -f "$OUT/unsigned.apk" "$OUT/aligned.apk"
ls -la "$APK"
