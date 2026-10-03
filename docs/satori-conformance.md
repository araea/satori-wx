# Satori v1 协议层验收

本实现端提供 native 协议服务端、native 后端接口、只读账号身份与消息库适配层、内置资源路由（`upload.create` 与 `/v1/proxy`），以及一组反射写操作（消息发送 / 撤回与群管理，没有开关）。

读侧 13 个方法已实现，`upload.create` 1 个，写侧 6 个（`message.create/delete`、`channel.delete`、`guild.member.kick`、`guild.member.role.set/unset`），`login.get` 1 个。11 个方法微信无法表达（见下），其余为尚未实现的写操作。下表中的「完成」指协议层及带独立测试后端 / 账号适配层的验收，不代表微信支持或已适配所有 API。

| 标准项目 | 实现 / 验证 |
| --- | --- |
| HTTP RPC、Bearer、Satori-Platform / Satori-User-ID | 完成；401 缺令牌、403 令牌错误或账号不存在、405 方法错误 |
| 错误体 | 每个非 2xx 响应都是 `{"code": "<机器可读的短名>", "message": "<给人看的>"}`（发送失败另带 `rejected`），与 satori-qq 同形，客户端按 `code` 判断。常用：`missing_token`（401）、`invalid_token`（403）、`login_not_found`（403 / 404）、`missing_login_headers`（400）、`invalid_request`（400）、`unsupported_method`（404，微信没有这个能力）、`not_found`（404）、`login_offline`（503）、`payload_too_large`（413）、`backend_not_implemented`（501）；发送与群管理的失败码见 [发送各类消息](wechat-send-types.md) |
| 标准方法目录 | 37 个方法；与上游 protocol/src/index.ts 的 Methods 对照 |
| JSON 参数 | 必填、可选、字符串、对象、布尔、非负整数、分页枚举验证 |
| 方法可用性 | 登录快照 features 控制；不支持返回 404（在参数校验之前判定，不支持的方法缺参也回 404 而非 400），声明支持但无 handler 返回 501，离线返回 503 |
| login.get / meta / READY | 同一份登录快照；登录身份由只读偏好解析得到，无账号时为空；`features` 列出全部实现的方法（含 `login.get`）与平台特性 `guild.plain`（微信群就是它唯一的频道） |
| message.create / update 的 content | 保留 Satori 标记字符串；提供 native 文本转义 helper，不把标记当 HTML 执行 |
| message.create（发送） | 反射调微信自己的发送管线。内容先按 `<message>` 容器 / `<message/>` 分隔符切成多条，各自走下面的流水线，回执按序合成一个 `Message[]`，空白段跳过。`<message forward>` 发成微信「聊天记录」卡片，一条卡算一个 part，限额与坏行整单 400 的口径见[发送各类消息](wechat-send-types.md)。`<message id="…" forward/>` 回 400 `forward_unsupported`。`<p>` 与相邻内容之间保证换行。每条再按 `<img>` 切成有序的文本 / 图片消息，返回 `Message[]`（官方客户端对结果调用 `.map()`）。文本走 `NetSceneSendMsg`，群里的 `<at>` 是真提及（`atuserlist`），图片走聊天界面自己的 `rj()` 管线。不限目标、不限速。成功表示已派发，不是投递确认（图片、卡片要等库里出现行才回 200） |
| message.create 的图片 | `src` 只认 `upload.create` 的 `internal:` 链接、`data:image/…;base64` 与 `base64://`；所有图片先解析、核对魔数，坏一张整条 400（`media_unresolved` / `media_unsupported` / `image_too_large` / `too_many_images`）；6 秒内没入库回 502 `image_unconfirmed`，不假报成功。见 [发送各类消息](wechat-send-types.md) |
| message.create 的视频 / 文件 / 音频 / 引用 | `<video>`：MP4 / MOV 且有视频轨发成视频气泡（`poster` 可选、时长取自文件），其它容器作为文件；`<file>`：文件名取 `title` → 上传名 → 按内容猜扩展名；`<audio>` 转成 SILK 发语音条（≤ 60 秒，回执里是 `<audio duration>`），超长 / 太短 / 读不出来的作为文件（回执里是 `<file>`）；`<quote id>` 是微信的引用回复（挂在第一段文字，找不到被引用消息退成普通文本）。每个媒体元素单独成条，`src` 只认 `internal:` / `data:` / `base64://`，先全部解析再发；发完等库里 status 离开「发送中」，失败回 502 `upload_failed`，慢上传照回 200。错误码：`media_unresolved` / `media_unsupported` / `media_too_large` / `too_many_media` / `video_unconfirmed` / `voice_unconfirmed` / `upload_failed` |
| message.delete（撤回） | 用 `ex0.k0.F0.k(talker,localId)` 取 MsgInfo，反射调 `com.tencent.mm.modelsimple.d1`（cgi revokemsg）；只撤回本账号消息 |
| channel.delete / guild.member.kick | 反射 `qn.p`（cgi delchatroommember），退群用 `[self]`，踢人用目标 wxid |
| guild.member.role.set / unset（可选） | 反射 `qn.b` / `qn.e`（cgi add/delchatroomadmin），经 `com.tencent.mm.modelbase.z2.d(o,null,false)` 走微信 Cgi 运行器；只变更 `admin` 角色 |
| upload.create | core 内置 SDK 默认实现，`multipart/form-data` 流式落盘（超过 16 KiB 的 body 不缓冲，上限 1 GiB）为 `internal:wechat/<user>/_tmp/<name>`（5 分钟、随机名、0600），落在微信数据目录下的 `files/satori-wx-tmp/`；链接由 `/v1/proxy` 回读（发送端不消费） |
| /v1/proxy/{url} | `internal:` 链接按登录号解析并流式回文件（`Range`、`HEAD`、百分号编码）；`_tmp` 是上传，`_msg` 是收到的消息媒体（验签失败与不存在都是 404）；未登记 http(s) 前缀 403；非法 400；未知登录 404；带 CORS，不需 Satori 登录头 |
| guild.member.get / list | 读 `chatroom` 的 memberlist + displayname（`、` 分隔）+ roomowner；`next` 是成员偏移；displayname 与 memberlist 数量不一致时忽略群昵称、回落到 rcontact |
| guild.role.list / guild.member.role.list | 合成角色：`owner`（群主）/ `admin`（管理员）/ `member`（成员）；管理员位读 `chatroom.roomdata` 的成员标志（`flag & 2048`），没有 roomdata 缓存时按普通成员算；非成员返回空列表；未知群返回 404 |
| message.list / message.get | 双向分页：`next` 令牌、`direction` before/after/around、`order` asc/desc，结果带 `prev` / `next`；系统提示与撤回标记不是消息；`message.get` 认本地 id 与服务端 id。见 [消息内容](wechat-content.md) |
| 消息内容 | 图片 / 语音 / 视频 / 表情 / 链接 / 文件 / 位置 / 名片 / 回复 / @ 解码成 Satori 元素；媒体是 HMAC 签名的 `internal:` 链接，由 `/v1/proxy` 流式回包 |
| 事件 | `message-created`（带 `guild` `member` 头像）、`message-deleted`、`guild-member-added|removed`、`guild-added|removed`、`friend-added|removed`；启动只快照，差异连续两轮才发；见 [事件](wechat-events.md) |
| user.channel.create | 返回该 wxid 的私聊频道（`type=1`） |
| `internal/capabilities` | 与 satori-qq 共用口径：`adapter`、`version`、`platform`、`standard_methods`（= `login.features` 去掉 `guild.plain`，即本实现端提供的标准方法）、`unsupported`、`event_types`、`message_elements`（`message.create` 接受的元素）、`limits`（`upload_bytes` 等）。另有 wx 自己的诊断块（`send`、`events`、`keepalive`） |
| 微信无法表达（`internal/capabilities.unsupported`） | `message.update`、`channel.create`、`channel.mute`、`guild.member.mute`、`reaction.create/delete/clear/list`、`guild.role.create/update/delete`。微信不能编辑消息、群内没有子频道、没有服务端禁言、无表态、无自定义角色 |
| 未实现的写操作 | 群改名、好友删除 / 审批、入群审批，卡点见[群管理写操作](wechat-room.md)。只有真实实现的方法才进 features，否则返回 404 |
| 分页 | params 的 next/direction/limit/order 与后端返回的 data/prev/next 原样传递 |
| upload.create | multipart/form-data，有界二进制零拷贝解析，字段名与返回 URL 映射由后端实现 |
| WebSocket | RFC 6455 握手、掩码、文本分片、控制帧交错、UTF-8、关闭、大小限制 |
| IDENTIFY / READY / PING / PONG | 完成；10 秒鉴权、30 秒心跳期限 |
| EVENT | native 生产者队列；服务端分配 sn / timestamp；多客户端广播；事件遵守资源提升（`message` 里不重复 `channel` `guild` `user` `member`，`member` 里不重复 `user`），消息时间是 `created_at`（毫秒） |
| 登录事件 | added / updated / removed 更新快照；非登录事件只带 sn/platform/user 身份 |
| META | op=5，当前仅广播空 proxy_urls；不在 META 中传 logins |
| 会话恢复 | 64 条有界历史、按客户端游标发送；登录事件不回放；游标不在窗口内（旧进程、滚出窗口、未来序号）不拒绝：回 READY 后从当前推送，READY 带 `satori_wx.session_id` 供客户端判断服务端是否重启 |
| 背压 | 32 条生产队列；满时 Publish 返回 false；单客户端发送缓冲有界；回放超出缓存时关闭 |
| WebHook（标准可选） | 完成：`/v1/meta/webhook.create` / `webhook.delete`，EVENT / META 以 `Satori-Opcode` 推送，可选 `Authorization`；仅 http，无 TLS |
| 资源代理 | 未实现，proxy_urls 保持空，相关路由 404 |

