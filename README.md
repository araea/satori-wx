# 知言（satori-wx）

微信 `com.tencent.mm` 的 Zygisk 模块。v0.9.0 在 **Satori v1 服务端**
（C++ + POSIX socket，无 DEX、Java 助手、APK、ArtMethod 偏移或 hook 引擎）之上，
加入**只读的微信账号身份 / 消息库适配层**，内置 **`upload.create` 与 `/v1/proxy` 资源路由**，
以及一个**默认关闭的反射写操作集**（文本与图片发送、撤回、群管理）。

账号身份来自微信自己持久化的 SharedPreferences（同 uid 直接读文件，不 hook、不改写、
不访问数据库）。消息读取用微信自己的 `libWCDB.so` 只读打开 `EnMicroMsg.db`；
数据库密钥由模块自身在 `RegisterNatives` 边界捕获（见 `native/wx_key.cpp`），
不再需要单独的探针模块。
**发送与群管理都是可选项**：`send=on` 后，实现端用**宿主 ClassLoader 反射调用微信自己的
`v51.r0`（NetSceneSendMsg）与网络派发器**，由微信完成入库、加密、发送；图片走同一个类的媒体
重载（本地文件路径 + type 42/66）；群管理同样反射微信自己的 NetScene（踢人/退群
`delchatroommember`、设/撤管理员 `add/delchatroomadmin`）。
不发任何原始包、不 hook、不加载 dex。默认 `send=off`；开启后不限目标（旧的 `send_allow`
白名单已取消）。已登录时 `READY` / `/v1/meta` 会带真实 `logins`；账号快照的 `features` 只声明
后端真正实现的方法（见下表）；未实现的方法返回 404，不会伪造成功。
旧 v0.2.x 的 `native/wx.cpp`、`native/jni_helpers.h`、`src/`、`AndroidManifest.xml` 和 `libs/`
保留作研究资料，不参与任何当前构建。版本来源改为根目录 `module.prop`。

## 构建与验证

在 arm64 Termux 中，需要 clang、Python 3、readelf、patchelf、zip；不需要 JDK、Android SDK 或 D8。

```sh
./build.sh               # 服务端 ZIP + 三个诊断工具
./tests/run.sh           # HTTP/WebSocket socket 测试 + 账号解析/事件测试
```

产物：

- `build/satori-wx-server-v0.9.0.zip`，模块 ID `satori_wx`。
- `build/module-server/`，服务端模块目录。
- `build/satori-wx-account`，读取某个微信数据目录并打印推导出的登录事件（诊断用，不联网）。
- `build/satori-wx-wcdb`，只读 SQLCipher/SQLite 客户端，用微信自己的 libWCDB 读导出数据库（诊断用）。

构建检查 AArch64、Zygisk 导出入口、动态依赖白名单及 DEX/旧引导标记。
构建会移除 Termux RUNPATH，运行时不依赖 Termux 库目录。C++ 不链接共享 STL；JSON 解析器为静态编译的 cJSON 1.7.19（MIT，许可证随包附带）。
构建与测试都不安装模块、不重启设备、不启动或操作微信。

## 安装与连接

通过 Magisk / KernelSU 管理器安装 server ZIP，并在需要时重启设备使模块生效。
要求已有可用的 Zygisk 实现。使用 Zygisk Next 时，按其官方要求配置。
安装器首次生成 256 位随机 token，升级保留现有配置：

```text
/data/adb/modules/satori_wx/satori-wx.conf
```

格式（端口可改，token 使用 32–128 位字母、数字、`-` 或 `_`；`send` 默认关闭）：

```ini
port=5601
token=<安装器生成的令牌>
send=off
```

配置权限为 `0600`；缺失、过长、重复字段或无效 token 时服务端不启动。`send=on` 后所有会话
都允许发送，没有限速也没有白名单。旧配置里的 `send_allow=` 行仍被接受、但值被忽略，
升级后不用手改文件。
配置在 `preAppSpecialize` 读取，文件描述符立即关闭；服务在 `postAppSpecialize` 启动。
只在精确匹配的微信主进程内运行。非目标进程和 system_server 请求卸载模块。
服务端只监听 **127.0.0.1**，随微信主进程结束而退出；配置更改在下一次进程启动生效。

也可以用 **知言应用**（[`app/`](app/README.md)）管理：查看连接链路、编辑发送开关、端口与令牌，
并一键重新启动微信让配置生效。应用需要 Root 授权。

Satori 客户端填写：

