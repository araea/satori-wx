# 知言（satori-wx）

微信的 Satori v1 实现端：通过 Zygisk 注入把微信暴露为统一接口，目前仅文本收发

[![GitHub](https://img.shields.io/badge/GitHub-仓库-181717)](https://github.com/araea/satori-wx)

纯 native C++ 服务端。无 DEX、无 Java 助手、无 APK、无 ArtMethod 偏移、无 hook 引擎。账号身份读取微信自己持久化的 SharedPreferences（同 uid 直接读文件，不 hook、不改写、不访问数据库）；消息用微信自己的 `libWCDB.so` 只读打开 `EnMicroMsg.db`，密钥在 `RegisterNatives` 边界捕获（见 `native/wx_key.cpp`）。发送、撤回与群管理是可选写操作，配置 `send=on` 后由宿主 ClassLoader 反射调用微信自己的 NetScene 与网络派发器，由微信完成入库、加密、发送。

## 安装

通过 Magisk / KernelSU 管理器安装服务端 ZIP，重启设备让模块生效。要求已有可用的 Zygisk 实现（使用 Zygisk Next 按其官方要求配置）。

安装器首次生成 256 位随机 token，升级保留现有配置。配置文件位于：

```text
/data/adb/modules/satori_wx/satori-wx.conf
```

格式（端口可改，token 使用 32–128 位字母、数字、`-` 或 `_`；`send` 默认关闭）：

```ini
port=5601
token=<安装器生成的令牌>
send=off
```

配置权限为 `0600`。token 缺失、过长、重复字段或无效时服务端不启动。`send=on` 后所有会话都允许发送，没有限速也没有白名单。配置在 `preAppSpecialize` 读取，文件描述符立即关闭；服务在 `postAppSpecialize` 启动，只在精确匹配的微信主进程内运行，非目标进程和 system_server 请求卸载模块。服务端只监听 `127.0.0.1`，随微信主进程结束而退出；配置更改在下一次进程启动生效。

也可用知言应用（`app/`）管理：查看连接链路、编辑发送开关、端口与令牌，并一键重新启动微信让配置生效。应用需要 Root 授权。

## 快速使用

Satori 客户端填写：

- API：`http://127.0.0.1:5601/v1`
- Events：`ws://127.0.0.1:5601/v1/events`
- Token：配置文件中的 `token` 值

已登录时 `READY` / `/v1/meta` 携带真实 `logins`；账号快照的 `features` 只声明后端真正实现的方法，未实现的方法返回 404，不会伪造成功。

## 配置 / API

HTTP 使用 `Authorization: Bearer <token>`。缺失 token 返回 401，错误 token 返回 403。账号类接口要求 `Satori-Platform: wechat` 和 `Satori-User-ID`。未知接口返回 404，已有 RPC 接口错误方法返回 405。`login.list` 不属于这里实现的 Satori v1 API，登录集合从 meta / READY 获取。

| 接口 | 行为 |
| --- | --- |
| `POST /v1/meta` | 已登录时返回只读身份快照 `{"logins":[...],"proxy_urls":[]}`；无账号时 `logins` 为空 |
| `POST /v1/meta/webhook.create` / `webhook.delete` | 注册/注销 WebHook（`url` 必填、`token` 可选），最多 4 个，仅 http |
| `POST /v1/internal/status` | 版本、native 状态、`send` 与 `keepalive` 状态块 |
| `POST /v1/internal/wakelock` | 切换 CPU / Wi-Fi 唤醒锁（`{"on":true|false}` 或 `{"toggle":true}`） |
| `POST /v1/login.get` | 返回已登记账号快照；未登录或身份不匹配时返回 403 |
| `POST /v1/{resource}.{method}` | 标准方法参数校验与 native 后端分发（见下） |
| `POST /v1/upload.create` | 标准 multipart 上传，落盘到 `files/satori-wx-tmp/`，返回 `internal:wechat/<user>/_tmp/<name>`（5 分钟有效），经 `/v1/proxy` 回读 |
| `GET /v1/proxy/{url}` | 资源代理：`internal:` 链接按登录号解析并回文件；未登记 http(s) 前缀 403；非法 URL 400；未知登录 404 |
| `POST /v1/internal/capabilities` | 报告 `send` 状态块与 `unsupported` |
| `GET /v1/events` | WebSocket upgrade；10 秒内 IDENTIFY；READY、登录事件与 PING/PONG |

`features` 的唯一来源是 `native/wx_capabilities.cpp`。已实现方法：

- 读侧 13：`message.get/list`、`user.get`、`friend.list`、`guild.get/list`、`guild.member.get/list`、`guild.role.list`、`guild.member.role.list`、`channel.get/list`、`user.channel.create`
- 资源 1：`upload.create`
- 写侧 6（`send=on`）：`message.create`（纯文本）、`message.delete`、`channel.delete`（退群）、`guild.member.kick`、`guild.member.role.set/unset`
- 账号 1：`login.get`

其余标准方法返回 404。WebSocket 按 Satori 的 `op=3` + `body.token` 鉴权，成功后 `op=4`，`op=1` 心跳回复 `op=2`，每 10 秒发送一次 PING，连续 30 秒无响应则断开；支持掩码、文本分片、控制帧交错、TCP 分包/合包与关闭握手，64 条有界历史，登录事件不参与回放，窗口外序号用 4009 拒绝恢复。资源限制：8 个并发连接、8 KiB HTTP 头、16 KiB 请求体/WS 消息、有界发送缓冲；HTTP 每次响应后关闭连接，暂不支持 TLS、chunked 请求体或真实微信消息事件。

## 限制 / 风险

- 发送只有纯文本。`content` 先拍平（丢弃 `<quote>`/`<at>`/`<emoji>`，`<br/>` 变换行）；拍平后为空且只含 `<img>` 等媒体元素时返回 400 `media_unsupported`。图片/语音/视频/文件均未实现，原因为 App 发新图要跑 Kotlin 协程、收尾回调 native 交不出来（见 [发送各类消息](docs/wechat-send-types.md)）。
- 微信无此概念的方法列入 `internal/capabilities.unsupported`：`message.update`、`channel.create`、`channel.mute`、`guild.member.mute`、`reaction.*`、`guild.role.create/update/delete`。
- 微信被系统冻结时回环端口握手成功但无响应，客户端挂起至超时；root 侧 `wxguard` 默认 ARMED 负责解冻（见 [常驻通知与保活](docs/keepalive.md)）。
- 只在 arm64 上构建与运行。
- 写操作默认关闭，成功表示「已交给微信派发」而非投递确认；破坏性动作不伪造成功，风控责任在调用方。

## 必要链接

- [ZygiskNext](https://github.com/LSPosed/ZygiskNext)
- [标准 Zygisk 模块接口](https://github.com/topjohnwu/zygisk-module-sample)
- [Satori HTTP API](https://satori.chat/zh-CN/protocol/api.html)、[事件](https://satori.chat/zh-CN/protocol/events.html)、[元信息](https://satori.chat/zh-CN/advanced/meta.html)
- [知言应用设计规范](docs/app-design.md)
- [只读账号身份说明](docs/wechat-account.md)
- [消息后端设计（native、低特征）](docs/wechat-store.md)
- [微信消息发送路径（反射）](docs/wechat-send.md)
- [发送各类消息（反射，含图片为何没做）](docs/wechat-send-types.md)
- [微信群管理写操作（反射）](docs/wechat-room.md)
- [常驻通知与保活（wxguard）](docs/keepalive.md)
- [v0.9.2 协议覆盖矩阵](docs/satori-conformance.md)
- [研究记录与已知边界](docs/native-server.md)
