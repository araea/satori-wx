#!/system/bin/sh
# 知言的管理入口：模块管理器里的网页界面（webroot/）只通过它读写，每个动作一条子命令。
#
#   ctl.sh probe                  一次快照：模块、配置、开关、微信进程、看守、模块发布的状态与服务的实时应答
#   ctl.sh write-conf <base64>    原子写入 satori-wx.conf（有待生效的模块更新时一并写入）
#   ctl.sh set-enabled 0|1        模块主开关；关闭时暂停看守，重新开启时恢复关闭前的看守
#   ctl.sh guard start|stop       保活看守
#   ctl.sh wakelock on|off        切换模块持有的 CPU / Wi-Fi 唤醒锁
#   ctl.sh open | restart | app-settings    打开微信 / 重新启动微信 / 微信的应用详情
#   ctl.sh update-check | update-install <url> <sha256> | reboot
#   ctl.sh purge                  清掉微信目录里属于模块的东西（卸载模块前用）
#
# 输出是 key=value 行，任意文本走 base64。参数都由界面生成，这里仍逐一校验。
MODDIR=${0%/*}
case $MODDIR in /*) ;; *) MODDIR=$(cd "$MODDIR" && pwd) ;; esac

# Android 11 起每个应用有自己的挂载命名空间，管理器给的 root shell 可能沿用它：那里看不到微信的数据目录。
if [ -z "$SATORI_CTL_NS" ] && [ "$(readlink /proc/self/ns/mnt)" != "$(readlink /proc/1/ns/mnt 2>/dev/null)" ]; then
    export SATORI_CTL_NS=1
    for n in /system/bin/nsenter /data/adb/ksu/bin/busybox /data/adb/ap/bin/busybox /data/adb/magisk/busybox; do
        [ -x "$n" ] || continue
        case $n in */nsenter) exec "$n" -t 1 -m -- /system/bin/sh "$0" "$@" ;; *) exec "$n" nsenter -t 1 -m -- /system/bin/sh "$0" "$@" ;; esac
    done
fi

ID=satori_wx
PKG=com.tencent.mm
M=/data/adb/modules/$ID
U=/data/adb/modules_update/$ID
DATA=/data/user/0/$PKG
# 宿主私有目录里的落盘根。不带模块名：微信随时能看见自己的 files/，名字里写模块名等于
# 把模块写在宿主的目录上。目录由模块进程自己建，这里只补 0700 与属主。
STATE_DIR=$DATA/files/.svc
FLAG=$STATE_DIR/enabled
PUBLISHED=$STATE_DIR/state.json
G=/data/adb/satori-wx
GUARD=$G/wxguard.sh
CONF=$M/satori-wx.conf

b64() { base64 -w 0 2>/dev/null || base64; }
prop() { sed -n "s/^$1=//p" "$2" 2>/dev/null | head -n 1; }
conf_key() { sed -n "s/^$1=//p" "$CONF" 2>/dev/null | head -n 1 | tr -d '\r'; }

guard() { [ -f "$GUARD" ] || GUARD=$MODDIR/wxguard.sh; /system/bin/sh "$GUARD" "$@"; }

# 调服务：POST /v1/<路径>，打印正文，最后一行是 HTTP 状态码；连不上时 curl 的退出码记在 $FAIL。
api() {
    port=$(conf_key port); port=${port:-5601}
    token=$(conf_key token)
    data=$2; [ -n "$data" ] || data='{}'
    curl -s -m 3 -X POST -H "Authorization: Bearer $token" -H 'Content-Type: application/json' \
        -d "$data" -w '\n%{http_code}' "http://127.0.0.1:$port/v1/$1" 2>/dev/null
}

