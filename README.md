# 知言（satori-wx）

微信 `com.tencent.mm` 的 Zygisk 模块。v0.6.7 在 **Satori v1 服务端**
（C++ + POSIX socket，无 DEX、Java 助手、APK、ArtMethod 偏移或 hook 引擎）之上，
加入**只读的微信账号身份 / 消息库适配层**，以及一个**默认关闭的反射消息发送与撤回**。

账号身份来自微信自己持久化的 SharedPreferences（同 uid 直接读文件，不 hook、不改写、
不访问数据库）。消息读取用微信自己的 `libWCDB.so` 只读打开 `EnMicroMsg.db`；
数据库密钥由模块自身在 `RegisterNatives` 边界捕获（见 `native/wx_key.cpp`），
不再需要单独的探针模块。
**发送是可选项**：`send=on` 后，实现端用**宿主 ClassLoader 反射调用微信自己的
`v51.r0`（NetSceneSendMsg）与网络派发器**，由微信完成入库、加密、发送；不发任何原始包、
不 hook、不加载 dex。默认 `send=off`，且只对 `send_allow` 白名单内的会话开放。
已登录时 `READY` / `/v1/meta` 会带真实 `logins`；账号快照的 `features` 只声明后端
真正实现的方法（见下表）；未实现的方法返回 404，不会伪造成功。
旧 v0.2.x 的 `native/wx.cpp`、`native/jni_helpers.h`、`src/`、`AndroidManifest.xml` 和 `libs/`
保留作研究资料，不参与任何当前构建。版本来源改为根目录 `module.prop`。

## 构建与验证

在 arm64 Termux 中，需要 clang、Python 3、readelf、patchelf、zip；不需要 JDK、Android SDK 或 D8。

```sh
./build.sh               # 服务端 ZIP + 三个诊断工具
./tests/run.sh           # HTTP/WebSocket socket 测试 + 账号解析/事件测试
```

产物：

- `build/satori-wx-server-v0.6.7.zip`，模块 ID `satori_wx`。
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
# send=on 时才需要白名单；分号分隔，只允许这些会话（wxid 或 <数字>@chatroom）
# send_allow=wxid_xxxxxxxx;1234567890@chatroom
```

配置权限为 `0600`；缺失、过长、重复字段或无效 token 时服务端不启动。`send=on` 但
`send_allow` 为空时，服务端照常启动，但**任何发送目标都会被拒绝**（记 logcat 警告）。
配置在 `preAppSpecialize` 读取，文件描述符立即关闭；服务在 `postAppSpecialize` 启动。
只在精确匹配的微信主进程内运行。非目标进程和 system_server 请求卸载模块。
服务端只监听 **127.0.0.1**，随微信主进程结束而退出；配置更改在下一次进程启动生效。

也可以用 **知言应用**（[`app/`](app/README.md)）管理：查看连接链路、编辑发送开关与白名单
（从服务读群与联系人挑选）、端口与令牌，并一键重新启动微信让配置生效。应用需要 Root 授权。

Satori 客户端填写：

- API：`http://127.0.0.1:5601/v1`
- Events：`ws://127.0.0.1:5601/v1/events`
- Token：配置文件的 `token` 值

可用接口：

| 接口 | 当前行为 |
| --- | --- |
| `POST /v1/meta` | 已登录时返回只读身份快照 `{"logins":[...],"proxy_urls":[]}`；无账号时 `logins` 为空 |
| `POST /v1/meta/webhook.create` / `webhook.delete` | 注册/注销 WebHook（`url` 必填、`token` 可选）；标准可选功能 |
| `POST /v1/internal/status` | 实验版版本、native 状态、backend unavailable；项目自定义诊断接口 |
| `POST /v1/login.get` | 返回已登记账号快照；未登录或身份不匹配时返回 403 |
| `POST /v1/{resource}.{method}` | 37 个标准方法的参数校验及 native 后端分发；已实现读侧 `message.get/list`、`user.get`、`friend.list`、`guild.get/list`、`guild.member.get/list`、`guild.role.list`、`guild.member.role.list`、`channel.get/list`、`user.channel.create`；`send=on` 时另实现 `message.create`（反射发送，见下）；其余返回 404 |
| `POST /v1/internal/capabilities` | 除标准方法目录外，报告 `send` 状态块与 `unsupported`（微信无法表达的方法：`message.update`、`reaction.*`、`guild.role.create/update/delete`） |
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

