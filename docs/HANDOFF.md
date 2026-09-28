# 知言 satori-wx —— 交接文档

> 仓库：`/data/data/com.termux/files/home/dev/araea/satori-wx`（GitHub 私有 `araea/satori-wx`，`gh` 要 `env -u GH_TOKEN`）。
> 这份是唯一入口，读完它再按需翻 `README.md`、`docs/wechat-send.md`、`docs/satori-conformance.md`、
> `docs/wechat-store.md`、`docs/wechat-account.md`。

## 1. 这是什么

微信 `com.tencent.mm`（8.0.78 / versionCode 671108664）的 Zygisk 原生模块，实现 **Satori v1 协议**，
供本机 Koishi / 知微等客户端连接。

**纯 native C++**：无 DEX、无 Java 助手、无 ArtMethod 改写、无 hook 引擎。连发送都是纯反射调用
微信自己的代码，不加载任何额外东西。

- **当前版本 v0.9.2**（v0.9.2：撤回图片发送。真机证明 `v51.r0` 的媒体重载（type 42）只把行写进库，状态停在 5 永不推进；App 发新图的入口是 Kotlin 协程（`qs5.v5.b` → `da0.g` → `kt.d1` flow → `e36.t0.g(new a6(this, flow, dVar))`），收尾必须由调用方给一个 Kotlin 回调 `s0.d`，native 造不出来。`message.create` 对只带媒体的 content 现在明确回 400 `media_unsupported`，不假报成功；证据与全部死路记在 [发送各类消息](wechat-send-types.md)。v0.9.1：修 `internal:` 链接解析——`tempstore.cpp` 里把字符串字面量写成了
  `constexpr const char *`，`sizeof` 拿到的是指针大小，`message.create` 的每张图片都回
  `media_unavailable`；改成数组并补 `tests/tempstore_test.cpp`。v0.9.0：`message.create` 支持 `<img>`——反射 `v51.r0` 的媒体重载发本地图片（type 42 / 动图 66），资源只认自己 `upload.create` 的 `internal:..._tmp/` 链接；读侧 `guild.member.role.list` 认 `chatroom.roomdata` 里的管理员位；`channel.create`/`channel.mute`/`guild.member.mute` 移入 `unsupported`；`wxguard` 全新安装默认 ARMED。v0.8.2：修通知渠道重建；v0.8.1：常驻通知/唤醒锁对齐知弦；v0.8.0：资源路由 + `message.create` 返回数组 + 反射群管理；v0.7.1：修 keepalive JNI 截断崩溃；v0.7.0：取消白名单 + 常驻通知/唤醒锁 + wxguard）。
- 家账号：`wxid_8zxjsghrk8vz41`。模块配置：`send=on`（白名单已取消，任意会话可发）。

## 2. 协议覆盖（37 个标准方法）

| | 数量 | 方法 |
| --- | --- | --- |
| ✅ 已实现、真机验证 | **21** | 读侧 13：`message.get/list`、`user.get`、`friend.list`、`guild.get/list`、`guild.member.get/list`、`guild.role.list`、`guild.member.role.list`、`channel.get/list`、`user.channel.create`；资源 1：`upload.create`；写侧 6（`send=on`）：`message.create`、`message.delete`、`channel.delete`(退群)、`guild.member.kick`、`guild.member.role.set/unset`；`login.get` |
| ❌ 微信无此概念 | **11** | `message.update`（不能编辑已发消息）、`channel.create`（群内没有子频道）、`channel.mute` / `guild.member.mute`（没有服务端禁言）、`reaction.create/delete/clear/list`（没有表态）、`guild.role.create/update/delete`（没有自定义角色）——列在 `internal/capabilities.unsupported` |
| ⬜ 待逆向的写操作 | **5** | `channel.update`(群改名)、`friend.delete`、`friend.approve`、`guild.approve`、`guild.member.approve`——卡点见 [群管理写操作](wechat-room.md) |

读侧 + 资源 + 发送/撤回/群管理可以认为做完了。`features` 的唯一来源是 `native/wx_capabilities.cpp`（`WeChatFeatures()`）；
`wx_backend.cpp` 与 `wx_account.cpp` 都读它，**不要各写一份**。

