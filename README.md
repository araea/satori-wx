# 知言（satori-wx）

微信的 Satori v1 实现端：通过 Zygisk 注入把微信暴露为统一接口——收消息（文本、图片、语音、视频、表情、链接、回复、@）、事件（撤回、入退群、好友）、发文本 / 群内 @ / 引用回复 / 图片 / 视频 / 语音 / 文件

[![GitHub](https://img.shields.io/badge/GitHub-araea%2Fsatori--wx-181717?logo=github&logoColor=white)](https://github.com/araea/satori-wx)

纯 native C++ 服务端：无 DEX、无 Java 助手、无 APK、无 ArtMethod 偏移、无 hook 引擎。账号身份读取微信自己持久化的 SharedPreferences（同 uid 直接读文件，不 hook、不改写、不访问数据库）；消息用微信自己的 `libWCDB.so` 只读打开 `EnMicroMsg.db`，密钥在 `RegisterNatives` 边界捕获。发送、撤回与群管理由宿主 ClassLoader 反射调用微信自己的 NetScene 与网络派发器，由微信完成入库、加密、发送；没有开关，不连客户端就不会有任何写操作。

## 安装

通过 Magisk / KernelSU 管理器安装服务端 ZIP，重启设备让模块生效。要求已有可用的 Zygisk 实现（使用 Zygisk Next 按其官方要求配置）。

安装器首次生成 256 位随机 token，升级保留现有配置。配置文件位于 `/data/adb/modules/satori_wx/satori-wx.conf`：

```ini
port=5601
token=<安装器生成的令牌>
```

- 端口可改；token 使用 32–128 位字母、数字、`-` 或 `_`。
- 配置权限为 `0600`。token 缺失、过长、重复字段或无效时服务端不启动。
- 发送没有开关、没有限速、没有白名单。旧配置里的 `send=on|off` 与 `send_allow=` 仍被接受，但已不起作用。
- 配置在 `preAppSpecialize` 读取，文件描述符立即关闭；服务在 `postAppSpecialize` 启动，只在精确匹配的微信主进程内运行，非目标进程和 system_server 请求卸载模块。
- 服务端只监听 `127.0.0.1`，随微信主进程结束而退出；配置更改在下一次进程启动生效。

也可用知言应用（`app/`）管理：查看连接链路、编辑端口与令牌，并一键重新启动微信让配置生效。应用需要 Root 授权。

## 快速使用

Satori 客户端填写：

- API：`http://127.0.0.1:5601/v1`
- Events：`ws://127.0.0.1:5601/v1/events`
- Token：配置文件中的 `token` 值

已登录时 `READY` / `/v1/meta` 携带真实 `logins`；账号快照的 `features` 只声明后端真正实现的方法，未实现的方法返回 404，不会伪造成功。

## 接口

HTTP 使用 `Authorization: Bearer <token>`：缺失 token 返回 401，错误 token 返回 403。账号类接口要求 `Satori-Platform: wechat` 和 `Satori-User-ID`。未知接口返回 404，已有 RPC 接口错误方法返回 405。`login.list` 不属于这里实现的 Satori v1 API，登录集合从 meta / READY 获取。

| 接口 | 行为 |
| --- | --- |
| `POST /v1/meta` | 返回只读身份快照 `{"logins":[...],"proxy_urls":[]}`；无账号时 `logins` 为空 |
| `POST /v1/meta/webhook.create` / `webhook.delete` | 注册 / 注销 WebHook（`url` 必填、`token` 可选），最多 4 个，仅 http |
| `POST /v1/internal/status` | 版本、native 状态、`send` 与 `keepalive` 状态块 |
| `POST /v1/internal/wakelock` | 切换 CPU / Wi-Fi 唤醒锁（`{"on":true\|false}` 或 `{"toggle":true}`） |
| `POST /v1/login.get` | 返回已登记账号快照；未登录或身份不匹配时返回 403 |
| `POST /v1/{resource}.{method}` | 标准方法参数校验与 native 后端分发 |
| `POST /v1/upload.create` | 标准 multipart 上传，边收边落盘到 `files/satori-wx-tmp/`（上限 1 GiB，不占内存；令牌与登录在读 body 之前校验），返回 `internal:wechat/<user>/_tmp/<name>`（5 分钟有效），经 `/v1/proxy` 回读；视频、文件先传这里，再把链接放进 `<video src>` / `<file src>` |
| `GET /v1/proxy/{url}` | 资源代理：`internal:` 链接按登录号解析并回文件；非法 URL 400；未登记 http(s) 前缀 403；未知登录 404 |
| `POST /v1/internal/capabilities` | 报告 `send` 状态块、`unsupported`、`message_elements`（`message.create` 认得的元素）与 `limits` |
| `GET /v1/events` | WebSocket upgrade；10 秒内 IDENTIFY；READY、登录事件与 PING / PONG |

`features` 的唯一来源是 `native/wx_capabilities.cpp`。已实现方法：

- 读侧 13：`message.get/list`、`user.get`、`friend.list`、`guild.get/list`、`guild.member.get/list`、`guild.role.list`、`guild.member.role.list`、`channel.get/list`、`user.channel.create`
- 资源 1：`upload.create`
- 写侧 6：`message.create`（文本 / @ / 引用回复 / 图片 / 视频 / 语音 / 文件）、`message.delete`、`channel.delete`（退群）、`guild.member.kick`、`guild.member.role.set/unset`
- 账号 1：`login.get`

其余标准方法返回 404。WebSocket 按 Satori 的 `op=3` + `body.token` 鉴权，成功后 `op=4`，`op=1` 心跳回复 `op=2`，每 10 秒发送一次 PING，连续 30 秒无响应则断开；支持掩码、文本分片、控制帧交错、TCP 分包 / 合包与关闭握手，64 条有界历史，登录事件不参与回放，窗口外序号用 4009 拒绝恢复。资源限制：8 个并发连接、8 KiB HTTP 头、16 KiB 请求体 / WS 消息（`message.create` 可到 16 MiB，`upload.create` 流式收到 1 GiB，都先验令牌）、单条事件 128 KiB；HTTP 每次响应后关闭连接，暂不支持 TLS、chunked 请求体。

## 限制 / 风险

- 发送：文本、群内 `@`（`<at id name/>` / `<at type="all"/>`）、引用回复（`<quote id/>`）、图片、视频、文件，全部真机验证。每个媒体元素单独成一条微信消息，按书写顺序发；先全部解析核对再动手，坏一个整条 400。`src` 只认 `upload.create` 的链接、`data:` URI 与 `base64://`（远程 URL 没有 HTTP 客户端，会 400 并提示先上传）。
  - `<video>`：MP4 / MOV 且真有视频轨才发成可点播的视频气泡（时长取自文件，`poster` 可选，缺省由微信抽帧）；MKV / WebM / FLV 这类微信不能内联播放的，作为文件发出。
  - `<file>`：文件名取 `title`，其次是上传时的文件名，再次按内容猜扩展名。
  - `<audio>`：发成语音条。任何 Android 能解码的音频（MP3、M4A/AAC、OGG/Opus、AMR、FLAC、WAV…）都会转成微信的 SILK（用微信自己的编码器，模块不带编解码器），已经是微信 SILK 的原样发；回执里是 `<audio duration>`。超过微信的 60 秒上限、短于 0.2 秒或读不出来的，作为文件发出，不会丢。
  - `<quote id>`：`id` 是本地消息 id 或 svrid，找不到被引用的消息就退成普通文本；微信的回复只带文字，所以引用挂在请求里第一段文字上，只有媒体时引用被忽略。
  - 视频、文件的上传由微信自己完成：回执在库里出现该行并等到「发送中」结束（最多几秒）才回，慢的上传不算失败，失败（微信标为失败）回 502 `upload_failed`（见[发送各类消息](docs/wechat-send-types.md)）。
- 收到的图片多半只有缩略图，原图是微信私有的 `wxgf` 容器；语音是 SILK，不转码（见[消息内容](docs/wechat-content.md)）。
- 好友 / 入群申请事件与对应的 approve 方法没做。事件的延迟与限制见[事件](docs/wechat-events.md)。
- 微信无此概念的方法列入 `internal/capabilities.unsupported`：`message.update`、`channel.create`、`channel.mute`、`guild.member.mute`、`reaction.*`、`guild.role.create/update/delete`。
- 微信被系统冻结时回环端口握手成功但无响应，客户端挂起至超时；root 侧 `wxguard` 默认 ARMED 负责解冻（见[常驻通知与保活](docs/keepalive.md)）。
- 只在 arm64 上构建与运行。
- 写操作没有开关，成功表示「已交给微信派发」而非投递确认；破坏性动作不伪造成功，风控责任在调用方。

## 链接

- [ZygiskNext](https://github.com/LSPosed/ZygiskNext)
- [标准 Zygisk 模块接口](https://github.com/topjohnwu/zygisk-module-sample)
- [Satori HTTP API](https://satori.chat/zh-CN/protocol/api.html) / [事件](https://satori.chat/zh-CN/protocol/events.html) / [元信息](https://satori.chat/zh-CN/advanced/meta.html)
- [知言应用设计规范](docs/app-design.md)
- [只读账号身份说明](docs/wechat-account.md)
- [消息后端设计（native、低特征）](docs/wechat-store.md)
- [消息内容、媒体链接与历史分页](docs/wechat-content.md)
- [事件](docs/wechat-events.md)
- [微信消息发送路径（反射）](docs/wechat-send.md)
- [发送各类消息（反射，含图片管线）](docs/wechat-send-types.md)
- [微信群管理写操作（反射）](docs/wechat-room.md)
- [常驻通知与保活（wxguard）](docs/keepalive.md)
- [协议覆盖矩阵](docs/satori-conformance.md)
- [研究记录与已知边界](docs/native-server.md)