- API：`http://127.0.0.1:5601/v1`
- Events：`ws://127.0.0.1:5601/v1/events`
- Token：配置文件的 `token` 值

可用接口：

| 接口 | 当前行为 |
| --- | --- |
| `POST /v1/meta` | 已登录时返回只读身份快照 `{"logins":[...],"proxy_urls":[]}`；无账号时 `logins` 为空 |
| `POST /v1/meta/webhook.create` / `webhook.delete` | 注册/注销 WebHook（`url` 必填、`token` 可选）；标准可选功能 |
| `POST /v1/internal/status` | 实验版版本、native 状态、`send` 与 `keepalive`（常驻通知、CPU/Wi-Fi 唤醒锁、自动持有、在连客户端、进程 adj/wchan）状态块；项目自定义诊断接口 |
| `POST /v1/internal/wakelock` | 切换模块在微信进程内持有的 CPU / Wi-Fi 唤醒锁（`{"on":true|false}` 或 `{"toggle":true}`）；常驻通知上的按钮通过知言应用转到这个接口 |
| `POST /v1/login.get` | 返回已登记账号快照；未登录或身份不匹配时返回 403 |
| `POST /v1/{resource}.{method}` | 37 个标准方法的参数校验及 native 后端分发；已实现读侧 `message.get/list`、`user.get`、`friend.list`、`guild.get/list`、`guild.member.get/list`、`guild.role.list`、`guild.member.role.list`、`channel.get/list`、`user.channel.create`、`upload.create`；`send=on` 时另实现 `message.create`（文本 + `<img>`）、`message.delete`、`channel.delete`（退群）、`guild.member.kick`、`guild.member.role.set/unset`（反射，见下与 [群管理](docs/wechat-room.md)）；其余返回 404 |
| `POST /v1/upload.create` | 标准 multipart 上传，落盘到微信数据目录下的 `files/satori-wx-tmp/`，返回 `internal:wechat/<user>/_tmp/<name>`（5 分钟有效），经 `/v1/proxy` 回读，也是 `message.create` 里 `<img src>` 唯一接受的链接 |
| `GET /v1/proxy/{url}` | 标准资源代理：`internal:` 链接按登录号解析并直接回文件；未登记的 http(s) 前缀 403；非法 URL 400 |
| `POST /v1/internal/capabilities` | 除标准方法目录外，报告 `send` 状态块与 `unsupported`（微信无法表达的方法：`message.update`、`channel.create`、`channel.mute`、`guild.member.mute`、`reaction.*`、`guild.role.create/update/delete`） |
| `GET /v1/events` | WebSocket upgrade；10 秒内 IDENTIFY；READY、登录事件与 PING/PONG |

HTTP 使用 `Authorization: Bearer <token>`。缺失 token 返回 401，错误 token 返回 403。
账号 API 要求 `Satori-Platform: wechat` 和 `Satori-User-ID`。
未知接口返回 404，已有 RPC 接口错误方法返回 405。
`login.list` 不属于这里实现的 Satori v1 API；登录集合从 meta / READY 获取。

WebSocket 按 Satori 的 `op=3` + `body.token` 鉴权；成功后 `op=4`，`op=1` 心跳回复 `op=2`。
每 10 秒发送一次 Satori PING；连续 30 秒没有收到则断开。
支持掩码、文本分片、控制帧交错、TCP 分包/合包和关闭握手。
已实现 native 事件生产队列、EVENT/META 和 64 条有界历史；登录事件不参与回放。窗口外序号用 4009 拒绝恢复。
账号适配层每 3 秒只读扫描一次偏好文件，身份出现/变化/消失会广播对应的 `login-added` /
`login-updated` / `login-removed`，并同步 meta 与 READY。
消息等业务事件源尚未实现；广播、状态同步和断线回放已通过独立测试后端与账号端到端测试验证。
401/403 是 HTTP 状态；4000/4004/4009 是本项目的 WebSocket 应用关闭码。

资源限制：8 个并发连接、8 KiB HTTP 头、16 KiB 请求体/WS 消息、有界发送缓冲。
单个 `poll` 线程处理读写、背压和单调时钟超时；空闲时阻塞等待。
HTTP 每次响应后关闭连接；暂不提供 TLS、chunked 请求体、资源代理或真实微信消息事件。

## WebHook（标准可选）