## 3. 环境 / 构建 / 部署

- 设备：Android 16 / arm64-v8a，KernelSU，Zygisk Next 1.5.0
- 构建：arm64 Termux，`clang` / `python3` / `readelf` / `patchelf` / `zip`；不需要 JDK/SDK/D8
- 模块目录：`/data/adb/modules/satori_wx`（服务端；密钥捕获已并入主模块）

```sh
cd /data/data/com.termux/files/home/dev/araea/satori-wx
./build.sh          # 服务端 ZIP + satori-wx-check + satori-wx-account + satori-wx-wcdb
./tests/run.sh      # 22 socket + 11 协议 + account + wcdb + store + capabilities + keepalive + content + webhook

su -c 'ksud module install build/satori-wx-server-v0.9.1.zip'   # 装机（暂存，重启才生效）
su -c 'setsid sh -c "sleep 60; /system/bin/reboot" </dev/null >/dev/null 2>&1 &'
```

**重启纪律**：构建、装机可以先行；**`reboot` 必须等用户明确确认**（重启会打断会话和用户手头的事）。
装机后没重启 = 还是旧版在跑，`module.prop` 会显示新版但行为不变，别误判。

**版本号**：`module.prop`（`version` + 独立的 `versionCode` 递增）与 `native/version.h` 两处同步。

### 配置文件 `/data/adb/modules/satori_wx/satori-wx.conf`

```ini
port=5601
token=<32-128 位字母数字-_>
send=off            # 默认关闭；on 才进 features 并允许向任意会话发送
# send_allow=...    # 已取消；旧行仍被接受、值被忽略
```

未知键、非法值会让 `ReadConfig` 失败 → 服务端不启动（fail-closed）。`send=on` 后既没有白名单
也没有实现端限速。`app/src/com/satori/wx/core/Conf.java`
必须与 `ReadConfig` 判得完全一致（`app/test.sh` 的 `ConfTest` 逐条比对）。

## 4. 文件职责

| 文件 | 职责 |
| --- | --- |
| `native/module.cpp` | Zygisk 入口：进程筛选、读配置、注册状态提供者、起服务端/账号/发送预热线程 |
| `native/server.cpp` | HTTP/WS、鉴权、路由、事件循环、`StatusProvider` 钩子 |
| `native/protocol.cpp/.h` | cJSON 校验、37 方法表、EventBus、Hub（登录快照 + 事件回放）、`g_login_count` |
| `native/webhook.cpp/.h` | 可选 WebHook（独立线程、有界队列、仅 http） |
| `native/wx_account.cpp/.h` | 只读解析微信偏好 → Satori Login（含 features） |
| `native/wx_adapter.cpp/.h` | 每 3 秒扫描身份状态机（added/updated/removed） |
| `native/wcdb.cpp/.h` | `dlopen("libWCDB.so")` + SQLCipher 只读客户端 |
| `native/wx_store.cpp/.h` | 只读 store：message / rcontact / chatroom → Satori JSON；含 `roomdata` 的小 protobuf 遍历器（管理员位） |
| `native/wx_live.cpp/.h` | 读主模块捕获的 key.log，只读打开库，轮询新消息 → `message-created` |
| `native/wx_backend.cpp/.h` | Backend：13 个读方法 + `message.create`（`send=on` 时，纯文本；媒体元素回 400 `media_unsupported`） |
| `native/wx_capabilities.cpp/.h` | 唯一 features 列表 + `unsupported` 列表 |
| `native/wx_send.cpp/.h` | 反射发送器（`SendText` 文本）+ 状态/计数快照（无白名单、无限速）；并对外暴露 ReflectEnv/ReflectResolve/ReflectLoad/ReflectDispatchScene 供群管理复用 |
| `native/wx_room.cpp/.h` | 反射群管理写操作：`qn.p`（踢人/退群，`m1` 派发）与 `qn.b`/`qn.e`（设/撤管理员，`z2.d` Cgi 派发） |
| `native/tempstore.cpp/.h` | 内置 `upload.create` 的落盘与 TTL；`/v1/proxy` 的 `internal:.../_tmp/...` 目标 |
| `native/wx_keepalive.cpp/.h` | 微信进程内常驻状态通知（状态色 / 在线时长 / 在连客户端数）、唤醒锁（用户开关 + 出站期自动持有 + 客户端在连时保 Wi-Fi）、每 10 分钟重启微信自己的 CoreService；`keepalive` 状态块 |
| `native/wx_key.cpp/.h` | 捕获 SQLCipher 密钥：RegisterNatives 指针替换，只取 setCipherKey/nativeSetKey |
| `tools/wxguard.sh` | root 侧看守（`service.sh` 开机恢复，`action.sh` 切换，`docs/keepalive.md`） |
| `tools/*.py` | 离线 DEX 分析工具（见 §9） |
| `tools/verify-onboot.sh` | 一次性开机自检脚本（见 §8） |

