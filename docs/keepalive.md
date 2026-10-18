# 常驻通知、唤醒锁与 wxguard

微信被系统冻结或回收时，微信里的知言服务就停了。三层保活：进程内的常驻通知与唤醒锁、进程内重启微信自己的核心服务、root 侧看守 `wxguard`。前两层在微信进程里，第三层在系统里。

## 进程内（`native/wx_keepalive.cpp`）

模块在 `postAppSpecialize` 里起一个守护线程，等 `android.app.ActivityThread.currentApplication()` 返回后，用宿主（微信）上下文做四件事。全部是公开 Android 框架 API 的 JNI 调用：不加载 dex、不定义类、不改 ArtMethod。这也是按钮不能直接在进程内注册接收器的原因。

### 事件驱动，没有轮询

这个线程睡在一个条件变量（`CLOCK_MONOTONIC`）上，只为三件事醒来：

1. **状态变了。** 它展示的每一个值都有明确的来源，来源在改值的那个线程里调 `NotifyStateChanged()`（`protocol.h`）：登录集合变化（`Apply` 处理 `login-*` 事件）、监听端口就绪（`module.cpp`）、WebSocket 客户端连上或断开（`server.cpp`）、发送能力变化（`wx_send.cpp` 的 `SendProbe` 与发送路径）、用户切换唤醒锁。回调只做「记一笔、发信号」，不阻塞。一串变化（登录、端口、客户端几乎同时到）合成一次处理（40 ms 合并，上限 250 ms）。
2. **心跳到期。** 每 10 分钟一次：对微信自己的 `CoreService` 调 `startService()`，并核对通知、通知渠道和通知权限还在不在——这些没有事件可订，所以靠心跳兜底。
3. **自己的最后期限。** 只有三种：宿主 Application 还没出现时的启动等待（0.2 / 0.4 / 0.8 / 1.6 秒，之后每 3 秒，最多约 10 分钟）；登录后发送能力还没就绪时的探测重试（0 / 2.5 / 5 / 10 / 30 / 60 秒，之后交给心跳）；自动持锁的 180 秒安全期限。

一次处理是幂等的，所以生产者只管「戳一下」，不必描述变了什么。空闲时线程一次都不醒：`/v1/internal/status` 的 `keepalive.wakes` 与 `event_wakes` 停着不动（`tests/keepalive_test.cpp` 断言了这一点；真机上连上、断开一个客户端各让它加 1）。

### 通知

- 常驻状态通知：低重要性、静默、`ongoing` 的通道 `satori-wx-status`，状态色与 satori-qq 一致（在线=品牌色 / 等待=琥珀 / 异常=错误红）。标题与正文跟随真实状态：服务在监听且已登录 / 等待登录 / 已登录但端口没监听。折叠时是标题加一行（在连客户端数（WebSocket）与端口）；展开后是这一行，加「上线于 03:41」（不是今天就带日期），以及发送与当前微信版本不兼容时的一句提醒。不附唤醒锁与发送开关的状态：唤醒锁的状态就是按钮上的字（「获取 / 释放唤醒锁」），发送没有开关。点击用微信的 launcher intent 打开微信。
- **在线时长是系统的计时器**：`setWhen(上线时刻)` + `setUsesChronometer(true)`，SystemUI 自己每秒走字，进程里不为它醒。通知只在上面那些内容变化时重发，不再按分钟重绘。
- 通知渠道在每次发布前重建一次：微信或 ColorOS 会把 `satori-wx-status` 这个渠道删掉（`mDeleted=true`），向不存在的渠道发布通知会被系统默默拒收（logcat 里的 `No Channel found`，不抛异常）。
- 被清掉后补发：没有「被取消」的事件可订，所以在心跳时、以及 `/v1/internal/status` 被请求时（最多每 30 秒一次，由 keeper 线程做，不占服务线程）用 `NotificationManager.getActiveNotifications()` 核对，没了就补发。知言应用在前台时每次刷新都会请求状态，所以实际几乎立刻补上；应用没开时最迟一个心跳。

### 发布的状态文件

模块把自己的状态写在微信数据目录里：`files/satori-wx/state.json`（0600，临时文件加改名，原子；内容没变不重写）。只有变得慢的事实，不含逐条消息变化的计数器：

```json
{"schema":1,"version":"0.16.0","pid":11598,"port":5601,"listening":true,
 "login":{"online":1,"id":"wxid_…","name":"昵称"},"clients":1,"serving_since":1792246000000,
 "send":{"state":"incompatible","detail":"send methods not found (version mismatch?)"},
 "keepalive":{"notify":"posted","wakelock":true},"changed_at":1792297261248}
```