除 WebSocket 外，服务端实现 Satori 的可选 WebHook 推送：应用通过已鉴权的
`POST /v1/meta/webhook.create`（`{"url":"http://...", "token":"..."}`）登记地址，
最多 4 个；每个 EVENT / META 信号都会以 POST 推送到所有登记地址，请求头带
`Satori-Opcode`（EVENT=0，META=5），有 token 时带 `Authorization: Bearer <token>`，
请求体就是信号 body（Event / Meta 对象），与标准一致。`webhook.delete` 按 `url` 注销。
推送在独立线程、有界队列上进行，事件循环永不阻塞；队列满或推送失败只计数不重试。
没有 TLS 客户端，因此只接受 `http://`，`https://` 在登记时返回 400；只有持有服务端 token
的应用才能登记。`/v1/internal/capabilities` 的 `webhook` 为 true，并报告当前登记数。

停用或卸载模块后，结束已有微信进程并按模块管理器要求重启；已有进程中的线程不会因删除模块目录自行退出。

## 常驻通知、唤醒锁与保活（v0.7.0，v0.8.1 与知弦对齐）

模块在微信主进程内提供三项保活能力，全部走 Android 公开框架 API 的 JNI 调用：不加载 dex、
不定义类、不改 ArtMethod。通知与唤醒锁的行为对齐姊妹模块知弦（satori-qq）的 `StatusNotice` /
`WakeLockCtl`。

- **常驻状态通知**：低重要性、静默的常驻条目，状态色同样是「在线 / 等待 / 异常」三档；标题/正文
  跟随真实状态（等待登录 / 运行中 / 服务异常），正文带在连客户端数与在线时长，大文本附唤醒锁与
  发送状态；点击打开微信；微信在前台清掉自家通知后会自动补发。
- **唤醒锁**：通知上的「获取/释放唤醒锁」按钮切换一个 `PARTIAL_WAKE_LOCK` 与一个尽力而为的
  `WIFI_MODE_FULL_HIGH_PERF` 锁，锁由微信进程持有。除了用户开关，模块还在**出站派发期间自动
  持有**（引用计数，带 180 秒上限，卡死的发送不会一直占着 CPU），并在**有客户端连接时保持
  Wi-Fi 锁**——分别对应知弦的 `WakeLockCtl.begin()/end()` 与 `sustainWifi()`。按钮是一个显式
  广播，落到知言应用的导出接收器 `com.satori.wx.keepalive.WakeToggleReceiver`，它再转到回环上的
  `POST /v1/internal/wakelock`（模块纯 native，进程里没有能注册接收器的代码）。
- **进程内保活**：每 10 分钟重新 `startService` 微信自己的 `com.tencent.mm.booter.CoreService`，
  让主进程停在 SERVICE_ADJ 而不是 CACHED，从而不被系统 freezer 冻结。

`/v1/internal/status` 与 `/v1/internal/capabilities` 的 `keepalive` 块报告通知是否发布、
用户意图与 OS 实际持有的 CPU / Wi-Fi 锁、自动持有的层数、保 Wi-Fi 的原因（客户端数）、
已在线时长、上次 `startService` 结果，以及本进程的 `oom_score_adj` 与 `wchan`。

**root 侧看守 `wxguard`**（模块自带，`service.sh` 开机恢复）：让微信在一台已 root 的设备上
尽量不被冻结、不被回收。状态落盘在 `/data/adb/satori-wx/guard.state`，`ARMED` / `PAUSED` 区分
「保活中」与「用户已暂停」；被冻住只写 freezer cgroup 解冻（有冷却），进程死亡才在冷却与
每小时预算内拉起；用户在系统里手动强停会被尊重并转入 PAUSED。命令：

```sh
su -c 'sh /data/adb/satori-wx/wxguard.sh start'    # ARMED
su -c 'sh /data/adb/satori-wx/wxguard.sh stop'     # PAUSED（不关微信）
su -c 'sh /data/adb/satori-wx/wxguard.sh kill'     # PAUSED 并强停微信
su -c 'sh /data/adb/satori-wx/wxguard.sh status --json'
```

KernelSU / Magisk 模块的「操作」按钮 = `wxguard toggle`。`wxguard` 只加 Doze 白名单、调整必要的
AppOps、待机桶与流量白名单，不改全局 LMK / Doze 开关。

**全新安装默认 ARMED**（`WXGUARD_FRESH_MODE=PAUSED` 可写进 `guard.conf` 改回）。2026-09-28 真机
排查的结论：ColorOS 的 `OplusHansManager` 会按 uid 反复冻结/解冻微信，被冻期间回环端口仍然
握手成功、但没有任何响应——客户端是挂住到超时，不是收到错误。旧默认（`fresh-install` → PAUSED）
会让刚装好的模块看起来在线、实际不可用。