## 5. 微信数据层

### 5.1 库与密钥

- 路径：`/data/data/com.tencent.mm/MicroMsg/<32位哈希>/EnMicroMsg.db`（本机哈希 `aef94886e37998da5c4325f13c5caa62`）
- 密钥：`com.tencent.wcdb.core.Database.setCipherKey (J[BII)V` 的 **7 字节随机 key**，运行期捕获，不可派生
- 参数：`PRAGMA cipher_compatibility = 1`（SQLCipher v1 / page 1024）；打开后 `PRAGMA query_only=1`
- `libWCDB.so` 在 `/data/app/~~*/com.tencent.mm-*/lib/arm64/libWCDB.so`

### 5.2 ⚠️ 血泪教训（必须遵守）

**绝不能用候选密钥打开微信正在用的活库。** 曾致微信主进程 `SIGBUS BUS_ADRERR`（`libWCDB`
内 `__memset_aarch64_nt`）闪退：错误密钥让 SQLite 把库当损坏库，与微信共享 WAL/`-shm`，并发下 mmap 失效。

- 只用**确定的正确密钥**（来自 `setCipherKey` spec）开只读连接
- **离线验证一律在副本上做**：`cp EnMicroMsg.db EnMicroMsg.db-wal EnMicroMsg.db-shm` 到 `/data/local/tmp/`
- 密钥捕获只写 spec，不打开活库；用完删掉副本

### 5.3 已经确认的字段语义

- 群 ID = `<数字>@chatroom`（用户不可见）；`channel.type`：群 0、私聊 1
- 群消息 `content` = `wxid_xxx:\n正文`（发送者前缀需解析）
- `message.type`：`1` 文本、`10000` 系统，其余（3 图片、34 语音…）**跳过不伪造**
- 图片在这个实现端发不出去（见 §6.5b）：不要去写 `type=42` 这类行，真机验证过它停在 `status=5`
- `rcontact.type`：`3`=好友，`1`=系统号，`33`/`gh_%`=公众号；**群用 `username LIKE '%@chatroom'` 判定**
- `chatroom` 表（v0.6.4 起用于群成员/角色）：
  - `memberlist` = `wxid1;wxid2;…`（分号分隔）
  - `displayname` = 群内昵称，用 **U+3001 `、`（UTF-8 `E38081`）** 分隔，与 memberlist **同序**
  - `roomowner` = 群主 wxid；`memberCount` = 人数
  - `roomdata` = 成员 protobuf：`ChatRoomData{ repeated ChatRoomMember member = 1 }`、
    `ChatRoomMember{ string userName = 1; …; int32 flag = 3 }`，**`flag & 2048` = 管理员**
    （v0.9.0 起读侧用它；成员顺序与 memberlist 不一定一致，按 wxid 查）
  - 数量对不上时忽略群昵称、回落到 rcontact（已在 `wx_store.cpp` 处理）
  - ⚠️ 微信**没有**自定义角色，角色只有合成的 `owner`(群主) / `admin`(管理员) / `member`(成员)；
    只有 `admin` 可被 `guild.member.role.set/unset` 变更（v0.8.0），读侧 v0.9.0 起同步

## 6. 发送（已打通）

### 6.1 路径（不需要 hook、不需要 dex、不需要碰 `libapp.so`）

```
派发器 = com.tencent.mm.modelbase.r1.y.k()      ← 主进程的远端派发器
         或 com.tencent.mm.network.a3.c()       ← 兜底；只在 :push 进程有效
场景   = new v51.r0(talker, content, 1, 0, 0, "")   ← 构造器自己把消息写进微信库（SENDING）
结果   = scene.doScene(派发器, new com.tencent.mm.network.y2())   ← 返回 netId，>=0 即已交给微信
```

