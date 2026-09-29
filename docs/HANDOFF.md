# 开发者指引

仓库位于 `/data/data/com.termux/files/home/dev/araea/satori-wx`（GitHub 公开仓 `araea/satori-wx`，注意 docs 与测试夹具里有真实 wxid/uin/nick；`gh` 要 `env -u GH_TOKEN`）。先读本文，再按需翻 [`README.md`](../README.md) 与本文列出的其他文档。

## 是什么

微信 `com.tencent.mm`（8.0.78 / versionCode 671108664）的 Zygisk 原生模块，实现 Satori v1 协议，供本机 Koishi / 知微等客户端连接。

纯 native C++：无 DEX、无 Java 助手、无 ArtMethod 改写、无 hook 引擎。连发送都是纯反射调用微信自己的代码，不加载任何额外东西。

当前版本 v0.11.6：自己账号的消息里，号主在微信里手发的那些事件带 `satori_wx.manual_self: true`，模块自己 `message.create` 发出去被读回来的不带（见 [事件](wechat-events.md)）——acumen 靠它区分「号主贴的链接」与「机器人自己的回声」。v0.11.5：`/v1/internal/pat` 发微信「拍一戳」（内部扩展端点，不是 Satori 方法），收到的拍一戳行（appmsg 62）解码为可投递消息。v0.11.4：`message.create` 的图片 `src` 额外接受 `base64://`（社区通用 scheme，无 mime，按魔数定格式）——知微发图片就是这种，此前被当远程 URL 拒掉。v0.11.3：唤醒锁默认开。v0.11.2 曾把常驻通知改成单行收起态，已整体撤回（通知仍是标题加一行、展开正文带在线时长的老样子）；v0.11.3 只改一处——用户唤醒锁默认开，开机后第一个 keeper tick 就持上不定时的 CPU + Wi-Fi 锁，按钮与 `POST /v1/internal/wakelock` 关掉只管本次开机。v0.11.0 之前读侧与事件侧补齐、发送侧有了群内 @ 与图片；v0.11.0 把新消息的到达从 1–2 秒一拍的轮询改成 inotify 驱动（毫秒级），把 wxguard 的解冻从 5 秒轮询改成 cgroup 事件驱动（几毫秒），撤掉发送开关（写方法始终在 features 里；旧配置里的 `send=` 仍被接受但不起作用），并整理了常驻通知（不再重复标题、不再显示唤醒锁与发送状态）；v0.11.1 修了图片发送的确认（空壳行不算发出）。

- 收到的消息按 Satori 元素解码（图片 / 语音 / 视频 / 表情 / 链接 / 文件 / 回复 / @），媒体是签名链接，由 `/v1/proxy` 流式回包，见 [消息内容](wechat-content.md)。
- 事件：`message-created`（带 `guild` `member` 与头像）、`message-deleted`、`guild-member-added|removed`、`guild-added|removed`、`friend-added|removed`，见 [事件](wechat-events.md)。
- `message.list` 是 Satori 的双向分页；事件不再有 4 KiB 上限（128 KiB）；轮询水位不再吞消息。
- `message.create`：群里的 `<at>` 是真提及；`<img>` 走聊天界面自己的 `rj()` 图片管线（真机已验证，见 [发送各类消息](wechat-send-types.md)）；回复 `<quote>` 仍被当元素丢掉。
- 拍一戳：`POST /v1/internal/pat`（`{channel_id, user_id}`）发「拍一戳」，走微信自己的 NetSceneSendPat（`qv3.b`，cgi `sendpat`）；收到的拍一戳行（type 922746929 / appmsg 62）解码成消息投递，作者是发起者，模板里的 `${wxid}` 占位符展开（自己变「你」）。见 `native/wx_pat.cpp` 头注释。
- `upload.create` 收得下 16 MiB，`/v1/proxy` 流式回包、支持 `Range`。
- 发送没有开关：所有会话都可发送，没有限速也没有白名单，模块自己也不给发送加任何延迟。

