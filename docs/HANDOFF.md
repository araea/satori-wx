# 开发者指引

微信 `com.tencent.mm`（8.0.78 / versionCode 671108664）的 Zygisk 原生模块，实现 Satori v1 协议，供本机 Koishi / 知微等客户端连接。用法与接口见 [`README.md`](../README.md)。文档与测试夹具里有真实 wxid / uin，`gh` 命令要加 `env -u GH_TOKEN`。

## 环境与构建

- 设备：Android 16 / arm64-v8a，KernelSU，Zygisk Next 1.5.0
- 构建：arm64 Termux，`clang` / `python3` / `readelf` / `patchelf` / `zip`，不需要 JDK / SDK / D8
- 模块目录：`/data/adb/modules/satori_wx`

```sh
./build.sh          # 服务端 ZIP + satori-wx-check + satori-wx-account + satori-wx-wcdb
./tests/run.sh      # socket + 协议 + account + wcdb + store + events + media + message
                   # + capabilities + keepalive + content + tempstore + backend + webhook
```

版本号在 `module.prop`（`version` + 独立的 `versionCode` 递增）与 `native/version.h` 两处同步。

## 配置

`/data/adb/modules/satori_wx/satori-wx.conf`：

```ini
port=5601
token=<32-128 位字母数字-_>
```

未知键、非法值会让 `ReadConfig` 失败，服务端不启动（fail-closed）。发送没有开关也没有实现端限速。`app/src/com/satori/wx/core/Conf.java` 必须与 `ReadConfig` 判得完全一致，`app/test.sh` 的 `ConfTest` 逐条比对。

## 协议覆盖（37 个标准方法）

