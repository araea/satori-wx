#!/data/data/com.termux/files/usr/bin/bash
# 知言（satori-wx）探针构建：纯 native Zygisk 模块，无 dex、无 APK。
#
# 产物 build/module/：
#   module.prop / zn_modules.txt / zygisk/arm64-v8a.so（+ 可选 service.sh 占位）
# 装 /data/adb/modules/satori_wx_probe/ 后重启生效（Zygisk Next 只在开机读模块）。
set -e
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
R=${SATORI_WX_ROOT:-$SCRIPT_DIR}
OUT=${SATORI_WX_OUT:-$R/build}
VERSION=$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' $R/AndroidManifest.xml)
VERSION=${VERSION:-0.1.0}

echo "== 1. clang probe.so =="
mkdir -p $OUT/zygisk
clang++ -shared -fPIC -std=c++20 -O2 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
  -nostdinc++ -nostdlib++ \
  -I $R/native \
  -o $OUT/zygisk/arm64-v8a.so $R/native/probe.cpp \
  -Wl,--no-undefined -llog
echo "   probe.so: $(ls -la $OUT/zygisk/arm64-v8a.so | awk '{print $5}') bytes"
# -Wl,--no-undefined 保证符号在链接期对上；NEEDED 只许这四个。
NEEDED=$(readelf -d $OUT/zygisk/arm64-v8a.so | grep NEEDED | grep -v 'liblog\|libdl\|libm\|libc\.so' || true)
if [ -n "$NEEDED" ]; then
  echo "   FAIL unexpected dependencies:"; echo "$NEEDED"; exit 1
fi

echo "== 2. 模块目录 =="
MODDIR=$OUT/module
rm -rf $MODDIR && mkdir -p $MODDIR/zygisk
cp $OUT/zygisk/arm64-v8a.so $MODDIR/zygisk/arm64-v8a.so

cat > $MODDIR/module.prop <<EOF
id=satori_wx_probe
name=知言 Probe
version=v$VERSION-probe
versionCode=1
author=araea
description=知言探针：微信 JNI 边界侦察。只读不改行为，90 秒后自动还原。
EOF

# Zygisk Next 只认模块目录里这张表（name= 按进程名匹配）。
cat > $MODDIR/zn_modules.txt <<EOF
name=com.tencent.mm zygisk/arm64-v8a.so
EOF

echo "== 3. zip =="
ZIP=$OUT/satori-wx-probe-v$VERSION.zip
rm -f $ZIP
( cd $MODDIR && zip -qr $ZIP . )
echo "   zip: $ZIP"
echo "done. install: su -c 'cp -r $MODDIR /data/adb/modules/satori_wx_probe' && reboot"