## 协议覆盖（37 个标准方法）

| | 数量 | 方法 |
| --- | --- | --- |
| 已实现、真机验证 | **21** | 读侧 13：`message.get/list`、`user.get`、`friend.list`、`guild.get/list`、`guild.member.get/list`、`guild.role.list`、`guild.member.role.list`、`channel.get/list`、`user.channel.create`；资源 1：`upload.create`；写侧 6：`message.create`、`message.delete`、`channel.delete`（退群）、`guild.member.kick`、`guild.member.role.set/unset`；`login.get` |
| 微信无此概念 | **11** | `message.update`、`channel.create`、`channel.mute` / `guild.member.mute`、`reaction.create/delete/clear/list`、`guild.role.create/update/delete`，列在 `internal/capabilities.unsupported` |
| 待逆向的写操作 | **5** | `channel.update`（群改名）、`friend.delete`、`friend.approve`、`guild.approve`、`guild.member.approve`，卡点见 [群管理写操作](wechat-room.md) |

读侧 + 资源 + 发送（文本 / @ / 图片）/ 撤回 / 群管理视为完成。`features` 的唯一来源是 `native/wx_capabilities.cpp`（`WeChatFeatures()`）；`wx_backend.cpp` 与 `wx_account.cpp` 都读它，不要各写一份。

## 环境 / 构建 / 部署

- 设备：Android 16 / arm64-v8a，KernelSU，Zygisk Next 1.5.0
- 构建：arm64 Termux，`clang` / `python3` / `readelf` / `patchelf` / `zip`；不需要 JDK / SDK / D8
- 模块目录：`/data/adb/modules/satori_wx`（服务端；密钥捕获已并入主模块）

```sh
cd /data/data/com.termux/files/home/dev/araea/satori-wx
./build.sh          # 服务端 ZIP + satori-wx-check + satori-wx-account + satori-wx-wcdb
./tests/run.sh      # 22 socket + 18 协议 + account + wcdb + store + events + media + message + capabilities + keepalive + content + tempstore + backend + webhook
```

版本号在 `module.prop`（`version` + 独立的 `versionCode` 递增）与 `native/version.h` 两处同步。

### 配置文件 `/data/adb/modules/satori_wx/satori-wx.conf`

```ini
port=5601
token=<32-128 位字母数字-_>
```