`v51.r0.f`(J) 是本地消息 id。加密、序号、重发全是微信自己的代码。
高层入口不用找——`com.tencent.mm.network.a3.b(j1,m1)` 的实现就是 `m1.doScene(j1, new y2())`。
完整逆向过程见 `docs/wechat-send.md`。

### 6.2 派发器分进程（v0.6.0 真机踩过）

mars 在 `com.tencent.mm:push`；主进程（服务端所在）拿不到 `a3.c()` 的 `j1`，必须用 `r1.y.k()`。
`wx_send.cpp` 的顺序是 `r1.y.k()` 优先、`a3.c()` 兜底，失败时日志打印解析掩码 `probe=0x..`。

### 6.3 纪律

- **派发器要在构造场景之前拿到**：构造器会写库，拿不到派发器时不该先落一条 SENDING 行
- 默认 `send=off`；开启后不限目标、不限速（旧的 `send_allow` 已取消）
- 成功 = 「已交给微信派发」，**不是投递确认**；不伪造成功
- 微信 `doScene` 会把库里所有待发（SENDING）消息一起派发，所以历史遗留的孤儿行会跟着出去

### 6.4 诊断接口

`internal/status` 与 `internal/capabilities` 都有 `send` 块：

```json
"send": { "enabled":true, "ready":true, "resolved":true, "dispatcher":true,
          "sent":1, "failed":0, "rejected":0, "recalled":0, "media":1,
          "last_age_ms":24, "last_ok":true, "last_target":"filehelper",
          "last_net_id":0, "last_local_id":3292 }
```

- 计数：`sent` 已派发 / `failed` 到了管线但失败 / `rejected` 派发前被策略拒绝；
  `recalled`、`media` 是其中的子计数（都算进 `sent`）
- 语义：`ready` = JavaVM 已接上；`resolved` = 类已解析；`dispatcher` = 上次探测派发器可达
- `resolved`/`dispatcher` 由登录后的 `WarmSend` 线程主动探测（不发消息）维护
  - **v0.6.5 的修**：循环要「解析成功**且**派发器可达」才退出；v0.6.4 只探一次，所以刚开机
    `dispatcher` 会是 false，直到真发一条
- `message.create` 被策略拒绝时 502 体带 `rejected: true`

### 6.5 只收纯文本，进入发送前先拍平 `content`

微信发不了 Satori 的元素，所以 `message.create` 收到 `content` 后先过 `PlainText()`
（`native/protocol.cpp`）：保留转义文本、`<br/>` 变换行，丢掉 `<quote>`、`<at>`、`<emoji>`、
`<img>` 这些只带 id 的元素；`&lt;`/`&gt;`/`&amp;` 等实体还原。拍平后为空就返回 400。

拍平后为空时再用 `ImageSources()`（同文件）分一下类：content 里只有 `<img>` 之类的媒体元素时
回 400 `{"error":"media_unsupported"}`，让客户端能区分「空内容」和「我发不了这张图」，
而不是拿到一个笼统的 400。

> 背景：acumen 的主线改为按协议发完整 `content`，不再为微信单独拍平（见 acumen `refactor(ids)`）。
> 因此这一步落在实现端。`tests/content_test.cpp` 覆盖引号内 `>`、未闭合标签、容量边界，
> 以及 `<img src>` 的抽取（属性顺序、两种引号、空 src、只认 `img`、数量上限）。

### 6.5b 图片为什么没做（v0.9.0 试过，v0.9.2 撤回）

一句话：**发新图要跑 App 的 Kotlin 协程，收尾的回调是 Kotlin 接口，native 造不出来。**
完整的死路记录（含真机证据）在 [发送各类消息](wechat-send-types.md)，接手时先读它，别再重走：

- `v51.r0` 的媒体重载 `type=42`（App 的转发路径 `qs5.v5.fj`/`gj` 用的就是这个）在真机上只把行
  写进库：`type=42, isSend=1, status=5`，`imgPath` 为空，之后**永不推进**；库里 14 条真发出去的
  图片全是 `type=3, status=2`，`content` 是带 CDN 密钥的 `<msg><img aeskey=...>` 全文。
