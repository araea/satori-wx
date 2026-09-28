# 常驻通知、唤醒锁与 wxguard

微信被系统冻结或回收时，微信里的知言服务就停了。v0.7.0 加了三层保活：进程内的常驻通知与
唤醒锁、进程内重启微信自己的核心服务、以及 root 侧看守 `wxguard`。前两层在微信进程里，
第三层在系统里；它们互相独立，任何一层失效都不影响其余层。

## 进程内（`native/wx_keepalive.cpp`）

模块在 `postAppSpecialize` 里用 `JavaVM` 起一个守护线程，等
`android.app.ActivityThread.currentApplication()` 返回后，用宿主（微信）上下文做三件事。
全部是公开 Android 框架 API 的 JNI 调用：**不加载 dex、不定义类、不改 ArtMethod**，
这也是按钮不能直接在进程内注册接收器的原因。

- **常驻状态通知**：低重要性、静默、`ongoing` 的通道 `satori-wx-status`，状态色与 satori-qq 一致
  （在线=品牌色 / 等待=琥珀 / 异常=错误红）。标题/正文跟随真实状态：服务在监听且已登录 /
  等待登录 / 已登录但端口没监听；正文带**在连客户端数**（WebSocket，与 satori-qq 的 connection
  count 同义）与**在线时长**，大文本再附唤醒锁与发送状态。点击用微信的 launcher intent 打开微信。
  每 3 秒刷新一次，内容没变就只检查条目还在不在
  （`NotificationManager.getActiveNotifications()`），被微信在前台清掉后自动补发。通知渠道在**每次发布前**
  重建一次：微信或 ColorOS 会把 `satori-wx-status` 这个渠道删掉（`mDeleted=true`），而向不存在的渠道
  发布通知会被系统默默拒收（logcat 里的 `No Channel found`，不抛异常），投递会变成每 3 秒重发一次。
- **唤醒锁**：一个 `PARTIAL_WAKE_LOCK`（tag `satori-wx:wakelock`）加一个尽力而为的
  `WIFI_MODE_FULL_HIGH_PERF`，都 `setReferenceCounted(false)`。默认关；通知上的按钮切换它。
  对齐 satori-qq 的 `WakeLockCtl`，还有两路自动持有：
  - **出站期间自动持有**（`KeepaliveWakelockBegin/End`，引用计数）：`message.create`、
    `message.delete`、`channel.delete`、`guild.member.kick`、`guild.member.role.set/unset` 在
    派发期间持有 CPU 锁，避免息屏后微信内核的上传 / 派发被电源管理掐掉；自动持有带 180 秒上限，
    卡死的发送不会一直占着 CPU。用户手动的持有是不定时长的。
  - **有客户端连接时保 Wi-Fi**：只要有一个 `/v1/events` WebSocket 客户端在连，就保持 Wi-Fi 锁，
    避免息屏省电把回环事件挤在队列里（对应 satori-qq 的 `sustainWifi`）。
  按钮自身是一个 `PendingIntent.getBroadcast`，指向知言应用导出的
  `com.satori.wx.keepalive.WakeToggleReceiver`，Intent 里带当前 `port` / `token` / 目标 `on` 状态。
  接收器把 `{"on":…}` POST 到 `http://127.0.0.1:<port>/v1/internal/wakelock`，模块在
  `ApplyLock()` 里真正持有/释放，并在下一次刷新时更新按钮文字。
- **进程内保活**：每 10 分钟对微信自己的 `com.tencent.mm.booter.CoreService` 调一次
  `Context.startService()`。主进程里有「已启动的服务」时 oom_score_adj 停在 SERVICE_ADJ，
  高于 freezer 阈值；服务全停后才掉到 CACHED，被系统冻结。微信持有 `SYSTEM_ALERT_WINDOW`，
  后台 `startService` 被放行（记录了 `blocked` 也就只是记一行，不会崩）。

诊断：`POST /v1/internal/status` 与 `POST /v1/internal/capabilities` 的 `keepalive` 块：