| | 数量 | 方法 |
| --- | --- | --- |
| 已实现 | **21** | 读侧 13、资源 1（`upload.create`）、写侧 6、`login.get`，清单见 [README](README.md#接口) |
| 微信无此概念 | **11** | `message.update`、`channel.create`、`channel.mute` / `guild.member.mute`、`reaction.create/delete/clear/list`、`guild.role.create/update/delete`，列在 `internal/capabilities.unsupported` |
| 待逆向的写操作 | **5** | `channel.update`（群改名）、`friend.delete`、`friend.approve`、`guild.approve`、`guild.member.approve`，卡点见[群管理写操作](wechat-room.md) |

`features` 的唯一来源是 `native/wx_capabilities.cpp`（`WeChatFeatures()`）。`wx_backend.cpp` 与 `wx_account.cpp` 都读它，不要各写一份。每个方法真实实现后才进 `features`。`unsupported` 只放微信真的没有的能力，往上加条目时同步 `tests/capabilities_test.cpp` 的计数断言。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| `native/module.cpp` | Zygisk 入口：进程筛选、读配置、注册状态提供者、起服务端 / 账号 / 发送预热线程 |
| `native/server.cpp` | HTTP / WS、鉴权、路由、事件循环、`StatusProvider` 钩子；`/v1/proxy` 按 64 KiB 从文件流式回包（`Range` / `HEAD`），`message.create` 收 16 MiB，`upload.create` 流式收到 1 GiB（都先验令牌） |
| `native/protocol.cpp/.h` | cJSON 校验、37 方法表、EventBus（堆上变长事件，上限 128 KiB）、Hub（登录快照 + 事件回放）、`g_login_count`、发送侧内容拍平（`PlainText` / `OutgoingText` 含 `<at>`、`<a href>`）、`ImageSpans`、base64 |
| `native/webhook.cpp/.h` | 可选 WebHook（独立线程、有界队列、仅 http） |
| `native/wx_account.cpp/.h` | 只读解析微信偏好 → Satori Login（含 features） |
| `native/wx_adapter.cpp/.h` | 每 3 秒扫描身份状态机（added / updated / removed） |
| `native/wcdb.cpp/.h` | `dlopen("libWCDB.so")` + SQLCipher 只读客户端 |
| `native/wx_store.cpp/.h` | 只读 store：message / rcontact / chatroom / MsgQuote / img_flag → Satori JSON；`StorePoll`（水位只前进到已处理的行）、双向 `message.list`、`StoreMediaFile`、变化读取（撤回 / 群名单 / 好友）、`roomdata` 的小 protobuf 遍历器（管理员位） |
| `native/wx_message.cpp/.h` | 纯函数：一行 `message` → 作者 + Satori 内容（@、回复、媒体元素、系统行判定），无 I/O |
| `native/xml_lite.cpp/.h`、`native/textbuf.h` | 微信 XML 的轻量扫描器（只认直接子元素、CDATA、实体）与变长文本缓冲；模块不带 C++ 标准库 |
| `native/media.cpp/.h`、`native/wx_media.cpp/.h` | MD5 / SHA-256 / HMAC 与媒体链接签名（密钥由 token 派生）；`/v1/proxy` 的媒体解析器（先验签，再碰库与文件） |
| `native/wx_events.cpp/.h` | 事件扫描器：撤回、群成员、好友的差异 → `message-deleted` / `guild-*` / `friend-*`，启动只快照、两轮确认、重试队列 |
| `native/wx_live.cpp/.h`、`native/wx_watch.cpp/.h` | 读主模块捕获的 key.log，只读打开库；`wx_watch` 用 inotify 盯库所在目录（30ms、150ms 各补读一次，1 秒超时兜底，不可用退回 250ms 轮询），每 3 秒跑一次事件扫描器 |
| `native/wx_backend.cpp/.h` | Backend：13 个读方法 + `message.create`（按媒体元素切成有序的文本 / 图片 / 视频 / 文件消息，全部先解析核对再发，`<quote>` 挂在第一段文字上，视频与文件发完等库里的 status 离开「发送中」） |
| `native/wx_send_media.cpp` | 反射发送 `SendFile`（`k0.I`）、`SendVideo`（`ab5.s.cj`）、`SendQuote`（`dx0.r` type 57）；每次调用现查类与成员，缺一个只让这一次请求失败并在 detail 里点名 |
| `native/wx_forward.cpp/.h` | 合并转发卡片（`<message forward>` → `type=49` / appmsg 19）的 XML 构造与限额校验 |
| `native/upload_stream.cpp/.h`、`native/multipart.cpp` | `upload.create` 的流式 multipart 解析（滑动窗口找分隔符），逐个 part 写进 `tempstore` 的 `TempWriter` |
| `native/mp4_probe.cpp/.h` | 读 ISO-BMFF：有无视频轨、时长、画面尺寸，`moov` 在 `mdat` 后面也找得到；判「视频气泡还是文件」并给微信秒数 |
| `native/wx_voice.cpp/.h`、`native/audio_pcm.cpp/.h` | 语音条：`VoicePrepare` 把 `<audio>` 的文件变成微信 SILK（已是 SILK 原样、WAV 自读、其它 MediaCodec，重采样到 16 kHz，微信自己的 SILK 编码器）；`audio_pcm` 是纯函数（WAV、重采样、SILK 容器检查），host 可测 |
| `native/wx_quote.h` | `QuoteRef`：引用回复要的被引用消息信息（库里读出，发送器拼 `<refermsg>`） |
| `native/wx_capabilities.cpp/.h` | 唯一 features 列表 + `unsupported` 列表 |
| `native/wx_send.cpp/.h` | 反射发送器（`SendText` 文本与 @、`SendImage` 图片、`SendRecall` 撤回）+ 状态与计数快照；对外暴露 ReflectEnv / ReflectResolve / ReflectLoad / ReflectDispatchScene 供群管理复用 |
| `native/wx_room.cpp/.h` | 反射群管理写操作：`qn.p`（踢人 / 退群，`m1` 派发）与 `qn.b` / `qn.e`（设 / 撤管理员，`z2.d` Cgi 派发） |
| `native/wx_pat.cpp/.h` | 反射拍一戳：`nv3.l.nj` 插入本地行 + `qv3.b`（cgi `sendpat`）`doScene` 派发；`PatDispatch` 是 `/v1/internal/pat` 的 provider |
| `native/tempstore.cpp/.h` | 内置 `upload.create` 的落盘与 TTL；`/v1/proxy` 的 `internal:.../_tmp/...` 目标 |
| `native/wx_keepalive.cpp/.h` | 微信进程内常驻状态通知、唤醒锁、每 10 分钟重启微信自己的 CoreService；`keepalive` 状态块 |
| `native/wx_key.cpp/.h` | 捕获 SQLCipher 密钥：RegisterNatives 指针替换，只取 setCipherKey / nativeSetKey |
| `tools/wxguard.sh` | root 侧看守（`service.sh` 开机恢复，`action.sh` 切换，见[常驻通知与保活](keepalive.md)） |
| `tools/*.py` | 离线 DEX 分析工具 |
| `tools/verify-onboot.sh` | 一次性开机自检脚本 |

## 微信数据层

路径、密钥来源、SQLCipher 参数与安全边界见[微信消息后端](wechat-store.md)。字段语义：

- 群 ID = `<数字>@chatroom`。`channel.type`：群 0、私聊 1
- 群消息 `content` = `wxid_xxx:\n正文`，发送者前缀需解析
- `message.type`：`1` 文本、`10000` 系统，其余（3 图片、34 语音）按内容判定
- `rcontact.type`：`3` 好友，`1` 系统号，`33` / `gh_%` 公众号。群用 `username LIKE '%@chatroom'` 判定
- `chatroom` 表（群成员与角色）：
  - `memberlist` = `wxid1;wxid2;…`（分号分隔）
  - `displayname` = 群内昵称，用 U+3001 `、`（UTF-8 `E38081`）分隔，与 memberlist 同序
  - `roomowner` = 群主 wxid，`memberCount` = 人数
  - `roomdata` = 成员 protobuf：`ChatRoomData{ repeated ChatRoomMember member = 1 }`、`ChatRoomMember{ string userName = 1; …; int32 flag = 3 }`，`flag & 2048` = 管理员。成员顺序与 memberlist 不一定一致，按 wxid 查
  - 数量对不上时忽略群昵称、回落到 rcontact（`wx_store.cpp` 处理）
  - 微信没有自定义角色，角色只有合成的 `owner` / `admin` / `member`。只有 `admin` 可被 `guild.member.role.set/unset` 变更

## 发送

路径见[微信消息发送路径](wechat-send.md)，各媒体类型见[发送各类消息](wechat-send-types.md)，群管理见[群管理写操作](wechat-room.md)。要点：

- 派发器要在构造场景之前拿到。构造器会写库，拿不到派发器时不该先落一条 SENDING 行。
- 没有开关，不限目标、不限速。成功表示已交给微信派发，不是投递确认。
- 撤回只对本账号发出的消息有效（`MsgInfo.z0()==1`），别人的消息回 502 + `rejected:true`。群主撤回他人消息未做。
- 拍一戳走 `/v1/internal/pat`：`nv3.l.nj` 先插本地互动行（type 922746929），再 `new qv3.b(pair, 会话, 被拍者, 0)`（cgi `sendpat`）走 `doScene(派发器, y2)`。scene int 0 是普通拍一戳，1 是改后缀。直派不走中央 runner，849 回调（svrId 回填、失败 Toast）不发生，本地 svrId 停在 0。`nj` 拒绝时 `PatSend` 直接 rejected。
- 资源路由：`upload.create` 落盘到 `tempstore`，返回 `internal:wechat/<user>/_tmp/<name>`（5 分钟）。收到的媒体是 `internal:wechat/<user>/_msg/<kind>/<id>/<签名>`。`/v1/proxy` 对 `internal:` 解析登录号后流式回文件（`_tmp` 直接查，`_msg` 交给已登记的解析器并先验签），http(s) 前缀未登记 403，非法 400，未知登录或验签失败 404，带 CORS，不需 Satori 登录头。`proxy_urls` 为空。

## 协议层要点

- 端点：`/v1/meta`、`/v1/meta/webhook.create|delete`、`/v1/internal/status|capabilities`、`/v1/{resource}.{method}`
- 账号类方法要 `Satori-Platform: wechat` + `Satori-User-ID: <wxid>`
- 非 login 方法：不在 features → 404（在参数校验之前判定，缺参也回 404 而非 400），在 features 但后端没实现 → 501
- `g_login_count`（protocol.cpp）由 `Apply` 更新。`wx_live` 等它 >0 再发事件，避免事件被丢
- store 事件的 `login.sn` 必须等于账号适配器的 sn（当前 = 1）
- `server.cpp` 不知道发送的存在，只用 `StatusProvider` 钩子。测试与独立工具不注册，响应保持精简
- 服务端是单线程事件循环，后端调用在它上面同步执行。图片发送最多阻塞它 6 秒等库里出现行，别在后端里加更长的等待
- `internal/status` 有 `events` 块（`open`、`emitted`、`skipped`、`dropped`），`internal/capabilities` 有 `event_types`

## 离线 DEX 工具

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

`tools/dexlib.py` 是精确指令解码器。`libapp.so` 里的编译化 dex 这些工具看不到，能用可读 dex 拼出来的路径优先别去碰它。JADX 也可用：`~/tools/jadx/bin/jadx --single-class <点分名> -d <输出目录> base.apk`。

库检查台 `tools/wxq.cpp`（`./build.sh` 不编它，手编：`clang -O2 -I native tools/wxq.cpp native/wcdb.cpp -ldl -o build/wxq`）：只读跑一条 SELECT，参数是 libWCDB.so 路径、库路径、`hex:<密钥>` 或 `-`（明文）。密钥在 `<微信数据目录>/files/satori-wx/key.log` 的 `hex=`。热拷贝 db+wal 的副本读不了，密钥确定正确时直接以只读方式开活库是安全的。

## 真机验收

模块的 `.so` 在开机时被 Zygisk Next 钉成 memfd，覆盖磁盘文件后必须重启手机才会加载新代码。主机测试里没有的部分只能在这里验：JNI 调用、真实的库与文件。

```sh
T=$(su -c 'cat /data/adb/modules/satori_wx/satori-wx.conf' | sed -n 's/^token=//p')
ME=wxid_8zxjsghrk8vz41
api() { local body=${2:-'{}'}; curl -s -X POST "http://127.0.0.1:5601/v1/$1" -H "Authorization: Bearer $T" \
  -H "Satori-Platform: wechat" -H "Satori-User-ID: $ME" -H 'Content-Type: application/json' -d "$body"; echo; }

# 0) events.watching 必须 true。keepalive.wakelock 与 wakelock_held / cpu_held / wifi_held
#    开机后都应当是 true。HTTP 与 event 延迟都是几十毫秒
api internal/status | head -c 600
api internal/status | python3 -c 'import json,sys; print(json.load(sys.stdin)["keepalive"])'
python3 tools/latency-probe.py -n 10

# 1) 版本、事件计数、send.media。capabilities 里要有 event_types
api internal/status | head -c 1500
api internal/capabilities | head -c 1500

# 2) 读侧：最新一页历史（升序、带 prev），再用 prev 往回翻一页
api message.list '{"channel_id":"filehelper","limit":5}'

# 3) 媒体链接：从任意一页历史里找一个 <img src="internal:…"> 或 <audio>，取出来
#    200 + image/jpeg（或 audio/silk）；把签名改一位应当是 404；Range 请求是 206
curl -s -D- -o /dev/null "http://127.0.0.1:5601/v1/proxy/internal:wechat/$ME/_msg/image/<msgId>/<签名>"
curl -s -D- -o /dev/null -H 'Range: bytes=0-99' "http://127.0.0.1:5601/v1/proxy/internal:wechat/$ME/_msg/image/<msgId>/<签名>"

# 4) 发图到 filehelper（只影响自己）：先上传，再用返回的链接发；再试 data URI
curl -s -X POST http://127.0.0.1:5601/v1/upload.create -H "Authorization: Bearer $T" -H "Satori-Platform: wechat" \
  -H "Satori-User-ID: $ME" -F 'file=@/path/to/pic.png'
api message.create '{"channel_id":"filehelper","content":"<img src=\"internal:wechat/'$ME'/_tmp/xxx.png\"/>"}'
#   成功 = 200 + Message[]，id 是库里 type=3 且 content 已带 CDN XML 的那一行。
#   502 image_unconfirmed = 6 秒内没有入库；带「pipeline stalled」= 微信收下但管线卡死
#   （退化图，如 1x1，微信自己发不出去）；502 send_failed 带 detail = 哪个类 / 成员没找到，
#   同时看 internal/status 的 send.last_error 与 send.media 计数
#   样图用 8x8 以上的 PNG，不要用 1x1。坏链接（远程 URL）整条 400、什么都没发出去

# 5) 事件（另开一个终端跑着，再做 6、7）
python3 tools/events-tail.py --token "$T"

# 6) 撤回：发一条文字再删掉，events-tail 应先后看到 message-created 与 message-deleted
api message.create '{"channel_id":"filehelper","content":"撤回测试"}'
api message.delete '{"channel_id":"filehelper","message_id":"<上一步返回的 id>"}'

# 6b) 发文件、发视频、语音、引用回复（filehelper，只影响自己）
#     先 upload.create（multipart），再把返回的 internal: 链接放进元素；再用返回的 id 验引用
curl -s -X POST http://127.0.0.1:5601/v1/upload.create -H "Authorization: Bearer $T" -H "Satori-Platform: wechat" \
  -H "Satori-User-ID: $ME" -F 'file=@/path/to/clip.mp4;type=video/mp4'
api message.create '{"channel_id":"filehelper","content":"<video src=\"internal:wechat/'$ME'/_tmp/xxx-clip.mp4\"/>"}'
#   期望 200 + Message[]：<video src poster duration="秒数"/>；库里 type=43，status 1 → 2
api message.create '{"channel_id":"filehelper","content":"<file src=\"internal:wechat/'$ME'/_tmp/xxx-a.pdf\" title=\"季度报告.pdf\"/>"}'
#   期望 200：<file src title="季度报告.pdf"/>；库里 type=1090519089，status 2
api message.create '{"channel_id":"filehelper","content":"<audio src=\"internal:wechat/'$ME'/_tmp/xxx-a.mp3\"/>"}'
#   期望 200：<audio src duration="秒数"/>；库里 type=34，status 2；超过 60 秒的回 <file>
api message.create '{"channel_id":"filehelper","content":"<quote id=\"<某条已有消息的 id>\"/>引用回复"}'
#   期望 200，content 是 <quote id/>正文；库里 type=822083633，MsgQuote 有配对行
api internal/capabilities   # message_elements 与 limits

# 7) 收消息：让别人给你发图片 / 语音 / 引用回复 / 在群里 @ 你，events-tail 里对应看到
#    <img>/<audio>、<quote id=…/>、<at id="wxid_8zxjsghrk8vz41" …/>

# 8) 拍一戳：发到文件助手会 rejected（filehelper 不可拍），在真群里拍一个成员应当 200
api internal/pat '{"channel_id":"filehelper","user_id":"wxid_8zxjsghrk8vz41"}'   # 期望 502 rejected:true
api internal/pat '{"channel_id":"<群id>@chatroom","user_id":"<群成员wxid>"}'      # 期望 200 {"ok":true}
```

注意：

- 群里的 @ 发送（`<at id=… name=…/>`）需要一个真的群。验证办法是看微信库里那行的 `lvbuffer` 是否带 `<atuserlist>`，以及对方手机上是否高亮。
- 群成员 / 好友事件要等真的有人进群 / 加好友才看得到。
- 重启后 `internal/status` 的 `events.open` 应为 true（key.log 里有 spec 才会开库），`events.skipped` 与 `events.dropped` 应为 0。
- 图片这一项失败时 `SendImage` 每一步失败都会写进 `send.last_error`。先查类 / 方法名是否对得上（微信版本不是 8.0.78），其次是 `rj` 是否需要主线程调用。

## 不重启手机的真机调试

线上模块 `.so` 被 Zygisk Next 钉住，换了要重启手机。验证 JNI 改动不必这样：

```sh
tools/dev/dev.sh          # 编 build/dev/satori-wx-dev.so → 拷进微信私有目录 → ptrace 让微信主进程 dlopen（root）
tools/dev/dev.sh --port   # 最近一次注入的端口（5610 + 代际 % 80），令牌沿用线上的
```

它是同一套 server / backend / sender（去掉 `module.cpp`，不启保活与密钥捕获），在自己的端口上提供完整的 Satori 服务，curl 命令与上面一样，只换端口。`dev_entry.cpp` 另开「端口 +100」的单行控制口（`file` / `video` / `quote` 直调发送器，回一行 JSON），排查「哪个类找不到」最快。每次重跑得到新端口，旧代际留在进程里无害，微信重启后消失。新增源文件要同步 `dev.sh` 的 `SRC` 列表（和 `build.sh`、`tests/run.sh` 一样）。

查库用 `tools/wxq.cpp`。反编译用常驻的 jadx 服务：一次加载约一分钟，之后 `dec` / `use` / `find` 毫秒级。jadx 输出里 `f233841f` 这类名字是它改的，JNI 用原名（注释 `renamed from`）。

## 已知限制

README 的「限制 / 风险」是使用面清单。开发者另需知道：

1. 密钥捕获把明文写进 `files/satori-wx/key.log`（0600）。
2. 微信 `:push` 子进程有 mars。服务端只在主进程，发送靠反射，不依赖子进程。
3. 事件延迟：撤回 ≤ 3 秒，群成员 ≥ 3 秒（两轮确认），好友 12–24 秒。模块启动前发生的变化不补发。
4. 服务端事件序号以进程启动时的毫秒为基线，不跨进程回放。

## 管理应用

`app/` 是独立构建的 Android 原生管理界面（`com.satori.wx`，需要 JDK / aapt，与纯 native 的模块构建互不依赖）。它通过 `su` 读写 `satori-wx.conf`、经 `/v1/internal/status` 与 `/v1/meta` 看状态。常驻通知上的唤醒锁按钮是一个显式广播，落到应用的 `keepalive.WakeToggleReceiver`，它再把切换转到 `internal/wakelock`。

改 `ReadConfig` 的规则时同步改 `app/src/com/satori/wx/core/Conf.java`。`internal/status` 的 `send` 与 `keepalive` 块字段名被应用读取（`enabled`、计数与 `last_*`；`notification`、`wakelock`、`cpu_held`、`wifi_held`），改名要同步。构建与结构见 [app/README.md](../app/README.md)，设计规范见[知言应用设计规范](app-design.md)。
