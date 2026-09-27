#!/system/bin/sh
# KernelSU / Magisk module action button: toggle the root-side keep-alive guard (wxguard).
# Turning it off only pauses protection; it never force-stops WeChat. Use `wxguard kill`
# for that.
GUARD=/data/adb/satori-wx/wxguard.sh
[ -x "$GUARD" ] || exit 0
exec /system/bin/sh "$GUARD" toggle