- App 发新图的入口是 `qs5.v5.b(...)`：它造一个 `da0.g` 交给 `kt.d1` 的协程 flow，
  `e36.t0.g(new a6(this, flow, dVar))` 里的 `dVar`（`s0.d`）是**收集者**；传 null 协程根本不跑。
- `v51.q0`/`p0` **不是**上传步，是「需要校验支付密码时重试发送」的流程
  （`modelsimple.l1` = NetSceneVerifyPswd，日志串 `verifypsw ... needVerifyPswList`），别被名字骗了。
- 模块的硬约束是纯 native、不定义 Java 类（`README` 里就是卖点），所以没有合法手段交出这个回调。
  要做图片，先决定是否接受引入一个极小的 Java 助手/DEX——那是另一个量级的改动。

### 6.6 撤回（`message.delete`）

用微信自己的撤回场景，反射调用，不 hook：

```
MsgInfo = ex0.k0.F0.k(talker, localId)          // ex0.j0，按 talker+本地 msgId 取
场景    = new com.tencent.mm.modelsimple.d1(MsgInfo, "你撤回了一条消息", "")
结果    = 场景.doScene(派发器, com.tencent.mm.network.y2)   // cgi /cgi-bin/micromsg-bin/revokemsg
```

- `message.delete` 收 `channel_id` + `message_id`；id 是 `message.create` 回执里的**本地 id**（十进制）。
- 只撤回本账号发出的消息（`MsgInfo.z0()==1`）；别人的消息返回 502 + `rejected:true`。
  群主撤回他人消息要走另一套 ticket，未做。
- 与发送同一套开关；`internal/status.send` 多一个 `recalled` 计数。
- `d1` 构造器对文本类消息会先改本地库（标记已撤回），派发失败也不会回滚——跟微信自己一致。

### 6.7 `message.create` 返回数组（v0.8.0 的协议修正）

官方 `Methods` 里 `createMessage` 返回 `Message[]`，客户端拿到结果会直接 `.map()`。
旧实现返回单个 `Message` 对象，一调就报错；v0.8.0 起 `wx_backend.cpp` 把它包成数组。

### 6.8 群管理写操作（v0.8.0）

`send=on` 时另实现 `channel.delete`(退群)/`guild.member.kick`/`guild.member.role.set/unset`。
微信这个版本有两套请求基类：

- `com.tencent.mm.modelbase.m1`（如 `qn.p`）→ 复用发送的 `doScene(派发器, y2)`；
- `com.tencent.mm.modelbase.i`（如 `qn.b`/`qn.e`，新版 Cgi）→ 构造后取继承字段 `f`（`o` 请求），
  调 `com.tencent.mm.modelbase.z2.d(o, null, false)` 交给 Cgi 运行器。

详情、参数与未做的方法（群改名/禁言/好友审批）见 [群管理写操作](wechat-room.md)。

### 6.9 资源路由（v0.8.0）

- `upload.create` 由 `native/tempstore.cpp` 落盘，返回 `internal:wechat/<user>/_tmp/<name>`（5 分钟）；
- `message.create` 的 `<img src>` 只认这个前缀，落盘位置在微信数据目录下（同 uid 可读）；
- `/v1/proxy/{url}` 在 `server.cpp` 里：`internal:` 解析登录号后回文件；http(s) 前缀未登记则 403；
  非法 400；未知登录 404；带 CORS，不需 Satori 登录头。`proxy_urls` 仍为空（微信没有公网资源 URL）。

## 7. 协议层要点

- 端点：`/v1/meta`、`/v1/meta/webhook.create|delete`、`/v1/internal/status|capabilities`、`/v1/{resource}.{method}`
- 账号类方法要 `Satori-Platform: wechat` + `Satori-User-ID: <wxid>`
- 非 login 方法：**不在 features → 404**；在 features 但后端没实现 → 501
- `g_login_count`（protocol.cpp）由 `Apply` 更新；`wx_live` 等它 >0 再发事件，避免事件被丢
- store 事件的 `login.sn` 必须等于账号适配器的 sn（当前 = 1）
- `server.cpp` 不知道发送的存在，只用 `StatusProvider` 钩子；测试与独立工具不注册，响应保持精简