未知键、非法值会让 `ReadConfig` 失败，服务端不启动（fail-closed）。发送没有开关也没有实现端限速；`send=on|off` 与 `send_allow=` 是已退役的键，仍被接受（值必须是 on / off）但不起作用，好让旧配置文件继续能启动。`app/src/com/satori/wx/core/Conf.java` 必须与 `ReadConfig` 判得完全一致（`app/test.sh` 的 `ConfTest` 逐条比对）。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| `native/module.cpp` | Zygisk 入口：进程筛选、读配置、注册状态提供者、起服务端 / 账号 / 发送预热线程 |
| `native/server.cpp` | HTTP / WS、鉴权、路由、事件循环、`StatusProvider` 钩子；连接缓冲区按需分配，`/v1/proxy` 按 64 KiB 从文件流式回包（`Range` / `HEAD`），`upload.create` 与 `message.create` 可收 16 MiB（先验令牌） |
| `native/protocol.cpp/.h` | cJSON 校验、37 方法表、EventBus（堆上变长事件，上限 128 KiB）、Hub（登录快照 + 事件回放）、`g_login_count`；发送侧内容拍平（`PlainText` / `OutgoingText` 含 `<at>`、`<a href>`）、`ImageSpans`、base64 |
| `native/webhook.cpp/.h` | 可选 WebHook（独立线程、有界队列、仅 http） |
| `native/wx_account.cpp/.h` | 只读解析微信偏好 → Satori Login（含 features） |
| `native/wx_adapter.cpp/.h` | 每 3 秒扫描身份状态机（added / updated / removed） |
| `native/wcdb.cpp/.h` | `dlopen("libWCDB.so")` + SQLCipher 只读客户端 |
| `native/wx_store.cpp/.h` | 只读 store：message / rcontact / chatroom / MsgQuote / img_flag → Satori JSON；`StorePoll`（水位只前进到已处理的行）、双向 `message.list`、`StoreMediaFile`（媒体文件解析）、变化读取（撤回 / 群名单 / 好友），以及 `roomdata` 的小 protobuf 遍历器（管理员位） |
| `native/wx_message.cpp/.h` | 纯函数：一行 `message` → 作者 + Satori 内容（@、回复、媒体元素、系统行判定），无 I/O |
| `native/xml_lite.cpp/.h`、`native/textbuf.h` | 微信 XML 的轻量扫描器（只认直接子元素、CDATA、实体）与变长文本缓冲；模块不带 C++ 标准库 |
| `native/media.cpp/.h`、`native/wx_media.cpp/.h` | MD5 / SHA-256 / HMAC 与媒体链接签名（密钥由 token 派生）；`/v1/proxy` 的媒体解析器（先验签，再碰库与文件） |
| `native/wx_events.cpp/.h` | 事件扫描器：撤回、群成员、好友的差异 → `message-deleted` / `guild-*` / `friend-*`，启动只快照、两轮确认、重试队列 |
| `native/wx_live.cpp/.h`、`native/wx_watch.cpp/.h` | 读主模块捕获的 key.log，只读打开库；`wx_watch` 用 inotify 盯着库所在目录，微信一写 `EnMicroMsg.db` / `-wal` 就立刻读新消息 → `message-created`（30ms、150ms 各补读一次；1 秒超时兜底；inotify 不可用退回 250ms 轮询），每 3 秒跑一次事件扫描器 |
| `native/wx_backend.cpp/.h` | Backend：13 个读方法 + `message.create`（按 `<img>` 切成有序的文本 / 图片消息，图片先全部解析核对；音视频文件回 400 `media_unsupported`） |
| `native/wx_capabilities.cpp/.h` | 唯一 features 列表 + `unsupported` 列表 |
| `native/wx_send.cpp/.h` | 反射发送器（`SendText` 文本与 @、`SendImage` 图片、`SendRecall` 撤回）+ 状态 / 计数快照（无白名单、无限速）；对外暴露 ReflectEnv / ReflectResolve / ReflectLoad / ReflectDispatchScene 供群管理复用 |
| `native/wx_room.cpp/.h` | 反射群管理写操作：`qn.p`（踢人 / 退群，`m1` 派发）与 `qn.b` / `qn.e`（设 / 撤管理员，`z2.d` Cgi 派发） |
| `native/wx_pat.cpp/.h` | 反射拍一戳：`nv3.l.nj` 插入本地行 + `qv3.b`（cgi `sendpat`）`doScene` 派发；`PatDispatch` 是 `/v1/internal/pat` 的 provider |
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
- 没有开关；不限目标、不限速
- 成功 = 已交给微信派发，不是投递确认；不伪造成功
- 微信 `doScene` 会把库里所有待发（SENDING）消息一起派发，历史遗留的孤儿行会跟着出去

`message.create` 的 `content` 按 `<img>` 切成有序的文本 / 图片消息：

- 文本段先拍平：保留转义文本、`<br/>` 变换行、`<a href>` 在文字后带上目标，丢掉 `<quote>`、`<emoji>` 等元素；群里的 `<at id name/>` 变成 `@昵称` + U+2005 并把 id 交给 `atuserlist`（`<at type="all"/>` 是 `notify@all`），缺 `name` 时用群内昵称补。
- 图片走 `ha0.w.rj()`（聊天界面自己的图片管线）：异步，库里出现 `type=3` 的新行才回 200 并带真实消息 id，6 秒内没有回 502 `image_unconfirmed`（行插入但 content 空壳 = 管线卡死，退化图如 1x1 会这样）。真机已验证。`src` 只认 `upload.create` 的 `internal:` 链接、`data:image/…;base64` 与 `base64://`（社区通用 scheme，知微发的就是这种；无 mime，按魔数定格式），远程 URL 拒绝；所有图片先解析、核对魔数，坏一张整条 400，不发半条。
- 拍平后没有文本也没有图片：只剩音视频文件元素回 400 `media_unsupported`，其余 400。

