# 知言（satori-wx）

微信的 Satori v1 实现端。Zygisk 注入微信后，本机提供 HTTP 与 WebSocket 接口，收发消息与事件。

[![GitHub](https://img.shields.io/badge/GitHub-araea%2Fsatori--wx-181717?logo=github&logoColor=white)](https://github.com/araea/satori-wx)

## 安装

- 微信 `com.tencent.mm`（8.0.78 / 8.0.79 已适配），arm64
- 已配置好的 Zygisk（Zygisk Next 按其官方要求配置）

在 Magisk / KernelSU 管理器刷入模块 ZIP，重启设备。重启后在模块列表里点「知言」打开管理页，确认微信账号与连接状态。

安装器生成 256 位随机 token，写在 `/data/adb/modules/satori_wx/satori-wx.conf`：

```ini
port=5601
token=<安装器生成的令牌>
```

端口与令牌可在管理页修改。升级保留现有配置。文件权限 `0600`，token 缺失或无效时服务端不启动。

## 使用

Satori 客户端填写：

- API：`http://127.0.0.1:5601/v1`
- Events：`ws://127.0.0.1:5601/v1/events`
- Token：配置文件里的 `token`

HTTP 请求带 `Authorization: Bearer <token>`。账号类接口另带 `Satori-Platform: wechat` 与 `Satori-User-ID`。WebSocket 在 10 秒内发 `op=3` 与 `body.token` 鉴权，成功后收到 `READY`。事件有 `message-created`、`message-deleted`、`guild-member-added`、`guild-member-removed`、`guild-added`、`guild-removed`、`friend-added`、`friend-removed` 与 `login-*`。

服务端只监听 `127.0.0.1`，随微信主进程退出。已登录时 `READY` 与 `/v1/meta` 携带真实 `logins`，账号快照的 `features` 只声明实现了的方法。

## 接口

| 接口 | 行为 |
| --- | --- |
| `POST /v1/meta` | 身份快照 `{"logins":[...],"proxy_urls":[]}`；无账号时 `logins` 为空 |
| `POST /v1/meta/webhook.create` / `webhook.delete` | 注册 / 注销 WebHook（`url` 必填、`token` 可选），最多 4 个，仅 http |
| `POST /v1/login.get` | 已登记账号快照；未登录或身份不匹配返回 403 |
| `POST /v1/upload.create` | multipart 上传，边收边落盘到 `files/.svc/tmp/`，上限 1 GiB，返回 `internal:wechat/<user>/_tmp/<name>`（5 分钟有效） |
| `GET /v1/proxy/{url}` | 资源回读：`internal:` 链接按登录号解析；未登记的 http(s) 前缀返回 403 |
| `POST /v1/{resource}.{method}` | 标准方法分发 |
| `POST /v1/internal/status` | 版本、native 状态、`send`（ready / waiting / incompatible）与 `keepalive` |
| `POST /v1/internal/wakelock` | CPU / Wi-Fi 唤醒锁：`{"on":true\|false}` 或 `{"toggle":true}` |
| `POST /v1/internal/at_all_quota` | 本号能否在群里 @所有人，与 satori-qq 同名同形 |
| `POST /v1/internal/capabilities` | `send` 状态、`unsupported`、`message_elements`、`limits`、`actions` |
| `GET /v1/events` | WebSocket：`READY`、登录事件与 PING / PONG |

已实现方法：`message.get/list/create/delete`、`user.get`、`friend.list`、`guild.get/list`、`guild.member.get/list`、`guild.role.list`、`guild.member.role.list/set/unset`、`guild.member.kick`、`channel.get/list/delete`、`user.channel.create`、`upload.create`、`login.get`。其余标准方法返回 404，已有接口的错误方法返回 405。

错误体统一为 `{"code": "...", "message": "..."}`：缺失 token 401，错误 token 403，未知接口 404。

## 发送消息

`message.create` 支持文本、群内 @、引用回复、图片、视频、语音、文件与合并转发。一次请求可含 8 个媒体元素，其中图片 4 张。图片最多等 6 秒，视频与文件 8 秒，整条请求 12 秒。

- @：`<at id="wxid" name="昵称"/>`，`<at type="all"/>` 需要群主或管理员。
- 引用：`<quote id="..."/>`，`id` 取本地消息 id 或 svrid。微信回复只带文字，引用挂在第一段文字上。
- `src` 只认 `upload.create` 返回的链接、`data:` URI 与 `base64://`。远程 URL 没有 HTTP 客户端，返回 400。
- `<video>`：MP4 / MOV 且有视频轨，发成可点播的视频；其余容器作为文件发出。
- `<file>`：文件名取 `title`，其次取上传时的文件名，最后按内容猜扩展名。
- `<audio>`：转成微信 SILK 语音条（用微信自己的编码器）。超过 60 秒、短于 0.2 秒或解不出来的，作为文件发出。
- `<message>` 把一次请求拆成多条消息，回执按序合成数组；`<message forward>` 发成「聊天记录」卡片，一张卡最多 100 行，单行最多 4000 字节。
- `<p>` 与相邻内容之间换行。每个媒体元素单独成一条微信消息。

先解析核对全部元素再动手，坏一个整条返回 400。

## 限制 / 风险

- 发送、撤回、群管理靠反射调用微信混淆过的类，名字每个版本都变。内置 8.0.78 与 8.0.79 两套，对不上的版本在后台从安装包推导（约 11 秒，写入缓存）。推导不出来时 `send.state` 为 `incompatible`，发送失败，接收与事件不受影响。
- 收到的图片多半只有缩略图，原图是微信私有的 `wxgf` 容器。语音是 SILK，不转码。
- 好友申请与入群申请事件、对应的 approve 方法没有实现。微信没有对应概念的方法列在 `capabilities.unsupported`。
- 微信被系统冻结时，回环端口能握手但不响应，客户端挂起至超时。root 侧 `wxguard` 默认负责解冻。
- 落盘只有一个隐藏目录 `files/.svc`，线程名与通知渠道 id 不带模块名。
- 写操作没有开关，成功只表示已交给微信派发，不代表投递成功。风控责任在调用方。
- 并发 8 个连接，请求体与 WS 消息 16 KiB（`message.create` 16 MiB），单条事件 128 KiB。不支持 TLS 与 chunked 请求。

## 链接

- [ZygiskNext](https://github.com/LSPosed/ZygiskNext)
- [Satori HTTP API](https://satori.chat/zh-CN/protocol/api.html) / [事件](https://satori.chat/zh-CN/protocol/events.html)