## 8. 真机验证

### v0.9.0 的验收清单（重启后按顺序跑一遍）

```sh
cd /data/data/com.termux/files/home/dev/araea/satori-wx
T=$(su -c 'cat /data/adb/modules/satori_wx/satori-wx.conf' | sed -n 's/^token=//p')
H=(-H "Authorization: Bearer $T" -H 'Content-Type: application/json')

# 1) 版本与 send/keepalive 块（要看到 version 0.9.2、oom_score_adj）
su -c "curl -s -X POST http://127.0.0.1:5601/v1/internal/status ${H[*]} -d '{}'"

# 2) unsupported 要 11 条，含 channel.create / channel.mute / guild.member.mute
su -c "curl -s -X POST http://127.0.0.1:5601/v1/internal/capabilities ${H[*]} -d '{}'"

# 3) 管理员位：21378418394@chatroom 里 wxid_1i1atvx8t8a021 / wxid_9fpvlcb6z74r22 应是 admin
su -c "curl -s -X POST http://127.0.0.1:5601/v1/guild.member.role.list -H 'Authorization: Bearer $T' \
  -H 'Satori-Platform: wechat' -H 'Satori-User-ID: wxid_8zxjsghrk8vz41' -H 'Content-Type: application/json' \
  -d '{\"guild_id\":\"21378418394@chatroom\",\"user_id\":\"wxid_1i1atvx8t8a021\"}'"

# 4) 只带图片的 content 必须明确被拒（不是笼统 400，也不许假报成功）
su -c "curl -s -X POST http://127.0.0.1:5601/v1/message.create -H 'Authorization: Bearer $T' \
  -H 'Satori-Platform: wechat' -H 'Satori-User-ID: wxid_8zxjsghrk8vz41' -H 'Content-Type: application/json' \
  -d '{\"channel_id\":\"filehelper\",\"content\":\"<img src=\\\"internal:wechat/wxid_8zxjsghrk8vz41/_tmp/x.png\\\"/>\"}'"
# 期望 400 {"error":"media_unsupported"}，且库里不新增任何行。

# 4b) 文本照旧要能发（发到 filehelper，只影响自己）
su -c "curl -s -X POST http://127.0.0.1:5601/v1/message.create -H 'Authorization: Bearer $T' \
  -H 'Satori-Platform: wechat' -H 'Satori-User-ID: wxid_8zxjsghrk8vz41' -H 'Content-Type: application/json' \
  -d '{\"channel_id\":\"filehelper\",\"content\":\"satori-wx v0.9.2 send verify\"}'"
# 期望 200 + Message[]；logcat 有 sent to filehelper；库里 type=1, status=2。

# 5) wxguard：全新安装已默认 ARMED；本机 guard.state 里是 ARMED 才对
su -c 'sh /data/adb/satori-wx/wxguard.sh status --json'
```

被冻住时的现场特征：`uid_*/cgroup.freeze=1`、`/proc/<pid>/wchan` 全是 `do_freezer_trap`、
`internal/status` 请求能连上但 0 字节响应。`wxguard start` 会立刻解冻。

### 常用命令

```sh
# 服务端配置
su -c 'cat /data/adb/modules/satori_wx/satori-wx.conf'

# 状态 / 能力（含 send 块与 unsupported）
T=$(su -c 'cat /data/adb/modules/satori_wx/satori-wx.conf' | sed -n 's/^token=//p')
su -c "curl -s -X POST http://127.0.0.1:5601/v1/internal/status -H 'Authorization: Bearer $T' \
  -H 'Content-Type: application/json' -d '{}'"

# 业务方法（注意 wxid 与 header）
su -c "curl -s -X POST http://127.0.0.1:5601/v1/guild.list -H 'Authorization: Bearer $T' \
  -H 'Satori-Platform: wechat' -H 'Satori-User-ID: wxid_8zxjsghrk8vz41' \
  -H 'Content-Type: application/json' -d '{\"limit\":3}'"

# 日志
su -c 'logcat -d -s SatoriWx:V "*:S" | tail -20'
su -c 'cat /data/data/com.tencent.mm/files/satori-wx-store.log'
su -c 'cat /data/adb/satori-wx-research/postboot.log'
```