序号以进程启动时的 Unix 毫秒为基线，在进程内递增，并限制在 JSON / JavaScript 安全整数范围（毫秒而非微秒：16 位的数在 cJSON 里可能被印成 `1.79e+15`，整数解析器读不了）。正常重启后旧进程序号落在新窗口外，此时按上一行处理，不宣称跨进程持久回放。显式 sn=0 仅在当前进程历史未丢弃非登录事件时从缓存起点恢复，否则同样从当前推送。

HTTP 请求体 / WS 消息最大 16 KiB（`message.create` 是 16 MiB 且先验令牌再缓冲，`upload.create` 流式收到 1 GiB、同样先验令牌与登录）、HTTP 头 8 KiB、单事件 128 KiB、最多 16 个登录快照，单连接 HTTP 响应后关闭。当前 HTTP profile 不支持 chunked 请求体、TLS 或 WebSocket 压缩，支持 `Expect: 100-continue`。这些限制是实现约束，并非 Satori 标准规定的上限。

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

`internal/status`、`internal/capabilities` 是本项目诊断扩展，不冒充标准 API。标准方法目录不是登录账号的 features，只有后端明确实现的能力才能列入 features。

## 测试

`./tests/run.sh`：22 项 HTTP / WebSocket socket 测试、18 项协议测试（含大上传、流式回包、`Range`、`Expect`、停滞读者、128 KiB 事件与回放）、账号解析 / 状态机 / Hub 集成测试、账号端到端适配测试、能力 / 配置测试、探针并发 / 资源 / 权限测试，以及下列专项。

