# Satori v1 协议层验收（v0.6.4）

本版本提供 native 协议服务端、native 后端接口、只读账号身份与消息库适配层，
以及一个**默认关闭**的反射消息发送器。**读侧 13 个方法已实现**（消息、历史、联系人/群/频道、
群成员与角色、私聊频道），发送 1 个（可选），`login.get` 1 个；8 个方法微信无法表达（见下），
其余 14 个是
尚未实现的写操作。下表中的“完成”指协议层及带独立测试后端/账号适配层的验收，
不代表微信支持或已适配所有 API。

| 标准项目 | 实现 / 验证 |
| --- | --- |
| HTTP RPC、Bearer、Satori-Platform / Satori-User-ID | 完成；401 缺令牌、403 令牌错误或账号不存在、405 方法错误 |
| 标准方法目录 | 37 个方法；与上游 protocol/src/index.ts 的 Methods 对照 |
| JSON 参数 | 必填、可选、字符串、对象、布尔、非负整数、分页枚举验证 |
| 方法可用性 | 登录快照 features 控制；不支持返回 404，声明支持但无 handler 返回 501，离线返回 503 |
| login.get / meta / READY | 同一份登录快照；登录身份由只读偏好解析得到，无账号时为空 |
| message.create / update 的 content | 保留 Satori 标记字符串；提供 native 文本转义 helper，不把标记当 HTML 执行 |
| message.create（可选发送） | v0.6.4：`send=on` 时反射调微信自己的 NetSceneSendMsg 发纯文本；默认关闭、`send_allow` 白名单、限速；成功＝已派发，非投递确认；2026-09-27 真机向 filehelper 实发成功 |
| guild.member.get / list | 读 `chatroom` 的 memberlist + displayname（`、` 分隔）+ roomowner；`next` 是成员偏移；displayname 与 memberlist 数量不一致时忽略群昵称、回落到 rcontact |
| guild.role.list / guild.member.role.list | 合成角色：`owner`（群主）/ `member`（成员）；非成员返回空列表；未知群返回 404 |
| user.channel.create | 返回该 wxid 的私聊频道（`type=1`） |
| 微信无法表达（`internal/capabilities.unsupported`） | `message.update`、`reaction.create/delete/clear/list`、`guild.role.create/update/delete` —— 微信不能编辑消息、无表态、无自定义角色 |
| 未实现的写操作 | 撤回、群改名/退群/禁言、踢人/管理员、好友删除与三种审批、`upload.create`：各自需逆向微信内部接口，返回 404 |
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
账号端到端适配测试、能力/配置测试（features 开关、`send`/`send_allow` 解析、发送门禁）、
探针并发/资源/权限测试。
协议测试遍历 37 个方法，并验证上传二进制、标记保留、分页、广播、登录状态、元信息、
历史窗口淘汰以及超过发送缓冲容量的分段回放。
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
