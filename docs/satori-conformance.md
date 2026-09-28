# Satori v1 协议层验收（v0.9.2）

本版本提供 native 协议服务端、native 后端接口、只读账号身份与消息库适配层、**内置资源路由**
（`upload.create` 与 `/v1/proxy`），以及一个**默认关闭**的反射写操作集（消息发送/撤回 + 群管理）。
读侧 13 个方法已实现（消息、历史、联系人/群/频道、群成员与角色、私聊频道），`upload.create` 1 个；
`send=on` 时另实现 `message.create/delete`、`channel.delete`（退群）、`guild.member.kick`、
`guild.member.role.set/unset` 共 6 个；`login.get` 1 个；11 个方法微信无法表达（见下），
其余为尚未实现的写操作。下表中的“完成”指协议层及带独立测试后端/账号适配层的验收，
不代表微信支持或已适配所有 API。

| 标准项目 | 实现 / 验证 |
| --- | --- |
| HTTP RPC、Bearer、Satori-Platform / Satori-User-ID | 完成；401 缺令牌、403 令牌错误或账号不存在、405 方法错误 |
| 标准方法目录 | 37 个方法；与上游 protocol/src/index.ts 的 Methods 对照 |
| JSON 参数 | 必填、可选、字符串、对象、布尔、非负整数、分页枚举验证 |
| 方法可用性 | 登录快照 features 控制；不支持返回 404，声明支持但无 handler 返回 501，离线返回 503 |
| login.get / meta / READY | 同一份登录快照；登录身份由只读偏好解析得到，无账号时为空 |
| message.create / update 的 content | 保留 Satori 标记字符串；提供 native 文本转义 helper，不把标记当 HTML 执行 |
| message.create（可选发送） | `send=on` 时反射调微信自己的 NetSceneSendMsg 发纯文本；**返回 `Message[]`（官方客户端对结果调用 `.map()`）**；默认关闭，开启后不限目标、不限速；成功＝已派发，非投递确认 |
| message.create 的媒体元素 | 只带 `<img>` 之类元素的 content 返回 400 `media_unsupported`（App 发新图要跑 Kotlin 协程，收尾回调 native 交不出来，见 [发送各类消息](wechat-send-types.md)）；不会假报成功，也不写任何行 |
| message.delete（可选撤回） | `send=on` 时用 `ex0.k0.F0.k(talker,localId)` 取 MsgInfo，反射调 `com.tencent.mm.modelsimple.d1`（cgi revokemsg）；只撤回本账号消息，复用同一套开关 |
| channel.delete / guild.member.kick（可选） | `send=on` 时反射 `qn.p`（cgi delchatroommember），退群用 `[self]`，踢人用目标 wxid；`send=on` 才进 features |
| guild.member.role.set / unset（可选） | 反射 `qn.b`/`qn.e`（cgi add/delchatroomadmin），经 `com.tencent.mm.modelbase.z2.d(o,null,false)` 走微信 Cgi 运行器；只变更 `admin` 角色 |
| upload.create | core 内置 SDK 默认实现，`multipart/form-data` 落盘为 `internal:wechat/<user>/_tmp/<name>`（5 分钟、随机名、0600），落在微信数据目录下的 `files/satori-wx-tmp/`；链接由 `/v1/proxy` 回读（发送端不消费） |
| /v1/proxy/{url} | `internal:` 链接按登录号解析并回文件；未登记 http(s) 前缀 403；非法 400；未知登录 404；带 CORS，不需 Satori 登录头 |
| guild.member.get / list | 读 `chatroom` 的 memberlist + displayname（`、` 分隔）+ roomowner；`next` 是成员偏移；displayname 与 memberlist 数量不一致时忽略群昵称、回落到 rcontact |
| guild.role.list / guild.member.role.list | 合成角色：`owner`（群主）/ `admin`（管理员）/ `member`（成员）；管理员位读 `chatroom.roomdata` 的成员标志（`flag & 2048`），没有 roomdata 缓存时按普通成员算；非成员返回空列表；未知群返回 404 |
| user.channel.create | 返回该 wxid 的私聊频道（`type=1`） |
| 微信无法表达（`internal/capabilities.unsupported`） | `message.update`、`channel.create`、`channel.mute`、`guild.member.mute`、`reaction.create/delete/clear/list`、`guild.role.create/update/delete`。微信不能编辑消息、群内没有子频道、没有服务端禁言、无表态、无自定义角色 |
| 未实现的写操作 | 群改名、好友删除/审批、入群审批：卡点见 [群管理写操作](wechat-room.md)。只有真实实现的方法才进 features，否则返回 404 |
| 分页 | params 的 next/direction/limit/order 与后端返回的 data/prev/next 原样传递 |
| upload.create | multipart/form-data，有界二进制零拷贝解析，字段名与返回 URL 映射由后端实现 |
| WebSocket | RFC 6455 握手、掩码、文本分片、控制帧交错、UTF-8、关闭、大小限制 |
| IDENTIFY / READY / PING / PONG | 完成；10 秒鉴权、30 秒心跳期限 |
| EVENT | native 生产者队列；服务端分配 sn / timestamp；多客户端广播 |
| 登录事件 | added/updated/removed 更新快照；非登录事件只带 sn/platform/user 身份 |
| META | op=5，当前仅广播空 proxy_urls；不在 META 中传 logins |
| 会话恢复 | 64 条有界历史、按客户端游标发送；登录事件不回放；过期或未来序号拒绝恢复 |
| 背压 | 32 条生产队列；满时 Publish 返回 false；单客户端发送缓冲有界；回放超出缓存时关闭 |
| WebHook（标准可选） | 完成：`/v1/meta/webhook.create` / `webhook.delete`，EVENT/META 以 `Satori-Opcode` 推送，可选 `Authorization`；仅 http，无 TLS |
| 资源代理 | 未实现，proxy_urls 保持空，相关路由 404 |