- 协议测试遍历 37 个方法，并验证上传二进制、标记保留、分页、广播、登录状态、元信息、历史窗口淘汰以及超过发送缓冲容量的分段回放。
- 内容测试（`tests/content_test.cpp`）覆盖发送侧的标记拍平：`<at>` 变成 `@名` + U+2005 与提及名单、`<a href>` 保留目标、`<img src>` 的抽取与位置（属性顺序、两种引号、属性值里的 `>`、空 `src`、未闭合标签、只认 `img`、数量上限）、base64。
- 解码测试（`tests/message_test.cpp`）用照真机行造的样本（头、CDATA、怪类型号）覆盖每种消息类型、@ 的各种边角（名字不比对、多出的 id、手敲的 `@`）、回复、系统行与撤回标记。
- media / XML 测试（`tests/media_test.cpp`）：MD5、SHA-256、HMAC 的已知答案（含真机语音目录的 md5）、链接签名绑定登录 / 类型 / id、换 token 作废旧链接、XML 扫描器只认直接子元素。
- store 测试用真实列布局的夹具库：轮询（突发超过一批、总线满、超大事件）、`message.list` 的五种翻页、回复解析（`MsgQuote` 与 svrid 回退与内联）、头像、媒体文件解析（含 wxgf 跳过、路径穿越被拒、文件路径白名单）、代理路由验签。
- events 测试（`tests/events_test.cpp`）：撤回、群成员、自己入退群、好友增减、启动静默、两轮确认、闪变不算、重试队列保序。
- backend 测试把真实的 `wx_backend.cpp` 接进来说话（store / 群管理 / 保活用桩），覆盖文本拍平、群里 @、每一种被拒的图片请求、多图与文字混排先验后发。
- 视频 / 文件 / 引用测试（`tests/backend_test.cpp`，发送器换成记录调用的桩）：每种路由（MP4 → 视频、MKV → 文件、mp3 → 文件、有 / 无 title 的文件、内联 base64）、封面取舍、顺序、失败状态、坏一个整条不发、引用的各种边角。`tests/mp4_test.cpp` 用 ffmpeg 生成的真容器。`tests/upload_stream_test.cpp` 把同一个 body 按 15 种分块喂进流式解析器，逐字节比对落盘结果。
- tempstore 测试覆盖 `internal:` 链接的解析（外链、别的平台、`_tmp` 之外、路径穿越、未知名字、手工放进去的文件、输出缓冲太小）。
- 账号端到端测试用夹具偏好文件驱动真实适配层，验证 meta / login.get / READY 的一致快照、离线状态与账号切换。WebHook 测试用本地接收端验证 `Satori-Opcode`、`Authorization` 与信号体，以及登记上限与注销。

