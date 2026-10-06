#!/system/bin/sh
# Root-only device test for wxguard's event-driven thaw. It builds a throwaway cgroup tree under
# /sys/fs/cgroup/apps, freezes a sleeper in it the way ColorOS does (uid level and pid level), and
# checks that the watcher thaws it within milliseconds, not on the 5-second poll. WeChat itself is
# never touched. Run as root: su -c 'sh tests/wxguard_thaw_test.sh'. Exits 0 (with SKIP) when the
# device has no cgroup v2 freezer to test against.
set -u
export PATH=/system/bin:/system/xbin:${PATH:-}
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
GUARD=$HERE/../tools/wxguard.sh
BASE=/sys/fs/cgroup/apps
[ "$(id -u)" = "0" ] && [ -w "$BASE/cgroup.procs" ] || { echo "wxguard thaw test: SKIP (needs root and cgroup v2)"; exit 0; }

ROOT=$BASE/zz_wxguard_test_$$
UID_N=$(stat -c %u /data/data/com.termux 2>/dev/null)
[ -n "$UID_N" ] || { echo "wxguard thaw test: SKIP (no stand-in package)"; exit 0; }
TMP=$(mktemp -d /data/local/tmp/wxguard-test.XXXXXX) || exit 1
mkdir "$ROOT" "$ROOT/uid_$UID_N" "$ROOT/uid_$UID_N/pid_1" 2>/dev/null || { echo "wxguard thaw test: SKIP (cannot create cgroups)"; rm -rf "$TMP"; exit 0; }
failures=0
sleeper=""
cleanup() {
    [ -n "$sleeper" ] && kill -9 "$sleeper" 2>/dev/null
    stop_thaw_watch 2>/dev/null
    sleep 0.3
    rmdir "$ROOT/uid_$UID_N/pid_1" "$ROOT/uid_$UID_N" "$ROOT" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

export WXGUARD_DIR=$TMP WXGUARD_PKG=com.termux WXGUARD_CGROUP_APPS=$ROOT
# Not exported: the watcher is a child `sh wxguard.sh thaw-watch` and must run its main().
WXGUARD_LIB_ONLY=1
. "$GUARD"
unset WXGUARD_LIB_ONLY
# Sourced from here, $0 is this script: point SELF back at the guard or start_thaw_watch would
# re-run the test instead of the watcher.
SELF=$GUARD
LOG=$TMP/guard.log

check() { if [ "$1" = "0" ]; then echo "ok   - $2"; else echo "FAIL - $2"; failures=$((failures + 1)); fi; }
ms() { local up; read -r up _ < /proc/uptime; echo "${up%.*}${up#*.}0"; }   # 10ms resolution
# Waits until `file` reads 0, at most 4s; prints the elapsed ms (or 9999).
wait_thawed() {
    local file=$1 t0 v
    t0=$(ms)
    while [ $(( $(ms) - t0 )) -lt 4000 ]; do
        read -r v < "$file"; [ "$v" = "0" ] && { echo $(( $(ms) - t0 )); return; }
        /system/bin/sleep 0.01
    done
    echo 9999
}

# A process living in the stand-in uid cgroup, so a freeze has something to freeze.
sh -c "echo \$\$ > $ROOT/uid_$UID_N/cgroup.procs; exec /system/bin/sleep 300" &
sleeper=$!
/system/bin/sleep 0.3

start_thaw_watch $$
/system/bin/sleep 1.5
thaw_watch_running; check $? "the watcher is running"
[ -n "$(pgrep -f "inotifyd - $ROOT")" ]; check $? "it holds an inotifyd on the cgroup events files"

echo 1 > "$ROOT/uid_$UID_N/cgroup.freeze"
took=$(wait_thawed "$ROOT/uid_$UID_N/cgroup.freeze")
echo "     uid-level freeze thawed after ${took}ms"
[ "$took" -lt 500 ]; check $? "a uid-level freeze is thawed within 500ms (poll would take up to 5000ms)"

echo 1 > "$ROOT/uid_$UID_N/pid_1/cgroup.freeze"
took=$(wait_thawed "$ROOT/uid_$UID_N/pid_1/cgroup.freeze")
echo "     pid-level freeze thawed after ${took}ms"
[ "$took" -lt 500 ]; check $? "a pid-level freeze is thawed within 500ms"

for i in 1 2 3 4 5; do
    echo 1 > "$ROOT/uid_$UID_N/cgroup.freeze"
    took=$(wait_thawed "$ROOT/uid_$UID_N/cgroup.freeze")
    [ "$took" -lt 500 ] || break
done
[ "$took" -lt 500 ]; check $? "five freezes in a row are all thawed"

# Refreezing the moment it thaws (a fight with the OS) must not spin: still alive, still answering.
i=0; while [ $i -lt 40 ]; do echo 1 > "$ROOT/uid_$UID_N/cgroup.freeze"; /system/bin/sleep 0.02; i=$((i + 1)); done
took=$(wait_thawed "$ROOT/uid_$UID_N/cgroup.freeze")
[ "$took" -lt 2000 ]; check $? "a rapid refreeze storm is still thawed (took ${took}ms)"
thaw_watch_running; check $? "the watcher survives the storm"

stop_thaw_watch
/system/bin/sleep 0.5
thaw_watch_running; [ $? -ne 0 ]; check $? "stop_thaw_watch stops the watcher"
[ -z "$(pgrep -f "inotifyd - $ROOT")" ]; check $? "and leaves no inotifyd behind"

if [ "$failures" -gt 0 ]; then echo "wxguard thaw tests: $failures FAILED"; exit 1; fi
echo "wxguard thaw tests: PASS"
