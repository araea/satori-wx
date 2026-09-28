# 开发者指引

仓库位于 `/data/data/com.termux/files/home/dev/araea/satori-wx`（GitHub 私有 `araea/satori-wx`，`gh` 要 `env -u GH_TOKEN`）。先读本文，再按需翻 [`README.md`](../README.md) 与本文列出的其他文档。

## 是什么

微信 `com.tencent.mm`（8.0.78 / versionCode 671108664）的 Zygisk 原生模块，实现 Satori v1 协议，供本机 Koishi / 知微等客户端连接。

纯 native C++：无 DEX、无 Java 助手、无 ArtMethod 改写、无 hook 引擎。连发送都是纯反射调用微信自己的代码，不加载任何额外东西。

当前版本 v0.9.3：方法可用性先于参数校验判定——不在 features 的方法一律回 404 `unsupported_api`，不管参数缺不缺、对不对，不再因为参数不合法先漏出 400 `invalid_request`（v0.9.2 及以前，`reaction.list` 之类不支持的方法在缺参时会误报 400）。`message.create` 对只带媒体的 content 明确回 400 `media_unsupported`，不假报成功。模块默认 `send=off`；开启后所有会话都可发送，没有限速也没有白名单。

## 协议覆盖（37 个标准方法）

| | 数量 | 方法 |
| --- | --- | --- |
| 已实现、真机验证 | **21** | 读侧 13：`message.get/list`、`user.get`、`friend.list`、`guild.get/list`、`guild.member.get/list`、`guild.role.list`、`guild.member.role.list`、`channel.get/list`、`user.channel.create`；资源 1：`upload.create`；写侧 6（`send=on`）：`message.create`、`message.delete`、`channel.delete`（退群）、`guild.member.kick`、`guild.member.role.set/unset`；`login.get` |
| 微信无此概念 | **11** | `message.update`、`channel.create`、`channel.mute` / `guild.member.mute`、`reaction.create/delete/clear/list`、`guild.role.create/update/delete`，列在 `internal/capabilities.unsupported` |
| 待逆向的写操作 | **5** | `channel.update`（群改名）、`friend.delete`、`friend.approve`、`guild.approve`、`guild.member.approve`，卡点见 [群管理写操作](wechat-room.md) |

读侧 + 资源 + 发送 / 撤回 / 群管理视为完成。`features` 的唯一来源是 `native/wx_capabilities.cpp`（`WeChatFeatures()`）；`wx_backend.cpp` 与 `wx_account.cpp` 都读它，不要各写一份。

## 环境 / 构建 / 部署

- 设备：Android 16 / arm64-v8a，KernelSU，Zygisk Next 1.5.0
- 构建：arm64 Termux，`clang` / `python3` / `readelf` / `patchelf` / `zip`；不需要 JDK / SDK / D8
- 模块目录：`/data/adb/modules/satori_wx`（服务端；密钥捕获已并入主模块）

```sh
cd /data/data/com.termux/files/home/dev/araea/satori-wx
./build.sh          # 服务端 ZIP + satori-wx-check + satori-wx-account + satori-wx-wcdb
./tests/run.sh      # 22 socket + 11 协议 + account + wcdb + store + capabilities + keepalive + content + tempstore + backend + webhook
```

版本号在 `module.prop`（`version` + 独立的 `versionCode` 递增）与 `native/version.h` 两处同步。

### 配置文件 `/data/adb/modules/satori_wx/satori-wx.conf`

```ini
port=5601
token=<32-128 位字母数字-_>
send=off            # 默认关闭；on 才进 features 并允许向任意会话发送
```