用途：微信被系统冻结时回环端口不应答，但这份文件仍在，知言应用（root）读它就知道冻结前服务是什么样——账号是谁、发送能不能用、跑了多久。`pid` 对不上眼下的微信进程就是上一个进程的遗物，读者必须丢掉。应用还用 inotify 盯着它被改名替换，所以变化是推来的。

### 发送能力

`send.state`（`/v1/internal/status` 与上面的文件里都有）：`ready` 能发；`waiting` 找到了微信的发送类、但网络层还没起来；`incompatible` 当前微信版本里没有发送路径反射要用的类或成员（微信是混淆的，换版本会改名；模块只验证过一个版本，见 `wx_send.cpp` 头部）；`unknown` 还没探测、应用没就绪，或找不到类这件事还没被确认第二次。`incompatible` 时通知的展开正文会说，应用的状态卡会说「可以接收，暂不能发送」。状态由 keeper 线程的 `SendProbe()` 探测（登录后按 0 / 2.5 / 5 / 10 / 30 / 60 秒重试直到就绪或判定不兼容；心跳时再看一次；`internal/status` 被请求而发送还没就绪时，最多每 30 秒请求它再探一次），`internal/status` 只读缓存，服务线程上不做 JNI。找不到类或成员要**连着两次**才判不兼容（一次可能只是应用还没完全起来），之后一次成功的探测就清掉它。没有专门的预热线程。

### 唤醒锁

一个 `PARTIAL_WAKE_LOCK`（tag `satori-wx:wakelock`）加一个尽力而为的 `WIFI_MODE_FULL_HIGH_PERF`，都 `setReferenceCounted(false)`。默认开：第一次处理就把不定时的 CPU + Wi-Fi 锁持上。通知上的按钮与 `POST /v1/internal/wakelock` 关掉它只管本次开机。对齐 satori-qq 的 `WakeLockCtl`，还有两路自动持有：

- 出站期间自动持有（`KeepaliveWakelockBegin/End`，引用计数）：`message.create`、`message.delete`、`channel.delete`、`guild.member.kick`、`guild.member.role.set/unset` 在派发期间持有 CPU 锁，避免息屏后微信内核的上传 / 派发被电源管理掐掉。自动持有带 180 秒上限：到点 keeper 醒来放手（用户手动的持有是不定时长的，不受影响）。
- 有客户端连接时保 Wi-Fi：只要有一个 `/v1/events` WebSocket 客户端在连，就保持 Wi-Fi 锁（对应 satori-qq 的 `sustainWifi`）。
- 按钮自身是一个 `PendingIntent.getBroadcast`，指向知言应用导出的 `com.satori.wx.keepalive.WakeToggleReceiver`，Intent 里带当前 `port` / `token` / 目标 `on` 状态。接收器把 `{"on":…}` POST 到 `http://127.0.0.1:<port>/v1/internal/wakelock`，模块在 `ApplyLock()` 里真正持有或释放，随后 keeper 重发通知更新按钮文字。

### 进程内保活

每 10 分钟（心跳）对微信自己的 `com.tencent.mm.booter.CoreService` 调一次 `Context.startService()`。主进程里有「已启动的服务」时 oom_score_adj 停在 SERVICE_ADJ，高于 freezer 阈值。服务全停后才掉到 CACHED，被系统冻结。微信持有 `SYSTEM_ALERT_WINDOW`，后台 `startService` 被放行（记录了 `blocked` 只是记一行）。

### 诊断

`POST /v1/internal/status` 与 `POST /v1/internal/capabilities` 的 `keepalive` 块：

```json
"keepalive": {
  "notification": true,          // 最近一次发布成功
  "notifications_enabled": true, // 系统里微信的通知权限是否打开
  "channel": true,               // 通知渠道（satori-wx-status）是否可用
  "wakelock": false,             // 与 "user" 同值
  "wakelock_held": false,        // OS 实际持有 CPU 或 Wi-Fi
  "user": false,                 // 用户意图（通知按钮）
  "auto": 0,                     // 正在进行的出站自动持有层数
  "cpu_held": false,             // CPU 锁是否真的持有
  "wifi_held": false,            // Wi-Fi 锁是否真的持有
  "sustain_wifi": true,          // 是否因有客户端连接而保 Wi-Fi
  "clients": 1,                  // 在连的 /v1/events 客户端数
  "uptime_ms": 3600000,          // 已登录且在监听的时长
  "service": "ok",               // 最近一次 startService：ok / blocked
  "notify": "posted",            // posted / disabled / build-failed / notify-failed / unavailable
  "reposts": 2,                  // 被清掉后补发次数
  "wakes": 14,                   // keeper 线程处理过多少次（任何原因）
  "event_wakes": 12,             // 其中因为状态变化；差值是心跳与它自己的期限
  "heartbeat_age_ms": 120000,    // 距上一次心跳
  "state_file": "ok",            // 发布的状态文件：ok / error:<errno> / pending / disabled
  "oom_score_adj": 700,
  "wchan": "do_freezer_trap"     // 被冻住时会是它
}
```