## 只读账号身份

适配层在微信主进程内以微信自己的 uid 读取（只读、不解析数据库）:

- `shared_prefs/com.tencent.mm_preferences.xml`：`login_weixin_username`（wxid）、`last_login_uin`、
  `isLogin`、`last_login_alias`、`last_login_nick_name`、`last_login_bind_mobile`、`login_user_name`。
- `shared_prefs/auth_info_key_prefs.xml`：`_auth_uin`，作为 uin 缺失时的回退。
- `files/mmkv/MMKV_Name_LastLoginInfo`（v0.7.0 起）：微信 8.0.78 把上次登录的身份写进这个未加密的 MMKV 文件，
  并可能把上面的偏好文件重写成不含登录键。两处同名键以 MMKV 为准、偏好文件补缺；有效长度取自 `.crc` 元数据
  （旧版取文件头），坏记录即停止解析。MMKV 里没有 `isLogin`，此时以 `_auth_uin` 与账号 uin 一致判定在线。

推导出的 Satori 登录以 wxid 作为 `user.id`，昵称与别名作为 `user.nick` / `user.name`。
`sn` 在账号出现时分配、更新时复用，账号切换会先 `login-removed` 再以新 `sn`
`login-added`，避免把两个身份折叠成一个。

用途与边界：

- `status=1` 仅表示微信持久化的 `isLogin=true`；网络层的 CONNECT / DISCONNECT 未区分，
  崩溃后的短暂过期标记可能造成误报上线。`status=0` 表示已登出。
- 这是**尽力而为的观测**，依赖微信未公开的偏好格式；格式变化时字段会失效或回退为空，
  不会让服务崩溃或阻塞。解析有界（单文件 128 KiB、160 条键值），不做 XML 外部实体展开。
- 不含头像 URL、好友、群、消息等数据；这些需要后续的数据库或网络层适配。
- 诊断：`build/satori-wx-account <数据目录>` 打印推导结果与将发布的登录事件，不写入、不联网。

## 可选消息发送（反射，默认关闭）

`send=on` 时，`message.create` 通过**反射调用微信自己的发送链路**发出消息：
`v51.r0`（NetSceneSendMsg）构造器负责入库，`doScene` 交给微信的 mars 传输层，
加密/序号/重发都是微信自己的行为。实现端不 hook、不改写代码、不加载 dex、不发原始封包。
完整逆向结论见 [微信消息发送路径](docs/wechat-send.md) 与
[发送各类消息](docs/wechat-send-types.md)。

- **文本**：`content` 先拍平（丢弃 `<quote>`/`<at>`/`<emoji>` 这类只带 id 的元素，`<br/>` 变换行）。
- **图片**（v0.9.0）：`content` 里的 `<img src="internal:wechat/<user>/_tmp/<name>"/>` 用
  `v51.r0` 的媒体重载发出，`src` 只接受本模块 `upload.create` 产出的链接（解析成微信 uid
  读得到的本地路径）。文本与图片各成一条消息，一起放在返回的 `Message[]` 里；**先把所有图片
  链接解析成功再发**，坏链接整条请求 400，不会留下半条消息。
- `channel_id` 是 wxid（私聊）或 `<数字>@chatroom`（群）。
- 返回的 `Message.id` 是微信本地消息 id；**成功表示「场景已交给微信派发」，不是投递确认**。
- 没有实现端限速，也没有目标白名单：客户端要求发就立即交给微信派发；解析失败或派发返回
  负值时返回 502 并写明原因。
- **2026-09-27 真机验证通过**（v0.6.1 向「文件传输助手」实发文本成功）。
- 诊断：`POST /v1/internal/status` 与 `/v1/internal/capabilities` 的响应里带 `send` 块
  （`enabled`/`ready`/`resolved`/`dispatcher`、`sent`/`failed`/`rejected`/`recalled`/`media`
  计数、上次尝试的目标/结果/`netId`/本地 id）；`resolved`/`dispatcher` 由登录后的预热线程
  主动探测（不发消息），`message.create` 被策略拒绝时 502 体带 `rejected: true`。
- 发送默认关闭（`send=off`），但开启后不做限速或白名单；`features` 与 `internal/capabilities`
  会如实反映当前是否可用。
- 语音/视频/文件未做，接口位置记在 [发送各类消息](docs/wechat-send-types.md)。

## 资源路由：upload.create 与 /v1/proxy（v0.8.0）

按 Satori 的资源最佳实践，微信没有可直接引用的公网资源 URL，于是回退到 SDK 内置实现：