序号以进程启动时的 Unix 微秒为基线，在进程内递增，并限制在 JSON/JavaScript 安全整数范围。
正常重启后旧进程序号落在新窗口外，会被拒绝；不宣称跨进程持久回放。
显式 sn=0 仅在当前进程历史未丢弃非登录事件时允许从缓存起点恢复。

HTTP 请求体 / WS 消息最大 16 KiB、HTTP 头 8 KiB、单事件 4 KiB、最多 16 个登录快照，
单连接 HTTP 响应后关闭。当前 HTTP profile 不支持 chunked 请求体、TLS 或 WebSocket 压缩。
这些限制是实现约束，并非 Satori 标准规定的上限。

## 方法目录

- channel.get/list/create/update/delete/mute
- message.create/update/delete/get/list
- reaction.create/delete/clear/list
- upload.create
- guild.get/list/approve
- guild.member.get/list/kick/mute/approve
- guild.member.role.set/unset/list
- guild.role.list/create/update/delete
- login.get
- user.get/channel.create
- friend.list/delete/approve

`internal/status`、`internal/capabilities` 是本项目诊断扩展，不冒充标准 API。
标准方法目录不是登录账号的 features；只有后端明确实现的能力才能列入 features。

## 测试

`./tests/run.sh`：22 项 HTTP/WebSocket socket 测试、10 项协议测试、账号解析/状态机/Hub 集成测试、
账号端到端适配测试、能力/配置测试（features 开关、`send` 解析、旧 `send_allow` 仍被接受、
发送门禁）、探针并发/资源/权限测试。
协议测试遍历 37 个方法，并验证上传二进制、标记保留、分页、广播、登录状态、元信息、
历史窗口淘汰以及超过发送缓冲容量的分段回放。
内容测试（`tests/content_test.cpp`）覆盖标记拍平与 `<img src>` 抽取：属性顺序、两种引号、
属性值里的 `>`、空 `src`、未闭合标签、只认 `img`、数量上限。
store 测试用夹具库覆盖群成员与角色：`roomdata` 里 `flag=2048` 的成员读成 `admin`，
没有 `roomdata` 缓存时读成 `member`，群主优先。
tempstore 测试覆盖 `internal:` 链接的解析（外链、别的平台、`_tmp` 之外、路径穿越、未知
名字、手工放进去的文件、输出缓冲太小），这条曾经因为 `sizeof` 用在指针上而全数失败。
backend 测试把真实的 `wx_backend.cpp` 接进来说话（store/群管理/保活用桩），覆盖 send 开关、
文本拍平后交给发送器、只带图片的 content 得到 400 `media_unsupported`、图配文仍走文本、
空白内容被拒。这个文件此前完全没有覆盖，两次出错都出在它身上。
账号端到端测试用夹具偏好文件驱动真实适配层，验证 meta / login.get / READY 的一致快照、
离线状态与账号切换；WebHook 测试用本地接收端验证 `Satori-Opcode`、`Authorization` 与
信号体，以及登记上限/注销；详情见 [只读账号身份说明](wechat-account.md)。

测试后端和事件输入管道仅编译到 `build/tests/server`，不在 Zygisk 模块内。
真机检查器 `build/satori-wx-check` 只读 meta/status 和 WebSocket 信令，不发送微信消息。

## 参考

- [Satori API](https://satori.chat/zh-CN/protocol/api.html)
- [Satori 事件和会话恢复](https://satori.chat/zh-CN/protocol/events.html)
- [消息资源](https://satori.chat/zh-CN/resources/message.html)
- [上游协议方法定义](https://github.com/satorijs/satori/blob/main/packages/protocol/src/index.ts)
