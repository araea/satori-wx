#!/system/bin/sh
# 卸载模块时：先停根侧看守，再把微信目录里属于模块的东西清掉（files/.svc 与旧条目）。
# 卸载之后模块就没有机会自己清了，这是最后一次。
GUARD=/data/adb/satori-wx/wxguard.sh
[ -x "$GUARD" ] && /system/bin/sh "$GUARD" stop
MODDIR=${0%/*}
case $MODDIR in /*) ;; *) MODDIR=$(cd "$MODDIR" && pwd) ;; esac
[ -f "$MODDIR/ctl.sh" ] && /system/bin/sh "$MODDIR/ctl.sh" purge >/dev/null 2>&1
exit 0
