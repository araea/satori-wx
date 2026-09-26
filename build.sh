#!/data/data/com.termux/files/usr/bin/bash
# 知言（satori-wx）构建：Java dex（内嵌进 .so rodata）→ Zygisk 原生模块 → KernelSU/Magisk 模块包。
#
# 与知弦（satori-qq）同构：注入后进程已在应用沙箱里读不了 /data/adb/modules 下的文件，
# 所以 dex 用 .incbin 嵌在 .so 里，运行时 InMemoryDexClassLoader 加载。
#
# NOTE: libs/r8.jar 与 libs/json.jar 被 gitignore。首次克隆后从 satori-qq 拷或下载一次：
#   curl -fsSL -o libs/r8.jar https://maven.google.com/com/android/tools/r8/8.9.35/r8-8.9.35.jar
#   curl -fsSL -o libs/json.jar https://repo1.maven.org/maven2/org/json/json/20250517/json-20250517.jar
set -e
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
R=${SATORI_WX_ROOT:-$SCRIPT_DIR}
ANDROID_JAR=/data/data/com.termux/files/home/android/platform/android-35/android.jar
R8=$R/libs/r8.jar
OUT=${SATORI_WX_OUT:-$R/build}
VERSION=$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' $R/AndroidManifest.xml)
VERSION=${VERSION:-0.1.0}
MODID=satori_wx

echo "== 1. javac =="
rm -rf $OUT/classes && mkdir -p $OUT/classes
find $R/src -name '*.java' > $OUT/sources.txt
javac -classpath $ANDROID_JAR -source 8 -target 8 -encoding UTF-8 \
  -nowarn -d $OUT/classes @$OUT/sources.txt
echo "   compiled $(find $OUT/classes -name '*.class' | wc -l) classes"

echo "== 2. d8 -> dex =="
rm -rf $OUT/dex && mkdir -p $OUT/dex
find $OUT/classes -name '*.class' > $OUT/classlist.txt
java -cp $R8 com.android.tools.r8.D8 --release --min-api 26 \
  --lib $ANDROID_JAR --output $OUT/dex @$OUT/classlist.txt
echo "   dex: $(ls -la $OUT/dex/classes.dex | awk '{print $5}') bytes"

echo "== 2c. libwxcore.so（纯 JNI 层，无第三方依赖） =="
# dex 内嵌进 .so 的 rodata：注入后进程已在应用沙箱里，读不了 /data/adb/modules 下的文件。
cat > $OUT/dex_blob.S <<EOF
	.section .rodata
	.global satori_dex_start
satori_dex_start:
	.incbin "$OUT/dex/classes.dex"
	.global satori_dex_end
satori_dex_end:
EOF
clang++ -c -o $OUT/dex_blob.o $OUT/dex_blob.S
mkdir -p $OUT/zygisk
clang++ -shared -fPIC -std=c++20 -O2 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
  -nostdinc++ -nostdlib++ \
  -I $R/native \
  -o $OUT/zygisk/arm64-v8a.so $R/native/wx.cpp $OUT/dex_blob.o \
  -Wl,--no-undefined -llog
echo "   libwxcore.so: $(ls -la $OUT/zygisk/arm64-v8a.so | awk '{print $5}') bytes"
# -Wl,--no-undefined 保证符号在链接期对上；NEEDED 只许这四个（漏什么直接链接失败，
# 而不是等到 dlopen 才报，那要烧一次重启才发现）。
NEEDED=$(readelf -d $OUT/zygisk/arm64-v8a.so | grep NEEDED | grep -v 'liblog\|libdl\|libm\|libc\.so' || true)
if [ -n "$NEEDED" ]; then
  echo "   FAIL unexpected dependencies:"; echo "$NEEDED"; exit 1
fi

echo "== 3. 模块目录 =="
MODDIR=$OUT/module
rm -rf $MODDIR && mkdir -p $MODDIR/zygisk
cp $OUT/zygisk/arm64-v8a.so $MODDIR/zygisk/arm64-v8a.so

cat > $MODDIR/module.prop <<EOF
id=$MODID
name=知言
version=v$VERSION
versionCode=$(sed -n 's/.*android:versionCode="\([0-9]*\)".*/\1/p' $R/AndroidManifest.xml)
author=araea
description=微信的 Satori v1 实现端（Zygisk 注入，纯 JNI 层，不挂钩子引擎）。
EOF

# Zygisk Next 只认模块目录里这张表；缺它不会加载（踩过）。
cat > $MODDIR/zn_modules.txt <<EOF
name=com.tencent.mm zygisk/arm64-v8a.so
EOF

echo "== 4. zip =="
ZIP=$OUT/satori-wx-v$VERSION.zip
rm -f $ZIP
( cd $MODDIR && zip -qr $ZIP . )
echo "   zip: $ZIP"
echo "done. install: su -c 'cp -r $MODDIR /data/adb/modules/$MODID' && reboot"
