#!/system/bin/sh
# Boot hook. Restores the root-side keep-alive guard (wxguard) and, when deployment tooling
# has armed it, runs the one-shot acceptance check.
MODDIR=${0%/*}
GUARD=/data/adb/satori-wx/wxguard.sh
mkdir -p /data/adb/satori-wx 2>/dev/null
cp -f "$MODDIR/wxguard.sh" "$GUARD" 2>/dev/null
chmod 0755 "$GUARD" 2>/dev/null
[ -x "$GUARD" ] && /system/bin/sh "$GUARD" boot </dev/null >/dev/null 2>&1 &

TASK=/data/adb/satori-wx-research
if [ -f "$TASK/pending" ] && [ -x "$TASK/verify-onboot.sh" ]; then
    /system/bin/sh "$TASK/verify-onboot.sh" </dev/null >/dev/null 2>&1 &
fi
