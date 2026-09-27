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
    if "$TASK/satori-wx-check" "$MOD/satori-wx.conf" --expect-login > "$TASK/latest-check.log" 2>&1; then
        cat "$TASK/latest-check.log"
        "$TASK/satori-wx-check" "$MOD/satori-wx.conf" --soak --expect-login
        rc=$?
        SEND_EXPECTED=0
        SEND_OK=1
        TARGET=none
        # v0.6.0: exercise the opt-in reflection sender when the config enables it.
        if grep -q '^send=on' "$MOD/satori-wx.conf"; then
            SEND_EXPECTED=1
            SEND_OK=0
            TOKEN=$(sed -n 's/^token=//p' "$MOD/satori-wx.conf")
            # 白名单已取消：验证发送固定发给文件传输助手，可用 SATORI_SEND_TARGET 覆盖。
            TARGET=${SATORI_SEND_TARGET:-filehelper}
            META=$(curl -s -X POST http://127.0.0.1:5601/v1/meta \
                -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d '{}')
            echo "--- meta ---"
            echo "$META"
            WXID=$(echo "$META" | sed -n 's/.*"user":{"id":"\([^"]*\)".*/\1/p')
            echo "--- send verify target=$TARGET wxid=$WXID ---"
            n=0
            while [ "$n" -lt 12 ]; do
                RESP=$(curl -s -X POST http://127.0.0.1:5601/v1/message.create \
                    -H "Authorization: Bearer $TOKEN" -H 'Satori-Platform: wechat' -H "Satori-User-ID: $WXID" \
                    -H 'Content-Type: application/json' \
                    -d "{\"channel_id\":\"$TARGET\",\"content\":\"[satori-wx v0.7.1 send verify]\"}")
                echo "attempt $n: $RESP"
                case "$RESP" in
                    *'"id"'*) SEND_OK=1; break ;;
                    *'dispatcher unavailable'*) sleep 6 ;;
                    *) break ;;
                esac
                n=$((n+1))
            done
            echo "--- readback ---"
            curl -s -X POST http://127.0.0.1:5601/v1/message.list \
                -H "Authorization: Bearer $TOKEN" -H 'Satori-Platform: wechat' -H "Satori-User-ID: $WXID" \
                -H 'Content-Type: application/json' -d "{\"channel_id\":\"$TARGET\",\"limit\":3}"
            echo
        fi
        logcat -d -s SatoriWx:V '*:S' | tail -n 60
        KEYS=/data/data/com.tencent.mm/files/satori-wx
        echo "--- cipher key ---"
        cat "$KEYS/key.log" 2>/dev/null || echo "(none)"
        printf 'SEND verdict: expected=%s ok=%s target=%s\n' "$SEND_EXPECTED" "$SEND_OK" "$TARGET"
        printf 'END exit=%s time=%s boot_id=%s\n' "$rc" "$(date -u +%FT%TZ)" "$(cat /proc/sys/kernel/random/boot_id)"
        if [ "$rc" -eq 0 ] && { [ "$SEND_EXPECTED" -eq 0 ] || [ "$SEND_OK" -eq 1 ]; }; then
            mv "$TASK/running" "$TASK/done"
        else
            mv "$TASK/running" "$TASK/failed"
        fi
        exit "$rc"
    fi
    sleep 4
    i=$((i+1))
done
cat "$TASK/latest-check.log"
logcat -d -s SatoriWx:I '*:S' | tail -n 40
printf 'VERDICT: FAIL no_verified_live_server_or_identity (check device unlock, module load and logcat)\n'
mv "$TASK/running" "$TASK/failed"
exit 1
