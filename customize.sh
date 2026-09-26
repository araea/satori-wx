#!/system/bin/sh
# Sourced by the Magisk/KernelSU module installer.
SKIPUNZIP=0
[ "$ARCH" = arm64 ] || abort "此实验包仅支持 arm64"
ui_print "知言：安装 native Satori 服务端"
old_config=/data/adb/modules/satori_wx/satori-wx.conf
if [ -f "$old_config" ]; then
    cp "$old_config" "$MODPATH/satori-wx.conf" || abort "无法保留配置"
else
    token=$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')
    [ "${#token}" -eq 64 ] || abort "无法生成访问令牌"
    (umask 077; printf 'port=5601\ntoken=%s\n' "$token" > "$MODPATH/satori-wx.conf") || abort "无法写入配置"
fi
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/satori-wx.conf" 0 0 0600
ui_print "监听 127.0.0.1:5601；令牌保存在模块目录 satori-wx.conf"