未知键、非法值会让 `ReadConfig` 失败，服务端不启动（fail-closed）。`send=on` 后既没有白名单也没有实现端限速。`app/src/com/satori/wx/core/Conf.java` 必须与 `ReadConfig` 判得完全一致（`app/test.sh` 的 `ConfTest` 逐条比对）。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| `native/module.cpp` | Zygisk 入口：进程筛选、读配置、注册状态提供者、起服务端 / 账号 / 发送预热线程 |
| `native/server.cpp` | HTTP / WS、鉴权、路由、事件循环、`StatusProvider` 钩子 |
| `native/protocol.cpp/.h` | cJSON 校验、37 方法表、EventBus、Hub（登录快照 + 事件回放）、`g_login_count` |
| `native/webhook.cpp/.h` | 可选 WebHook（独立线程、有界队列、仅 http） |
| `native/wx_account.cpp/.h` | 只读解析微信偏好 → Satori Login（含 features） |
| `native/wx_adapter.cpp/.h` | 每 3 秒扫描身份状态机（added / updated / removed） |
| `native/wcdb.cpp/.h` | `dlopen("libWCDB.so")` + SQLCipher 只读客户端 |
| `native/wx_store.cpp/.h` | 只读 store：message / rcontact / chatroom → Satori JSON；含 `roomdata` 的小 protobuf 遍历器（管理员位） |
| `native/wx_live.cpp/.h` | 读主模块捕获的 key.log，只读打开库，轮询新消息 → `message-created` |
| `native/wx_backend.cpp/.h` | Backend：13 个读方法 + `message.create`（`send=on` 时，纯文本；媒体元素回 400 `media_unsupported`） |
| `native/wx_capabilities.cpp/.h` | 唯一 features 列表 + `unsupported` 列表 |
| `native/wx_send.cpp/.h` | 反射发送器（`SendText` 文本）+ 状态 / 计数快照（无白名单、无限速）；对外暴露 ReflectEnv / ReflectResolve / ReflectLoad / ReflectDispatchScene 供群管理复用 |
| `native/wx_room.cpp/.h` | 反射群管理写操作：`qn.p`（踢人 / 退群，`m1` 派发）与 `qn.b` / `qn.e`（设 / 撤管理员，`z2.d` Cgi 派发） |
| `native/tempstore.cpp/.h` | 内置 `upload.create` 的落盘与 TTL；`/v1/proxy` 的 `internal:.../_tmp/...` 目标 |
| `native/wx_keepalive.cpp/.h` | 微信进程内常驻状态通知、唤醒锁、每 10 分钟重启微信自己的 CoreService；`keepalive` 状态块 |
| `native/wx_key.cpp/.h` | 捕获 SQLCipher 密钥：RegisterNatives 指针替换，只取 setCipherKey / nativeSetKey |
| `tools/wxguard.sh` | root 侧看守（`service.sh` 开机恢复，`action.sh` 切换，见 [常驻通知与保活](keepalive.md)） |
| `tools/*.py` | 离线 DEX 分析工具 |
| `tools/verify-onboot.sh` | 一次性开机自检脚本 |

## 微信数据层

### 库与密钥

- 路径：`/data/data/com.tencent.mm/MicroMsg/<32位哈希>/EnMicroMsg.db`
- 密钥：`com.tencent.wcdb.core.Database.setCipherKey (J[BII)V` 的 7 字节随机 key，运行期捕获，不可派生
- 参数：`PRAGMA cipher_compatibility = 1`（SQLCipher v1 / page 1024）；打开后 `PRAGMA query_only=1`
- `libWCDB.so` 在 `/data/app/~~*/com.tencent.mm-*/lib/arm64/libWCDB.so`

### 安全约束

**绝不能用候选密钥打开微信正在用的活库。** 曾致微信主进程 `SIGBUS BUS_ADRERR`（`libWCDB` 内 `__memset_aarch64_nt`）闪退：错误密钥让 SQLite 把库当损坏库，与微信共享 WAL / `-shm`，并发下 mmap 失效。

- 只用确定的正确密钥（来自 `setCipherKey` spec）开只读连接
- 离线验证一律在副本上做：`cp EnMicroMsg.db EnMicroMsg.db-wal EnMicroMsg.db-shm` 到 `/data/local/tmp/`
- 密钥捕获只写 spec，不打开活库；用完删掉副本

### 已确认的字段语义

- 群 ID = `<数字>@chatroom`（用户不可见）；`channel.type`：群 0、私聊 1
- 群消息 `content` = `wxid_xxx:\n正文`（发送者前缀需解析）
- `message.type`：`1` 文本、`10000` 系统，其余（3 图片、34 语音）跳过不伪造
- `rcontact.type`：`3`=好友，`1`=系统号，`33` / `gh_%`=公众号；群用 `username LIKE '%@chatroom'` 判定
- `chatroom` 表（v0.6.4 起用于群成员 / 角色）：
  - `memberlist` = `wxid1;wxid2;…`（分号分隔）
  - `displayname` = 群内昵称，用 U+3001 `、`（UTF-8 `E38081`）分隔，与 memberlist 同序
  - `roomowner` = 群主 wxid；`memberCount` = 人数
  - `roomdata` = 成员 protobuf：`ChatRoomData{ repeated ChatRoomMember member = 1 }`、`ChatRoomMember{ string userName = 1; …; int32 flag = 3 }`，`flag & 2048` = 管理员（v0.9.0 起读侧用它；成员顺序与 memberlist 不一定一致，按 wxid 查）
  - 数量对不上时忽略群昵称、回落到 rcontact（已在 `wx_store.cpp` 处理）
  - 微信没有自定义角色，角色只有合成的 `owner`（群主）/ `admin`（管理员）/ `member`（成员）；只有 `admin` 可被 `guild.member.role.set/unset` 变更（v0.8.0），读侧 v0.9.0 起同步