`tests/content_test.cpp`、`tests/backend_test.cpp` 覆盖拍平、@、链接、`ImageSpans`、base64，以及每一种被拒的请求；真正的 JNI 调用没法在主机上跑，见下面的验收清单。逆向依据与被撤回的转发路径在 [发送各类消息](wechat-send-types.md)。

撤回（`message.delete`）用微信自己的撤回场景，反射调用，不 hook：

```
MsgInfo = ex0.k0.F0.k(talker, localId)          // ex0.j0，按 talker+本地 msgId 取
场景    = new com.tencent.mm.modelsimple.d1(MsgInfo, "你撤回了一条消息", "")
结果    = 场景.doScene(派发器, com.tencent.mm.network.y2)   // cgi /cgi-bin/micromsg-bin/revokemsg
```

只撤回本账号发出的消息（`MsgInfo.z0()==1`）；别人的消息返回 502 + `rejected:true`。群主撤回他人消息要走另一套 ticket，未做。

群管理写操作：`channel.delete`（退群）/ `guild.member.kick` 用 `qn.p`（cgi `delchatroommember`）复用发送路径的 `doScene(派发器, y2)`；`guild.member.role.set/unset` 用 `qn.b` / `qn.e`（cgi `add/delchatroomadmin`），经 `com.tencent.mm.modelbase.z2.d(o, null, false)` 交给微信自带 Cgi 运行器。详情、参数与未做的方法见 [群管理写操作](wechat-room.md)。

拍一戳（v0.11.5，`/v1/internal/pat`）：微信双击头像的流程是两步，都可反射达成——`nv3.l` 单例（服务定位器 `ph5.n0.c(ov3.j.class)`）的 `nj(会话, 发起者, 被拍者, 模板, 时间秒, svrId)` 先插本地互动行（type 922746929，返回 `Pair(msgId, createTime)`，不可拍时返回 (0,0)），再 `new qv3.b(pair, 会话, 被拍者, 0)`（cgi `/cgi-bin/micromsg-bin/sendpat`，构造器自己拼 `uin_msgId_createTime` 指针串）走 `doScene(派发器, y2)`。scene int 0 是普通拍一戳，1 是「改拍一拍后缀」；直派不走中央 runner，所以 849 回调（svrId 回填、失败 Toast）都不会发生，本地记录的 svrId 停在 0，纯展示问题。`nj` 拒绝时 `PatSend` 直接 rejected，不派发。

资源路由：`upload.create` 由 `native/tempstore.cpp` 落盘，返回 `internal:wechat/<user>/_tmp/<name>`（5 分钟）；收到的消息媒体是 `internal:wechat/<user>/_msg/<kind>/<id>/<签名>`（见 [消息内容](wechat-content.md)）。`/v1/proxy/{url}` 在 `server.cpp` 里：`internal:` 解析登录号后流式回文件（`_tmp` 直接查，`_msg` 交给已登记的解析器，先验签）；http(s) 前缀未登记则 403；非法 400；未知登录 / 验签失败 404；带 CORS，不需 Satori 登录头。`proxy_urls` 仍为空（微信没有公网资源 URL）。

## 协议层要点