文件树版本与模块版本不一致时，可以用 `/data/adb/satori-wx-research/satori-wx-check` 做冒烟。

### 一次性开机自检（可选，用 `pending` 标记 arm）

`service.sh` → `/data/adb/satori-wx-research/verify-onboot.sh`（有 `pending` 才跑）。
它会等开机、拉起微信、等真实登录，然后跑 `satori-wx-check`、抓 logcat，`send=on` 时还自动
向 `SATORI_SEND_TARGET`（默认 `filehelper`）发一条并把结果写进 `postboot.log`（看 `SEND verdict:`
与 `done`/`failed` 标记）。
arm：`su -c ': > /data/adb/satori-wx-research/pending'`。

### 离线查微信 schema（**安全做法**）

```sh
D=/data/data/com.tencent.mm/MicroMsg/aef94886e37998da5c4325f13c5caa62
C=/data/local/tmp/dbcopy; su -c "mkdir -p $C; cp $D/EnMicroMsg.db* $C/; chmod 0644 $C/*"
KEY=$(su -c 'grep "ver=1 " /data/data/com.tencent.mm/files/satori-wx/key.log | sed "s/.*hex=//"' | tr -d ' \n')
LIB=/data/app/~~*/com.tencent.mm-*/lib/arm64/libWCDB.so   # 按实际展开
su -c "SATORI_WCDB_COMPAT=1 ./build/satori-wx-wcdb <展开后的LIB> $C/EnMicroMsg.db hex:$KEY \
  'PRAGMA table_info(chatroom)'"
su -c "rm -rf $C"   # 用完删
```

## 9. 离线 DEX 工具（tools/）

```sh
APK=/data/data/com.termux/files/home/tmp/satori-wx/base.apk    # 微信 base.apk
python3 tools/dexmethodsig.py     $APK 'Lv51/r0;'                        # 某类方法签名
python3 tools/dexfields.py        $APK 'Lcom/tencent/mm/modelbase/r1;'   # 某类字段
python3 tools/dexrefs.py          $APK 'method:Lv51/r0;:<init>'          # 谁引用了它
python3 tools/dexinvokes.py       $APK 'Lcom/tencent/mm/network/a3;' b   # 它调用了谁
python3 tools/dexfindclass.py     $APK 'Lgp0/y;'                          # 谁 new 了它
python3 tools/dexfindstring.py    $APK 'newsendmsg'                       # 谁引用某字符串
python3 tools/dexmethodstrings.py $APK 'Lcom/tencent/mm/app/q3;' b
```

`tools/dexlib.py` 是精确指令解码器。**注意**：`libapp.so` 里的编译化 dex 这些工具看不到，
但经验和教训是——**能用可读 dex 拼出来的路径，优先别去碰它**（发送就是这么找到的）。
JADX 也可用：`~/tools/jadx/bin/jadx --single-class <点分名> -d <输出目录> base.apk`。

## 10. 已知限制 / 安全项

1. 微信被隔一会儿冻一次。真机（ColorOS）实测 `OplusHansManager` 每几秒到几十秒就
   `freeze uid: 10419`，被冻期间回环端口握手成功但**不回任何东西**（客户端是挂住，不是报错）。
   应对：进程内常驻通知 + 唤醒锁 + `startService` 重拉 `CoreService`（v0.7.0），加 root 侧
   `wxguard` 轮询解冻（§4、`docs/keepalive.md`）。**`wxguard` 全新安装默认 ARMED**，
   别改回 PAUSED——那样装完看起来在线、实际不可用。
2. 密钥捕获把明文写进 `files/satori-wx/key.log`（0600）；后续可改为只在内存里传给 `wx_live`
3. 微信 `:push` 子进程有 mars；服务端只在主进程（发送靠反射，不依赖子进程）
4. 只支持 arm64
5. 发送默认关闭，但开启后不做实现端限速/白名单，不伪造成功；风控责任在调用方
6. 写操作已做 `message.delete`（仅本账号消息）、`channel.delete`(退群)、`guild.member.kick`、
   `guild.member.role.set/unset`；剩下 5 个（§2）没做，卡点见 [群管理写操作](wechat-room.md)。
   每个都必须默认关闭；破坏性动作不伪造成功。