## 发送（已打通）

路径（不需要 hook、不需要 dex、不需要碰 `libapp.so`）：

```
派发器 = com.tencent.mm.modelbase.r1.y.k()      ← 主进程的远端派发器
         或 com.tencent.mm.network.a3.c()       ← 兜底；只在 :push 进程有效
场景   = new v51.r0(talker, content, 1, 0, 0, "")   ← 构造器自己把消息写进微信库（SENDING）
结果   = scene.doScene(派发器, new com.tencent.mm.network.y2())   ← 返回 netId，>=0 即已交给微信
```

`v51.r0.f`(J) 是本地消息 id。加密、序号、重发全是微信自己的代码。高层入口不用找，`com.tencent.mm.network.a3.b(j1,m1)` 的实现就是 `m1.doScene(j1, new y2())`。完整逆向过程见 [微信消息发送路径](wechat-send.md)。

- 派发器要在构造场景之前拿到：构造器会写库，拿不到派发器时不该先落一条 SENDING 行
- 默认 `send=off`；开启后不限目标、不限速
- 成功 = 已交给微信派发，不是投递确认；不伪造成功
- 微信 `doScene` 会把库里所有待发（SENDING）消息一起派发，历史遗留的孤儿行会跟着出去

只收纯文本，进入发送前先拍平 `content`：保留转义文本、`<br/>` 变换行，丢掉 `<quote>`、`<at>`、`<emoji>`、`<img>`；拍平后为空时，若 content 里只有 `<img>` 之类媒体元素，返回 400 `{"error":"media_unsupported"}`。`tests/content_test.cpp` 覆盖引号内 `>`、未闭合标签、容量边界，以及 `<img src>` 的抽取（属性顺序、两种引号、空 src、只认 `img`、数量上限）。

图片发不出去的原因为 App 发新图要跑 Kotlin 协程，收尾回调是 Kotlin 接口，native 造不出来。完整的死路记录（含真机证据）在 [发送各类消息](wechat-send-types.md)。

撤回（`message.delete`）用微信自己的撤回场景，反射调用，不 hook：

```
MsgInfo = ex0.k0.F0.k(talker, localId)          // ex0.j0，按 talker+本地 msgId 取
场景    = new com.tencent.mm.modelsimple.d1(MsgInfo, "你撤回了一条消息", "")
结果    = 场景.doScene(派发器, com.tencent.mm.network.y2)   // cgi /cgi-bin/micromsg-bin/revokemsg
```

只撤回本账号发出的消息（`MsgInfo.z0()==1`）；别人的消息返回 502 + `rejected:true`。群主撤回他人消息要走另一套 ticket，未做。

群管理写操作（`send=on`）：`channel.delete`（退群）/ `guild.member.kick` 用 `qn.p`（cgi `delchatroommember`）复用发送路径的 `doScene(派发器, y2)`；`guild.member.role.set/unset` 用 `qn.b` / `qn.e`（cgi `add/delchatroomadmin`），经 `com.tencent.mm.modelbase.z2.d(o, null, false)` 交给微信自带 Cgi 运行器。详情、参数与未做的方法见 [群管理写操作](wechat-room.md)。

资源路由：`upload.create` 由 `native/tempstore.cpp` 落盘，返回 `internal:wechat/<user>/_tmp/<name>`（5 分钟）；`/v1/proxy/{url}` 在 `server.cpp` 里：`internal:` 解析登录号后回文件；http(s) 前缀未登记则 403；非法 400；未知登录 404；带 CORS，不需 Satori 登录头。`proxy_urls` 仍为空（微信没有公网资源 URL）。

## 协议层要点