- 端点：`/v1/meta`、`/v1/meta/webhook.create|delete`、`/v1/internal/status|capabilities`、`/v1/{resource}.{method}`
- 账号类方法要 `Satori-Platform: wechat` + `Satori-User-ID: <wxid>`
- 非 login 方法：不在 features → 404（在参数校验之前判定，缺参也回 404 而非 400）；在 features 但后端没实现 → 501
- `g_login_count`（protocol.cpp）由 `Apply` 更新；`wx_live` 等它 >0 再发事件，避免事件被丢
- store 事件的 `login.sn` 必须等于账号适配器的 sn（当前 = 1）
- `server.cpp` 不知道发送的存在，只用 `StatusProvider` 钩子；测试与独立工具不注册，响应保持精简
- 服务端是单线程事件循环，后端调用在它上面同步执行；图片发送最多阻塞它 6 秒等库里出现行，别在后端里加更长的等待
- 事件是堆上变长字符串，单条上限 `kEventSize` = 128 KiB；WebHook 队列同样按堆分配
- `internal/status` 有 `events` 块（`open`、`emitted`、`skipped`、`dropped`），`internal/capabilities` 有 `event_types`

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

库检查台 `tools/wxq.cpp`（`./build.sh` 不编它，手编：`clang -O2 -I native tools/wxq.cpp native/wcdb.cpp -ldl -o build/wxq`）：只读跑一条 SELECT，参数是 libWCDB.so 路径、库路径、`hex:<密钥>` 或 `-`（明文）。密钥在 `<微信数据目录>/files/satori-wx/key.log` 的 `hex=`；热拷贝 db+wal 的副本读不了，直接以只读方式开活库（密钥确定正确时是安全的）。

## 已知限制 / 安全项

1. 微信被系统反复冻结。真机（ColorOS）实测 `OplusHansManager` 按 uid 冻结微信，被冻期间回环端口握手成功但不回任何东西，客户端挂起至超时。应对：进程内常驻通知 + 唤醒锁 + `startService` 重拉 `CoreService`，加 root 侧 `wxguard` 监听冻结事件、几毫秒内解冻（见 [常驻通知与保活](keepalive.md)）。`wxguard` 全新安装默认 ARMED。
2. 密钥捕获把明文写进 `files/satori-wx/key.log`（0600）；后续可改为只在内存里传给 `wx_live`。
3. 微信 `:push` 子进程有 mars；服务端只在主进程（发送靠反射，不依赖子进程）。
4. 只支持 arm64。
5. 发送没有开关，也不做实现端限速 / 白名单 / 延迟，不伪造成功；风控责任在调用方。
6. 写操作已做 `message.delete`（仅本账号消息）、`channel.delete`（退群）、`guild.member.kick`、`guild.member.role.set/unset`；剩下 5 个（见上）没做，卡点见 [群管理写操作](wechat-room.md)。破坏性动作不伪造成功。
7. 发送：文本、群内 @、图片（真机已验证）。回复 `<quote>`、语音、视频、文件没做；GIF 按静态图发。
8. 收到的图片多半只有缩略图：原图是微信私有的 `wxgf` 容器（客户端打不开），要等用户在微信里点开才下载，模块不触发下载。语音是 SILK（`audio/silk`），不转码。
9. 事件延迟：撤回 ≤ 3 秒，群成员 ≥ 3 秒（要两轮确认），好友 12–24 秒；模块启动前发生的变化不会补发。撤回事件里群消息的作者只有模块启动之后见过的消息才知道。
10. `friend-request` / `guild-request` 等申请事件与对应的 approve 方法没做：这台机器上申请表是空的，没有样本。
11. 服务端事件序号以进程启动时的微秒为基线，不跨进程回放；微信重启后客户端会收到 4009 并重新 IDENTIFY。
12. 一个 `message.create` 里的图片发送是串行的，且占着服务线程等确认；一次最多 4 张。

## 接手第一步