`tools/conformance.py` 是对着一个在跑的服务端做的黑盒探针（只读，不发任何聊天消息）：状态码表、`features` 与 404 的关系、`login.get` / `meta` / READY 的一致、分页信封、`message.list` 的方向与令牌、`upload.create` 到 `/v1/proxy` 的往返、代理路由的 400 / 403 / 404、WebSocket 的 PING 与旧 `sn` 恢复、驼峰键。`--listen` 时还核对实时事件的形状（含资源提升）。

```sh
python3 tools/conformance.py --base http://127.0.0.1:5601 --token "$(su -c 'sed -n s/^token=//p /data/adb/modules/satori_wx/satori-wx.conf')"
```

JNI 那部分（图片管线、@ 的 Object 重载）主机上跑不了，只在真机验，清单见 [HANDOFF](HANDOFF.md)。

测试后端和事件输入管道仅编译到 `build/tests/server`，不在 Zygisk 模块内。真机检查器 `build/satori-wx-check` 只读 meta / status 和 WebSocket 信令，不发送微信消息。

## 参考

- [Satori API](https://satori.chat/zh-CN/protocol/api.html)
- [Satori 事件和会话恢复](https://satori.chat/zh-CN/protocol/events.html)
- [消息资源](https://satori.chat/zh-CN/resources/message.html)
- [上游协议方法定义](https://github.com/satorijs/satori/blob/main/packages/protocol/src/index.ts)