## 只读账号身份

适配层在微信主进程内以微信自己的 uid 读取（只读、不解析数据库）:

- `shared_prefs/com.tencent.mm_preferences.xml`：`login_weixin_username`（wxid）、`last_login_uin`、
  `isLogin`、`last_login_alias`、`last_login_nick_name`、`last_login_bind_mobile`、`login_user_name`。
- `shared_prefs/auth_info_key_prefs.xml`：`_auth_uin`，作为 uin 缺失时的回退。

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

`send=on` 且目标在 `send_allow` 内时，`message.create` 通过**反射调用微信自己的发送链路**
发出纯文本：`v51.r0`（NetSceneSendMsg）构造器负责入库，`doScene` 交给微信的 mars 传输层，
加密/序号/重发都是微信自己的行为。实现端不 hook、不改写代码、不加载 dex、不发原始封包。
完整逆向结论见 [微信消息发送路径](docs/wechat-send.md)。

- 只支持纯文本；`channel_id` 是 wxid（私聊）或 `<数字>@chatroom`（群）。
- 返回的 `Message.id` 是微信本地消息 id；**成功表示「场景已交给微信派发」，不是投递确认**。
- 额外限速：1.5 秒最小间隔、每分钟 10 条；解析失败或派发返回负值时返回 502 并写明原因。
- **2026-09-27 真机验证通过**（v0.6.1 向「文件传输助手」实发成功）。
- 诊断：`POST /v1/internal/status` 与 `/v1/internal/capabilities` 的响应里带 `send` 块
  （`enabled`/`ready`/`resolved`/`dispatcher`/`allowed_any`/`allow`、`sent`/`failed`/`rejected`/`recalled`
  计数、上次尝试的目标/结果/`netId`/本地 id）；`resolved`/`dispatcher` 由登录后的预热线程
  主动探测（不发消息），`message.create` 被策略拒绝时 502 体带 `rejected: true`。
- 这是本模块里风控最敏感的能力，请保持默认关闭；`features` 与 `internal/capabilities`
  会如实反映当前是否可用。

## 数据库密钥捕获

微信在运行时派生出 `EnMicroMsg.db` 的 SQLCipher 密钥，磁盘上没有；要读消息库只能观察
`com.tencent.wcdb.core.Database.setCipherKey` / `nativeSetKey`。模块在
`env->functions->RegisterNatives` 这个可写数据表上替换这两个 native 的 `fnPtr`，
原调用结果和异常原样保留，不替换微信方法本身、不改代码段。捕获到的 spec 写入
`<应用数据目录>/files/satori-wx/key.log`（0600），`wx_live` 只读打开库并轮询新消息。
观察器常驻，账号切换后新密钥会覆盖写入。这条链路以前是一个独立探针模块，v0.6.7 起并回主模块。

## 参考与下一步

- [ZygiskNext](https://github.com/LSPosed/ZygiskNext)：运行环境与公开接口参考。
- [标准 Zygisk 模块接口](https://github.com/topjohnwu/zygisk-module-sample)：当前沿用本地 API v4 头。
- [Satori HTTP API](https://satori.chat/zh-CN/protocol/api.html)、[事件](https://satori.chat/zh-CN/protocol/events.html)、[元信息](https://satori.chat/zh-CN/advanced/meta.html)。
- [**交接文档（下一位接手先读）**](docs/HANDOFF.md)。
- [知言应用设计规范](docs/app-design.md)。
- [研究记录与已知边界](docs/native-server.md)。
- [只读账号身份说明](docs/wechat-account.md)。
- [消息后端设计（native、低特征）](docs/wechat-store.md)。
- [微信消息发送路径（反射，v0.6.7）](docs/wechat-send.md)。
- [v0.6.7 协议覆盖矩阵](docs/satori-conformance.md)。
- [v0.4.0 安装与重启验收记录](docs/deployment-v0.4.0.md)。

下一步：写操作（群改名/退群/禁言、踢人/管理员、好友删除与审批、上传）需要逐个逆向微信
内部接口，每个都像发送/撤回那样是一次独立研究；读侧、发送与撤回已经齐了。
媒体发送（图片/语音/视频/文件）的接口位置见 [docs/wechat-send-types.md](docs/wechat-send-types.md)。