- `POST /v1/upload.create`（`multipart/form-data`）把文件写入 `<微信数据目录>/files/satori-wx-tmp/`
  （目录 0700、文件 0600、随机名、5 分钟 TTL），返回 `internal:wechat/<user.id>/_tmp/<name>`；
- `GET /v1/proxy/{url}` 按规范代理：`internal:` 链接解析出 platform/user 并直接回文件；
  未登记的 http(s) 前缀返 403（本构建无出站 HTTP 客户端，`proxy_urls` 为空）；非法 URL 返 400、
  未知登录返 404。该路由带 `Access-Control-Allow-Origin: *`，不要求 `Satori-Platform`/`Satori-User-ID`。

受现有 16 KiB 请求体上限约束，上传也限于此；`internal/capabilities` 里的 `proxy`/`upload` 报告能力。

## 可选群管理写操作（反射，v0.8.0，读侧 v0.9.0）

`send=on` 时另外实现三个群写操作，全部反射微信自己的 NetScene、经微信网络队列派发：

- `guild.member.kick` / `channel.delete`（退群）：`qn.p`（cgi `delchatroommember`），
  复用发送路径的 `doScene(派发器, y2)`；
- `guild.member.role.set` / `guild.member.role.unset`：`qn.b` / `qn.e`（cgi `add/delchatroomadmin`），
  经 `com.tencent.mm.modelbase.z2.d(o, null, false)` 交给微信自带 Cgi 运行器；
- `guild.role.list` 合成 `owner` / `admin` / `member`，只有 `admin` 可被设/撤；
- **读侧认管理员位**（v0.9.0）：`guild.member.role.list` 读 `chatroom.roomdata` 里该成员的标志位
  （`flag & 2048`），所以设/撤管理员之后读侧会跟着变；没有 `roomdata` 缓存时按普通成员算。

成功＝已交给微信派发，非服务端已生效；开关关闭时不在 features，客户端得到 404。
逆向记录与未做的方法（群改名、好友删除/审批）见 [群管理写操作](docs/wechat-room.md)。

## 数据库密钥捕获

微信在运行时派生出 `EnMicroMsg.db` 的 SQLCipher 密钥，磁盘上没有；要读消息库只能观察
`com.tencent.wcdb.core.Database.setCipherKey` / `nativeSetKey`。模块在
`env->functions->RegisterNatives` 这个可写数据表上替换这两个 native 的 `fnPtr`，
原调用结果和异常原样保留，不替换微信方法本身、不改代码段。捕获到的 spec 写入
`<应用数据目录>/files/satori-wx/key.log`（0600），`wx_live` 只读打开库并轮询新消息。
观察器常驻，账号切换后新密钥会覆盖写入。这条链路以前是一个独立探针模块，v0.7.0 起并回主模块。

## 参考与下一步

- [ZygiskNext](https://github.com/LSPosed/ZygiskNext)：运行环境与公开接口参考。
- [标准 Zygisk 模块接口](https://github.com/topjohnwu/zygisk-module-sample)：当前沿用本地 API v4 头。
- [Satori HTTP API](https://satori.chat/zh-CN/protocol/api.html)、[事件](https://satori.chat/zh-CN/protocol/events.html)、[元信息](https://satori.chat/zh-CN/advanced/meta.html)。
- [**交接文档（下一位接手先读）**](docs/HANDOFF.md)。
- [知言应用设计规范](docs/app-design.md)。
- [研究记录与已知边界](docs/native-server.md)。
- [只读账号身份说明](docs/wechat-account.md)。
- [消息后端设计（native、低特征）](docs/wechat-store.md)。
- [微信消息发送路径（反射，v0.7.0）](docs/wechat-send.md)。
- [发送各类消息（反射，v0.9.0 图片）](docs/wechat-send-types.md)。
- [微信群管理写操作（反射，v0.9.0）](docs/wechat-room.md)。
- [常驻通知与保活（wxguard）](docs/keepalive.md)。
- [v0.9.0 协议覆盖矩阵](docs/satori-conformance.md)。
- [v0.4.0 安装与重启验收记录](docs/deployment-v0.4.0.md)。

下一步：还没做的写操作（群改名、好友删除与审批、入群审批）在
[docs/wechat-room.md](docs/wechat-room.md) 里逐条写了卡点：群改名与删好友在可读 dex 里没有独立
cgi，审批依赖申请消息里的 ticket。语音/视频/文件的发送接口位置见
[docs/wechat-send-types.md](docs/wechat-send-types.md)。