1. `./tests/run.sh` 确认全绿；`git log --oneline -10`
2. 读 `internal/status` 的 `send`、`events`、`keepalive` 块确认线上状态：派发器是否就绪（`send.dispatcher`）、事件是否由变更通知驱动（`events.watching`）、常驻通知是否发布、唤醒锁是否持有、进程 `oom_score_adj` / `wchan`。要量响应速度跑 `python3 tools/latency-probe.py`。要接真实用例直接发即可，没有开关也没有白名单。
3. 想继续写功能：从 5 个待做写操作里挑一个，按发送 / 撤回的老路子做。先只读地找到微信自己的接口（离线 DEX 反查 + 必要时 JADX），再反射调用，最后真机验一条。群改名在可读 dex 里没有 cgi，删好友也没有 `delcontact`（只有 `delcontactlabel`），入群 / 好友审批依赖申请消息里的 ticket。想碰媒体发送先读 [发送各类消息](wechat-send-types.md)。
4. 纪律：每个方法真实实现后才进 `features`；`unsupported` 只放微信真的没有的能力；破坏性动作不伪造成功。往上加 `unsupported` 条目时记得同步 `tests/capabilities_test.cpp` 的计数断言。

## v0.10.0 / v0.11.0 / v0.11.3 真机验收清单（重启手机、微信起来之后按顺序跑一遍）

模块的 `.so` 在开机时被 Zygisk Next 钉成 memfd，覆盖磁盘文件后必须重启手机才会加载新代码（见 `satori-wx-module-deploy` 记忆条目）。主机测试里没有的部分只能在这里验：JNI 调用、真实的库与文件。

```sh
cd /data/data/com.termux/files/home/dev/araea/satori-wx
T=$(su -c 'cat /data/adb/modules/satori_wx/satori-wx.conf' | sed -n 's/^token=//p')
ME=wxid_8zxjsghrk8vz41
api() { local body=${2:-'{}'}; curl -s -X POST "http://127.0.0.1:5601/v1/$1" -H "Authorization: Bearer $T" \
  -H "Satori-Platform: wechat" -H "Satori-User-ID: $ME" -H 'Content-Type: application/json' -d "$body"; echo; }

# 0) v0.11.0：events.watching 必须是 true，然后量延迟——http 几十毫秒、event 也是几十毫秒（旧轮询是 200–1900ms 均匀分布）
api internal/status | head -c 600
python3 tools/latency-probe.py -n 10

# 0b) v0.11.3：keepalive.wakelock 与 wakelock_held / cpu_held / wifi_held 开机后都应当是 true
#     （用户锁默认开，不定时）；通知条目仍是展开样式，正文带在线时长
api internal/status | python3 -c 'import json,sys; print(json.load(sys.stdin)["keepalive"])'

# 1) 版本、事件计数、send.media；capabilities 里要有 event_types
api internal/status | head -c 1500
api internal/capabilities | head -c 1500

# 2) 读侧：最新一页历史（升序、带 prev），再用 prev 往回翻一页
api message.list '{"channel_id":"filehelper","limit":5}'

# 3) 媒体链接：从任意一页历史里找一个 <img src="internal:…"> 或 <audio>，取出来
#    200 + image/jpeg（或 audio/silk）；把签名改一位应当是 404
curl -s -D- -o /dev/null "http://127.0.0.1:5601/v1/proxy/internal:wechat/$ME/_msg/image/<msgId>/<签名>"
curl -s -D- -o /dev/null -H 'Range: bytes=0-99' "http://127.0.0.1:5601/v1/proxy/internal:wechat/$ME/_msg/image/<msgId>/<签名>"   # 206

# 4) 发图（发到 filehelper，只影响自己）：先上传，再用返回的链接发；再试 data URI
curl -s -X POST http://127.0.0.1:5601/v1/upload.create -H "Authorization: Bearer $T" -H "Satori-Platform: wechat" \
  -H "Satori-User-ID: $ME" -F 'file=@/path/to/pic.png'
api message.create '{"channel_id":"filehelper","content":"<img src=\"internal:wechat/'$ME'/_tmp/xxx.png\"/>"}'
#   成功 = 200 + Message[]，id 是库里 type=3 且 content 已带 CDN XML 的那一行；502 image_unconfirmed
#   带「pipeline stalled」= 微信收下但管线卡死（退化图，如 1x1，微信自己发不出去）；带「did not record」
#   = 6 秒内没有入库；502 send_failed 带 detail = 哪个类 / 成员没找到，同时看 internal/status 的
#   send.last_error 与 send.media 计数
api message.create '{"channel_id":"filehelper","content":"先文字<img src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAiElEQVR4nA3JIQ7EIBRFUUxDUkENCJIfzDOEBPExmJqa2f8C7mKGY08IgXiRbsqDZVQZDRchXMSb9FAyVlFjCO8nbuJDypSKNSRGx+eJh5hJldIwoc6Y+DqRiZXUKMI6moyF7xOV2EiidGyixdj4e6IRReqUiS20GS/+nRCxkyZlYRu9jA//8QdJnkXhUZzNAgAAAABJRU5ErkJggg==\"/>再文字"}'
#   期望：三条消息（文字、图、文字），顺序对；坏链接（远程 URL）整条 400、什么都没发出去
#   样图是 8x8 PNG；不要换成 1x1——微信图片管线对退化尺寸会把行卡在 status=5、content 空壳

# 5) 事件（另开一个终端跑着，再做 6、7）
python3 tools/events-tail.py --token "$T"

# 6) 撤回：发一条文字再删掉，events-tail 应先后看到 message-created 与 message-deleted（user 是自己）
api message.create '{"channel_id":"filehelper","content":"撤回测试"}'
api message.delete '{"channel_id":"filehelper","message_id":"<上一步返回的 id>"}'

# 7) 收消息：让别人给你发图片 / 语音 / 引用回复 / 在群里 @ 你，events-tail 里对应看到
#    <img>/<audio>、<quote id=…/>、<at id="wxid_8zxjsghrk8vz41" …/>

# 8) 拍一戳（v0.11.5）：真机验证一条——发到自己文件助手会 rejected（filehelper 不可拍），
#    在真群里拍一个成员应当 200；本机微信聊天页出现「你拍了拍…」，对方收到拍一戳提示。
api internal/pat '{"channel_id":"filehelper","user_id":"wxid_8zxjsghrk8vz41"}'   # 期望 502 rejected:true
api internal/pat '{"channel_id":"<群id>@chatroom","user_id":"<群成员wxid>"}'      # 期望 200 {"ok":true}
```