probe() {
    [ -d "$M" ] && echo module=1
    [ -f "$M/disable" ] && echo disabled=1
    [ -f "$M/remove" ] && echo removing=1
    { [ -d "$U" ] || [ -f "$M/update" ]; } && echo update=1
    echo "version=$(prop version "$M/module.prop")"
    echo "version_code=$(prop versionCode "$M/module.prop")"
    [ -f "$U/module.prop" ] && echo "update_version=$(prop version "$U/module.prop")"
    [ -f "$CONF" ] && echo "conf=$(b64 < "$CONF")"
    [ -f "$FLAG" ] && echo "enabled=$(cat "$FLAG")"
    if pm path "$PKG" >/dev/null 2>&1; then
        echo installed=1
        echo "wechat_version=$(dumpsys package "$PKG" | sed -n 's/^ *versionName=//p' | head -n 1)"
    fi
    pid=$(pidof "$PKG" | cut -d ' ' -f 1)
    echo "pid=$pid"
    # ColorOS 的 Hans 等机制会用 cgroup freezer 冻住后台微信：进程还在，服务线程却停着。
    if [ -n "$pid" ]; then
        cg=$(sed -n 's/^0:://p' /proc/$pid/cgroup 2>/dev/null)
        if [ "$(sed -n 's/^frozen //p' /sys/fs/cgroup$cg/cgroup.events 2>/dev/null)" = 1 ] \
            || grep -q freezer /proc/$pid/wchan 2>/dev/null; then echo frozen=1; fi
    fi
    # 微信自己的鉴权记录：非零 uin 表示微信里有已登录的账号（区分「没登录」与「服务没认出」）。
    sed -n 's/.*name="_auth_uin" value="\(-\{0,1\}[0-9]*\)".*/auth=\1/p' \
        "$DATA/shared_prefs/auth_info_key_prefs.xml" 2>/dev/null | head -n 1
    [ -f "$PUBLISHED" ] && echo "published=$(b64 < "$PUBLISHED")"
    sed -n 's/^MODE=/guard=/p' "$G/guard.state" 2>/dev/null | head -n 1
    sed -n 's/^REASON=/guard_reason=/p' "$G/guard.state" 2>/dev/null | head -n 1
    # pid 文件可能是旧的（进程号被别的进程用了）：认命令行里的 thaw-watch。
    w=$(cat "$G/guard.thaw.pid" 2>/dev/null)
    [ -n "$w" ] && grep -q thaw-watch /proc/$w/cmdline 2>/dev/null && echo guard_watch=1
    # 服务的实时应答：冻结时会超时，所以只等 3 秒。
    if [ -n "$pid" ] && [ -f "$CONF" ]; then
        out=$(api internal/status); rc=$?
        code=$(printf '%s' "$out" | tail -n 1)
        echo "http=${code:-0}"
        echo "curl=$rc"
        if [ "$code" = 200 ]; then
            printf '%s' "$out" | sed '$d' | { printf 'status='; b64; echo; }
            meta=$(api meta)
            [ "$(printf '%s' "$meta" | tail -n 1)" = 200 ] && printf '%s' "$meta" | sed '$d' | { printf 'meta='; b64; echo; }
        fi
    fi
    echo "now=$(date +%s)"
}

# 原子写入：先写同目录的临时文件、设好属主 0:0、权限 0600 与 module.prop 的 SELinux 上下文，再改名覆盖。
# 有待生效的模块更新（modules_update）时一并写入，否则重启后更新包里的旧配置会把这次修改盖掉。
write_conf() {
    text=$(printf '%s' "$1" | base64 -d 2>/dev/null) || { echo "error=配置不是有效的 base64"; return 1; }
    port=$(printf '%s\n' "$text" | sed -n 's/^port=//p')
    token=$(printf '%s\n' "$text" | sed -n 's/^token=//p')
    case $port in ''|*[!0-9]*) echo "error=端口无效"; return 1 ;; esac
    [ "$port" -ge 1024 ] && [ "$port" -le 65535 ] || { echo "error=端口须为 1024–65535"; return 1; }
    printf '%s' "$token" | grep -Eq '^[A-Za-z0-9_-]{32,128}$' || { echo "error=令牌须为 32–128 位英文字母、数字、- 或 _"; return 1; }
    umask 077
    wrote=0
    for D in "$M" "$U"; do
        [ -d "$D" ] || continue
        T=$D/.satori-wx.conf.new
        printf '%s\n' "$text" > "$T" || return 1
        chown 0:0 "$T"; chmod 0600 "$T"
        ctx=$(stat -c %C "$D/module.prop" 2>/dev/null)
        [ -n "$ctx" ] && chcon "$ctx" "$T" 2>/dev/null
        mv -f "$T" "$D/satori-wx.conf" || return 1
        wrote=1
    done
    [ "$wrote" = 1 ] || { echo "error=没有找到知言模块"; return 1; }
    echo ok=1
}