```json
"keepalive": {
  "notification": true,          // 最近一次发布成功
  "notifications_enabled": true, // 系统里微信的通知权限是否打开
  "channel": true,               // 通知渠道（satori-wx-status）是否可用
  "wakelock": false,             // 用户意图（与 "user" 同值，兼容旧版应用）
  "wakelock_held": false,        // OS 实际持有 CPU 或 Wi-Fi（兼容旧版应用）
  "user": false,                 // 用户意图（通知按钮）
  "auto": 0,                     // 正在进行的出站自动持有层数
  "cpu_held": false,             // CPU 锁是否真的持有
  "wifi_held": false,            // Wi-Fi 锁是否真的持有
  "sustain_wifi": true,          // 是否因有客户端连接而保 Wi-Fi
  "clients": 1,                  // 在连的 /v1/events 客户端数
  "uptime_ms": 3600000,          // 已登录且在监听的时长
  "service": "ok",               // 最近一次 startService：ok / blocked
  "notify": "posted",            // posted / disabled / build-failed / notify-failed / no-context
  "reposts": 2,                  // 被清掉后补发次数
  "oom_score_adj": 700,
  "wchan": "do_freezer_trap"     // 被冻住时会是它
}
```

`POST /v1/internal/wakelock`（需要 Bearer token）三种用法：`{"on":true}`、`{"on":false}`、
`{"toggle":true}`；返回 `{"on":bool,"held":bool}`。没有注册提供者（测试工具、独立构建）时返回 501。

## root 侧（`tools/wxguard.sh`）

`wxguard` 与 Zygisk 注入层解耦：即使注入暂时失效，进程死亡仍能被恢复。它由模块自带的
`service.sh` 在开机时复制到 `/data/adb/satori-wx/wxguard.sh` 并调 `boot` 恢复上次状态。
状态与日志放在模块目录之外，升级模块不会冲掉 `ARMED` / `PAUSED`。

**全新安装默认 ARMED**（`WXGUARD_FRESH_MODE`，写进 `guard.conf` 可改成 `PAUSED`）。这条是
2026-09-28 真机排查的结论：ColorOS 的 `OplusHansManager` 会按 uid 反复冻结/解冻微信
（`freeze uid: 10419 ... scene: |StrictMode-3|LcdOn`，间隔几秒到几十秒），被冻期间回环端口
仍然三次握手成功、但没有任何响应。客户端不是收到错误，而是挂住到超时。旧默认
（`fresh-install` → `PAUSED`）会让刚装好的模块看起来在线、实际不可用，所以改成默认保活。

```sh
su -c 'sh /data/adb/satori-wx/wxguard.sh start'      # ARMED：应用系统配置并启动 watchdog
su -c 'sh /data/adb/satori-wx/wxguard.sh stop'       # PAUSED：暂停保护（不关微信）
su -c 'sh /data/adb/satori-wx/wxguard.sh kill'       # PAUSED 并强停微信
su -c 'sh /data/adb/satori-wx/wxguard.sh toggle'     # ARMED / PAUSED 切换（模块「操作」按钮）
su -c 'sh /data/adb/satori-wx/wxguard.sh status --json'
su -c 'sh /data/adb/satori-wx/wxguard.sh apply'      # 只重配系统项
su -c 'sh /data/adb/satori-wx/wxguard.sh once'       # 只跑一轮探针，不动微信
su -c 'sh /data/adb/satori-wx/wxguard.sh log 50'
```

判据与动作（默认每 30 秒一轮）：

1. `stopped=true`（用户手动强停）→ 尊重用户，转 `PAUSED`。
2. 主进程不在 → 崩遗或被回收；宽限期内继续等、设备没网先不动、`CRASH_WINDOW` 内重启达到
   `CRASH_LIMIT` 次转 `PAUSED`；否则在冷却、每小时预算与指数退避允许时拉起。
3. 进程在但被冻（`uid_*/cgroup.freeze=1`、`pid_*/cgroup.freeze=1` 或 `wchan=do_freezer_trap`）→
   写 freezer cgroup 解冻，同一冷却期只写一次；**不因为冻结就强杀重启**。
4. 进程在、没冻，但 `POST /v1/meta` 连续多轮无响应 → 判定挂死，按预算强拉起。
5. 进程在、服务在，但 `logins` 里没有 `status=1`，且「刚刚还在线过」→ 按预算拉起触发自动登录。

探针用配置文件 `/data/adb/modules/satori_wx/satori-wx.conf` 里的 `port` 与 `token` 查
回环上的 `/v1/meta`，登录态是 `logins` 里 `status=1`。系统配置只加 Doze 白名单、
`RUN_IN_BACKGROUND` / `RUN_ANY_IN_BACKGROUND` / `WAKE_LOCK` / `START_FOREGROUND` 这几个 AppOps、
待机桶与流量白名单；不碰全局 LMK / Doze 开关。

配置可写进 `/data/adb/satori-wx/guard.conf`（脚本会 source，环境变量优先）；变量名以
`WXGUARD_` 开头，见脚本头部注释。`WXGUARD_FRESH_MODE=ARMED|PAUSED` 只决定没有
`guard.state` 时的初始模式。