7. **发送只有纯文本**。图片/语音/视频/文件都没做，而且图片不是「还没找到入口」，是当前约束下
   走不通：App 发新图要跑 Kotlin 协程，收尾回调是 Kotlin 接口，native 交不出来（§6.5b）。
   `upload.create` 仍然实现（资源路由是标准的一部分），但不会有人去消费它的 `internal:` 链接。
   已确认的类与方法签名、`v51.r1` 的字段语义、以及三条死路都记在
   [发送各类消息](wechat-send-types.md)，接手时先读它，别再从 `v51.r1` 的短字段名猜。

## 11. 下一位接手时的第一步

1. `./tests/run.sh` 确认全绿；`git log --oneline -10`
2. 读 `internal/status` 的 `send` 与 `keepalive` 块确认线上状态：发送是否开启、常驻通知是否发布、
   唤醒锁是否持有、进程 `oom_score_adj` / `wchan`。要接真实用例（自己的测试群）直接发即可，无需白名单。
3. 想继续写功能：从 §2 的 5 个待做写操作里挑一个，按发送/撤回的老路子做——
   **先只读地找到微信自己的接口**（离线 DEX 反查 + 必要时 JADX），再反射调用，
   再默认关闭，最后真机验一条。群改名在可读 dex 里没有 cgi，删好友也没有 `delcontact`
   （只有 `delcontactlabel`，是标签场景），入群/好友审批依赖申请消息里的 ticket。
   想碰媒体发送**先读** `docs/wechat-send-types.md`——图片那条路已经验证过走不通，
   别再花一轮重启去试 type 42/66。
4. 纪律：**每个方法真实实现后才进 `features`**；`unsupported` 只放微信真的没有的能力；
   破坏性/风控敏感动作默认关闭。往上加 `unsupported` 条目时记得同步
   `tests/capabilities_test.cpp` 的计数断言。

## 12. 版本与提交

- `module.prop` / `native/version.h`：当前 **v0.9.2**（v0.9.2 撤回图片发送 + `media_unsupported`；
  v0.9.1 修 internal: 链接解析；v0.9.0 读侧管理员位 + 三个不可表达的方法进 unsupported +
  wxguard 全新安装默认 ARMED + 图片发送（v0.9.2 已废）；v0.8.2 修通知渠道重建；
  v0.8.1 常驻通知/唤醒锁对齐知弦；v0.8.0 资源路由 upload/proxy + message.create 返回数组 +
  反射群管理；v0.7.1 修 keepalive JNI 截断崩溃；v0.7.0 取消白名单 + 常驻通知/唤醒锁 + wxguard）
- 近期：`56cbac5` 预热等派发器 → `f797dcb` 读侧补齐 + unsupported → `1b34615` v0.6.4 读侧 →
  `62228ec` 状态语义+预热 → `0895aac` 状态计数 → `d92ee8f` r1.y.k() 修复 → `8e71852` 发送打通
  → `af751de` 协议资源路由 → `e5c9a1c` 群写操作 → `7c85338` v0.8.1 常驻通知 → `616fd87` v0.8.2 通知渠道

## 13. 管理应用（`app/`）

`app/` 是独立构建的 Android 原生管理界面（`com.satori.wx`，需要 JDK / aapt，与纯 native 的模块构建互不依赖）。
它通过 `su` 读写 `satori-wx.conf`、经 `/v1/internal/status` 与 `/v1/meta` 看状态。白名单已取消，不再
挑会话；常驻通知上的唤醒锁按钮是一个显式广播落到应用的 `keepalive.WakeToggleReceiver`，它再把
切换转到 `internal/wakelock`。**改 `ReadConfig` 的规则时同步改 `app/src/com/satori/wx/core/Conf.java`**
——`app/test.sh` 的 `ConfTest` 会把同一批样例交给两边比对，不一致就失败。`internal/status` 的 `send`
与 `keepalive` 块字段名被应用读取（`enabled`、计数与 `last_*`；`notification`、`wakelock`、`cpu_held`、`wifi_held`），改名要同步。
设计规范见 `docs/app-design.md`。
