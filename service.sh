#!/system/bin/sh
# No normal boot daemon. A one-shot acceptance run is armed explicitly by deployment tooling.
TASK=/data/adb/satori-wx-research
if [ -f "$TASK/pending" ] && [ -x "$TASK/verify-onboot.sh" ]; then
    /system/bin/sh "$TASK/verify-onboot.sh" </dev/null >/dev/null 2>&1 &
fi
