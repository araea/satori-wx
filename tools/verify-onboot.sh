#!/system/bin/sh
# Installed under /data/adb/satori-wx-research; launched only for an explicitly armed acceptance run.
TASK=/data/adb/satori-wx-research
MOD=/data/adb/modules/satori_wx
LOG="$TASK/postboot.log"
[ -f "$TASK/pending" ] || exit 0
# Rename the marker before doing anything so subsequent boots do not relaunch this test.
mv "$TASK/pending" "$TASK/running" || exit 1
umask 077
exec >> "$LOG" 2>&1
printf 'BEGIN time=%s boot_id=%s\n' "$(date -u +%FT%TZ)" "$(cat /proc/sys/kernel/random/boot_id)"
i=0
while [ "$(getprop sys.boot_completed)" != 1 ] && [ "$i" -lt 120 ]; do sleep 2; i=$((i+1)); done
# App credential storage can remain locked after boot. Try launch/check with a bounded retry window.
i=0
while [ "$i" -lt 90 ]; do
    if [ -z "$(pidof com.tencent.mm)" ] && [ $((i % 5)) -eq 0 ]; then
        am start --user 0 -n com.tencent.mm/.ui.LauncherUI
    fi
    if "$TASK/satori-wx-check" "$MOD/satori-wx.conf" > "$TASK/latest-check.log" 2>&1; then
        cat "$TASK/latest-check.log"
        "$TASK/satori-wx-check" "$MOD/satori-wx.conf" --soak
        rc=$?
        logcat -d -s SatoriWx:I '*:S' | tail -n 40
        printf 'END exit=%s time=%s boot_id=%s\n' "$rc" "$(date -u +%FT%TZ)" "$(cat /proc/sys/kernel/random/boot_id)"
        if [ "$rc" -eq 0 ]; then mv "$TASK/running" "$TASK/done"; else mv "$TASK/running" "$TASK/failed"; fi
        exit "$rc"
    fi
    sleep 4
    i=$((i+1))
done
cat "$TASK/latest-check.log"
logcat -d -s SatoriWx:I '*:S' | tail -n 40
printf 'VERDICT: FAIL no_live_server (check device unlock, module load and logcat)\n'
mv "$TASK/running" "$TASK/failed"
exit 1