- 端点：`/v1/meta`、`/v1/meta/webhook.create|delete`、`/v1/internal/status|capabilities`、`/v1/{resource}.{method}`
- 账号类方法要 `Satori-Platform: wechat` + `Satori-User-ID: <wxid>`
- 非 login 方法：不在 features → 404（在参数校验之前判定，缺参也回 404 而非 400）；在 features 但后端没实现 → 501
- `g_login_count`（protocol.cpp）由 `Apply` 更新；`wx_live` 等它 >0 再发事件，避免事件被丢
- store 事件的 `login.sn` 必须等于账号适配器的 sn（当前 = 1）
- `server.cpp` 不知道发送的存在，只用 `StatusProvider` 钩子；测试与独立工具不注册，响应保持精简

## 离线 DEX 工具（tools/）

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

`tools/dexlib.py` 是精确指令解码器。注意：`libapp.so` 里的编译化 dex 这些工具看不到，能用可读 dex 拼出来的路径优先别去碰它（发送就是这么找到的）。JADX 也可用：`~/tools/jadx/bin/jadx --single-class <点分名> -d <输出目录> base.apk`。

## 已知限制 / 安全项

1. 微信被系统反复冻结。真机（ColorOS）实测 `OplusHansManager` 按 uid 冻结微信，被冻期间回环端口握手成功但不回任何东西，客户端挂起至超时。应对：进程内常驻通知 + 唤醒锁 + `startService` 重拉 `CoreService`，加 root 侧 `wxguard` 轮询解冻（见 [常驻通知与保活](keepalive.md)）。`wxguard` 全新安装默认 ARMED。
2. 密钥捕获把明文写进 `files/satori-wx/key.log`（0600）；后续可改为只在内存里传给 `wx_live`。
3. 微信 `:push` 子进程有 mars；服务端只在主进程（发送靠反射，不依赖子进程）。
4. 只支持 arm64。
5. 发送默认关闭，但开启后不做实现端限速 / 白名单，不伪造成功；风控责任在调用方。
6. 写操作已做 `message.delete`（仅本账号消息）、`channel.delete`（退群）、`guild.member.kick`、`guild.member.role.set/unset`；剩下 5 个（见上）没做，卡点见 [群管理写操作](wechat-room.md)。每个都必须默认关闭；破坏性动作不伪造成功。
7. 发送只有纯文本。图片 / 语音 / 视频 / 文件都没做，且图片是当前约束下走不通：App 发新图要跑 Kotlin 协程，收尾回调是 Kotlin 接口，native 交不出来。已确认的类与方法签名、`v51.r1` 的字段语义、以及三条死路都记在 [发送各类消息](wechat-send-types.md)。

## 接手第一步

1. `./tests/run.sh` 确认全绿；`git log --oneline -10`
2. 读 `internal/status` 的 `send` 与 `keepalive` 块确认线上状态：发送是否开启、常驻通知是否发布、唤醒锁是否持有、进程 `oom_score_adj` / `wchan`。要接真实用例直接发即可，无需白名单。
3. 想继续写功能：从 5 个待做写操作里挑一个，按发送 / 撤回的老路子做。先只读地找到微信自己的接口（离线 DEX 反查 + 必要时 JADX），再反射调用，再默认关闭，最后真机验一条。群改名在可读 dex 里没有 cgi，删好友也没有 `delcontact`（只有 `delcontactlabel`），入群 / 好友审批依赖申请消息里的 ticket。想碰媒体发送先读 [发送各类消息](wechat-send-types.md)。
4. 纪律：每个方法真实实现后才进 `features`；`unsupported` 只放微信真的没有的能力；破坏性 / 风控敏感动作默认关闭。往上加 `unsupported` 条目时记得同步 `tests/capabilities_test.cpp` 的计数断言。

## 管理应用（`app/`）

`app/` 是独立构建的 Android 原生管理界面（`com.satori.wx`，需要 JDK / aapt，与纯 native 的模块构建互不依赖）。它通过 `su` 读写 `satori-wx.conf`、经 `/v1/internal/status` 与 `/v1/meta` 看状态。常驻通知上的唤醒锁按钮是一个显式广播落到应用的 `keepalive.WakeToggleReceiver`，它再把切换转到 `internal/wakelock`。改 `ReadConfig` 的规则时同步改 `app/src/com/satori/wx/core/Conf.java`，`app/test.sh` 的 `ConfTest` 会把同一批样例交给两边比对，不一致就失败。`internal/status` 的 `send` 与 `keepalive` 块字段名被应用读取（`enabled`、计数与 `last_*`；`notification`、`wakelock`、`cpu_held`、`wifi_held`），改名要同步。设计规范见 [知言应用设计规范](app-design.md)。