`POST /v1/internal/wakelock`（需要 Bearer token）三种用法：`{"on":true}`、`{"on":false}`、`{"toggle":true}`；返回 `{"on":bool,"held":bool}`。没有注册提供者（测试工具、独立构建）时返回 501。

热加载开发版（`tools/dev/dev.sh`）也会起一个独立的 keeper：自己的通知 id、渠道、唤醒锁 tag 与 `state-dev.json`，互不冲突，也不重启 `CoreService`。真机上验证通知逻辑不必重启手机；用完后每个代际留在微信进程里的通知与唤醒锁要手动关（`{"on":false}`、`tools/dev` 控制口的 `cancel`），重启微信才会清掉。

## root 侧（`tools/wxguard.sh`）

### 冻结事件驱动的即时解冻

`OplusHansManager` 隔几秒就按 uid 冻一次微信（logcat 里是 `freeze uid: … scene: |StrictMode-3|LcdOn`）。被冻期间回环端口还能握手，却没人应答，客户端看到的就是「指令发出去很久才有回复」。只扫 cgroup 的话，一次冻结最长要卡 5–10 秒。

watchdog 起来时会拉起一个监听器（`wxguard thaw-watch`，自成会话），用系统自带的 `inotifyd` 盯着 `uid_<uid>/cgroup.events` 与每个 `pid_*/cgroup.events`：cgroup v2 在 `frozen` 变化时产生 inotify 事件（真机实测冻结后 6ms 内到），事件一到就把冻着的 `cgroup.freeze` 写回 0，微信实际停摆只有几毫秒（设备测试里 20–30ms，含测试自己的 10ms 轮询粒度）。监听每 60 秒重列一遍文件（微信重启后会长出新的 `pid_*`）。10 秒内解冻超过 30 次说明系统在反复冻结，每次停 1 秒不空转。日志每 30 秒合并成一行「事件驱动解冻 N 次」。每 5 秒一次的轮询保留作兜底。`tests/wxguard_thaw_test.sh`（root）用一次性 cgroup 覆盖 uid / pid 级冻结、连续冻结、风暴与清理。

### 看守主循环

`wxguard` 与 Zygisk 注入层解耦：即使注入暂时失效，进程死亡仍能被恢复。它由模块自带的 `service.sh` 在开机时复制到 `/data/adb/satori-wx/wxguard.sh` 并调 `boot` 恢复上次状态。状态与日志放在模块目录之外，升级模块不会冲掉 `ARMED` / `PAUSED`。

全新安装默认 ARMED（`WXGUARD_FRESH_MODE`，写进 `guard.conf` 可改成 `PAUSED`）。默认 PAUSED 会让刚装好的模块看起来在线、实际不可用。

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
2. 主进程不在 → 崩溃或被回收；宽限期内继续等、设备没网先不动、`CRASH_WINDOW` 内重启达到 `CRASH_LIMIT` 次转 `PAUSED`；否则在冷却、每小时预算与指数退避允许时拉起。
3. 进程在但被冻（`uid_*/cgroup.freeze=1`、`pid_*/cgroup.freeze=1` 或 `wchan=do_freezer_trap`）→ 写 freezer cgroup 解冻，同一冷却期只写一次。不因为冻结就强杀重启。
4. 进程在、没冻，但 `POST /v1/meta` 连续多轮无响应 → 判定挂死，按预算强拉起。
5. 进程在、服务在，但 `logins` 里没有 `status=1`，且「刚刚还在线过」→ 按预算拉起触发自动登录。

探针用配置文件 `/data/adb/modules/satori_wx/satori-wx.conf` 里的 `port` 与 `token` 查回环上的 `/v1/meta`，登录态是 `logins` 里 `status=1`。系统配置只加 Doze 白名单、`RUN_IN_BACKGROUND` / `RUN_ANY_IN_BACKGROUND` / `WAKE_LOCK` / `START_FOREGROUND` 这几个 AppOps、待机桶与流量白名单；不碰全局 LMK / Doze 开关。

配置可写进 `/data/adb/satori-wx/guard.conf`（脚本会 source，环境变量优先）；变量名以 `WXGUARD_` 开头，见脚本头部注释。`WXGUARD_FRESH_MODE=ARMED|PAUSED` 只决定没有 `guard.state` 时的初始模式。