- 群里的 @ 发送（`<at id=… name=…/>`）需要一个真的群，验之前先问用户要不要在哪个群里试；验证办法是看微信库里那行的 `lvbuffer` 是否带 `<atuserlist>`，以及对方手机上是否高亮。
- 群成员 / 好友事件要等真的有人进群 / 加好友才看得到，不必硬造。
- 重启后 `internal/status` 的 `events.open` 应为 true（key.log 里有 spec 才会开库）；`events.skipped` 与 `events.dropped` 应为 0。
- 如果图片这一项失败：`SendImage` 每一步失败都会写进 `send.last_error`。最可能的原因是类 / 方法名对不上（微信版本不是 8.0.78），其次是 `rj` 需要在主线程调用（那就要 `Handler` 切过去）。

## 管理应用（`app/`）

`app/` 是独立构建的 Android 原生管理界面（`com.satori.wx`，需要 JDK / aapt，与纯 native 的模块构建互不依赖）。它通过 `su` 读写 `satori-wx.conf`、经 `/v1/internal/status` 与 `/v1/meta` 看状态。常驻通知上的唤醒锁按钮是一个显式广播落到应用的 `keepalive.WakeToggleReceiver`，它再把切换转到 `internal/wakelock`。改 `ReadConfig` 的规则时同步改 `app/src/com/satori/wx/core/Conf.java`，`app/test.sh` 的 `ConfTest` 会把同一批样例交给两边比对，不一致就失败。`internal/status` 的 `send` 与 `keepalive` 块字段名被应用读取（`enabled`、计数与 `last_*`；`notification`、`wakelock`、`cpu_held`、`wifi_held`），改名要同步。设计规范见 [知言应用设计规范](app-design.md)。