# 开关文件在微信的数据目录里，必须归微信 uid 所有并带上同一 SELinux 上下文，否则模块进程读不到。
set_enabled() {
    case $1 in 0|1) ;; *) echo "error=参数须为 0 或 1"; return 2 ;; esac
    [ -d "$DATA" ] || { echo "error=没有找到微信的数据目录"; return 1; }
    was=$(sed -n 's/^MODE=//p' "$G/guard.state" 2>/dev/null)
    dir=${FLAG%/*}
    umask 077
    mkdir -p "$dir"
    owner=$(stat -c %u:%g "$DATA")
    chown "$owner" "$dir"; chmod 0700 "$dir"
    printf '%s' "$1" > "$FLAG.tmp" && chown "$owner" "$FLAG.tmp" && chmod 0600 "$FLAG.tmp"
    ctx=$(stat -c %C "$dir" 2>/dev/null)
    [ -n "$ctx" ] && chcon "$ctx" "$FLAG.tmp" 2>/dev/null
    mv -f "$FLAG.tmp" "$FLAG"
    [ "$(cat "$FLAG")" = "$1" ] || { echo "error=没能写入模块开关"; return 1; }
    # 关闭时把看守一起暂停（否则它还在保活），并记下原来的意图；重新开启时照着恢复。
    if [ "$1" = 0 ]; then
        if [ "$was" = ARMED ]; then echo 1 > "$G/guard.resume"; guard stop >/dev/null 2>&1; else rm -f "$G/guard.resume"; fi
    elif [ -f "$G/guard.resume" ]; then
        guard start >/dev/null 2>&1 && rm -f "$G/guard.resume"
    fi
    echo ok=1
}

wakelock() {
    case $1 in on) v=true ;; off) v=false ;; *) echo "error=参数须为 on 或 off"; return 2 ;; esac
    out=$(api internal/wakelock "{\"on\":$v}")
    [ "$(printf '%s' "$out" | tail -n 1)" = 200 ] || { echo "error=服务没有响应"; return 1; }
    printf '%s' "$out" | sed '$d' | grep -q '"on":true' && echo on=1 || echo on=0
}

open_app() {
    am start --user 0 -n $PKG/.ui.LauncherUI >/dev/null 2>&1 \
        || monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
}

# ---- 更新：读 module.prop 的 updateJson，下载、校验后交给 root 管理器安装（两仓的这一段逐字相同） ----

update_check() {
    url=$(prop updateJson "$M/module.prop")
    [ -n "$url" ] || { echo "error=模块没有声明 updateJson"; return 1; }
    body=$(curl -fsSL -m 20 "$url" 2>/dev/null) || { echo "error=连不上 GitHub"; return 1; }
    printf 'manifest=%s\n' "$(printf '%s' "$body" | b64)"
}

update_install() {
    url=$1 sum=$2
    case $url in https://*) ;; *) echo "error=只接受 https 下载地址"; return 2 ;; esac
    printf '%s' "$sum" | grep -Eq '^[0-9a-f]{64}$' || { echo "error=缺少校验值"; return 2; }
    f=/data/local/tmp/$ID-update.zip
    rm -f "$f"
    curl -fsSL -m 300 -o "$f" "$url" 2>/dev/null || { rm -f "$f"; echo "error=下载失败"; return 1; }
    [ "$(sha256sum "$f" | cut -d ' ' -f 1)" = "$sum" ] || { rm -f "$f"; echo "error=校验失败，安装包与清单不符"; return 1; }
    if [ -x /data/adb/ksud ]; then log=$(/data/adb/ksud module install "$f" 2>&1)
    elif [ -x /data/adb/apd ]; then log=$(/data/adb/apd module install "$f" 2>&1)
    elif command -v magisk >/dev/null 2>&1; then log=$(magisk --install-module "$f" 2>&1)
    else rm -f "$f"; echo "error=没有找到 KernelSU、APatch 或 Magisk"; return 1
    fi
    rc=$?
    rm -f "$f"
    [ "$rc" = 0 ] || { printf 'error=安装器报错：%s\n' "$(printf '%s' "$log" | tail -n 1)"; return 1; }
    echo ok=1
}

# 清掉宿主目录里属于模块的东西：隐藏目录（开关、状态、key、缓存、日志）与旧版本留下的条目。
# 升级不需要它——模块自己会在启动时清旧路径；这里给「卸载模块前」用。
purge() {
    rm -rf "$STATE_DIR"
    for p in "$DATA"/files/satori-wx*; do
        [ -e "$p" ] && rm -rf "$p"
    done
    echo ok=1
}

case $1 in
    probe) probe ;;
    write-conf) write_conf "$2" ;;
    set-enabled) set_enabled "$2" ;;
    guard) case $2 in start|stop) guard "$2" >/dev/null 2>&1 && echo ok=1 ;; *) exit 2 ;; esac ;;
    wakelock) wakelock "$2" ;;
    open) open_app ;;
    # 配置只在微信主进程启动时读取：结束并重新打开是让新设置生效的唯一办法。
    restart) am force-stop $PKG; sleep 1; open_app ;;
    app-settings) am start --user 0 -a android.settings.APPLICATION_DETAILS_SETTINGS -d package:$PKG >/dev/null 2>&1 ;;
    update-check) update_check ;;
    update-install) update_install "$2" "$3" ;;
    reboot) svc power reboot 2>/dev/null || reboot ;;
    purge) purge ;;
    *) echo "usage: ctl.sh probe|write-conf|set-enabled|guard|wakelock|open|restart|app-settings|update-check|update-install|purge|reboot" >&2; exit 2 ;;
esac
