# 消息内容、媒体链接与历史分页

读侧把 `message` 表的一行转成 Satori 的 `Message`：作者、内容标记串、回复、时间。`native/wx_message.{h,cpp}` 只做这一步映射，不碰数据库和文件；要查库的部分（被引用消息的本地 id、昵称、头像）由 `native/wx_store.cpp` 补。

微信 8.0.78，样本来自真机库。

## 行格式

| 列 | 用途 |
| --- | --- |
| `msgId` | 本地 id，等于 rowid；Satori `message.id` |
| `msgSvrId` | 服务端 id；`message.get` 也认它 |
| `type` | 基础类型，见下 |
| `isSend` | 1 = 本账号发出 |
| `createTime` | 毫秒；Satori `timestamp` |
| `talker` | 会话：`wxid_…` 私聊，`<数字>@chatroom` 群 |
| `content` | 群里别人的消息带 `wxid:\n` 头；表情与视频在任何会话里都带头 |
| `imgPath` | 媒体文件名：图片 `THUMBNAIL_DIRPATH://th_<md5>`，语音 `amr_…`，视频时间戳名 |
| `lvbuffer` | 9 字节头之后是 `<msgsource>…</msgsource>`；`<atuserlist>` 是 @ 名单 |

`type` 的低 16 位才是基础类型：`49`（富文本）带高位变出一批数字（285212721、822083633、436207665…），`10000` 与 `10002`（系统提示、撤回标记）同理。所以判定用 `type & 0xFFFF`，并以内容为准：`<sysmsg` 开头的即使类型看着像富文本，也是系统消息。

头的格式：

- 文本、图片、语音、富文本：群里别人的消息是 `wxid:\n<正文>`；语音是 `wxid: <xml>`（冒号加空格）。自己的消息、私聊没有头。
- 表情：`wxid:0:1:<md5>:wxid*#*\n<xml>`，视频：`wxid:<秒数>:<标志>\n`（整条没有别的内容）。

## 映射

| 微信 | Satori `content` |
| --- | --- |
| 文本（1） | 转义后的文本；群里的 `@昵称` + U+2005 换成 `<at id name/>`（见下） |
| 图片（3） | `<img src="internal:wechat/<自己>/_msg/image/<msgId>/<签名>"/>` |
| 语音（34） | `<audio src="…/voice/…" duration="4.746"/>`，时长取 `voicelength`（毫秒）折成秒 |
| 视频（43） | `<video src="…/video/…" poster="…/videothumb/…" duration="7"/>`，秒数取头里的第二段 |
| 表情（47） | `<img src="<cdnurl>"/>`，`cdnurl` 里的 `*#*` 还原成 `:`；没有 cdnurl 才走签名链接 `emoji` |
| 位置（48） | `[位置] 名称（地址） 纬度,经度` |
| 名片（42） | `[名片] 昵称 (wxid)` |
| 链接、小程序、公众号推送（49，其余子类型） | `<a href="…">标题</a>`，摘要另起一行；只认 `http(s)` 链接 |
| 文件（49 / type 6） | `<file src="…/file/…" title="文件名"/>` |
| 回复（49 / type 57） | `<quote id="被引用的本地 id"/>回复正文`，另填 `message.quote` |
| 转账、红包等（49 / 2000、2001…） | `[微信红包] 摘要`：里面的支付链接不外露 |
| 其余（聊天记录、群公告、接龙…） | `[标题] 摘要` |
| 系统提示（10000）、`<sysmsg>`、拍一拍、通话记录、邮件推送 | 不是消息：不发 `message-created`，`message.list/get` 里也没有 |
| 撤回标记（268445456、285222674、10002） | 不是消息；改发 `message-deleted`，见 [事件](wechat-events.md) |

没有登录（`self_id` 未知）时媒体退回文字占位（`[图片]`）：签名链接要绑定登录，没法验证的链接不如不给。

### @ 提及

微信的选人 @ 在文本里写 `@昵称` 再接一个 U+2005，名单放在 `msgsource` 的 `<atuserlist>`（逗号分隔的 wxid，`notify@all` 是 @所有人）。解码时按出现顺序把第 k 个「@…U+2005」配给第 k 个 id，**不比对名字**——群昵称、备注、当前昵称三者常常对不上。名单里多出来的 id 放在最前面，不丢；手敲的 `@` 没有 U+2005，不算提及；私聊里没有提及。

### 回复

`<title>` 是回复正文，`<refermsg>` 描述被引用的那条。被引用消息的本地 id 先查 `MsgQuote` 表（微信自己的记录，行可能比消息晚一点到），没有就拿 `<svrid>` 去 `message.msgSvrId` 里找。找到：`<quote id="…"/>`；找不到（历史已清理）：把原文内联成 `<quote><author user-id nickname/>原文</quote>`，仍然读得通，只是没有 `id`。

## 媒体链接

`/v1/proxy` 按 Satori 约定不要 Authorization（好让 `<img>` 直接引用），而 `msgId` 是小整数。链接若不带签名，本机任何应用从 1 数到 N 就能读完用户收到的所有照片。所以每个媒体链接都是：

```
internal:wechat/<login>/_msg/<kind>/<msgId>/<签名>
```

签名是 `HMAC-SHA256(key, login \n kind \n id)` 的前 8 字节（16 位十六进制），`key` 由配置里的 token 派生。绑定登录、类型与 id，所以换个 id 或类型就不验证。token 是密钥而不是进程级随机数：微信重启后旧链接仍然有效，换 token 就全部作废。验证在碰数据库和文件系统之前。`native/media.{h,cpp}`、`native/wx_media.cpp`。

`kind`：

| kind | 文件 |
| --- | --- |
| `image` | `image2/<md5前2>/<md5 第 3–4 位>/`：依次试 `<md5>`、`<md5>.jpg`、`th_<md5>hd`、`th_<md5>`；魔数是 JPEG/PNG/GIF/WebP 才收 |
| `voice` | `voice2/<md5(文件名)前2>/<第 3–4 位>/msg_<文件名>.amr`，SILK（`\x02#!SILK_V3`），类型 `audio/silk`，不转码 |
| `video`、`videothumb` | `video/<imgPath>.mp4`、`.jpg` |
| `emoji` | `emoji/<md5>`，md5 取自 XML |
| `file` | `appattach.fileFullPath`，只认微信私有目录或它在共享存储上的目录；类型按扩展名 |

`image` 的原图常是微信私有的 `wxgf` 容器，客户端打不开，会被跳过，回落到 `th_…hd` 与 `th_…` 两张缩略图。原图要等用户在微信里点开才会下载；模块不去触发下载。所以拿到的往往是缩略图（几十 KB，仍是 JPEG）。

`/v1/proxy` 是流式回包：按 64 KiB 从文件读，socket 排空了才读下一块，支持 `Range` 与 `HEAD`，客户端把整条链接做百分号编码也能解析。

## 事件里的资源

`message-created` 按 Satori 的资源提升带上：

```json
{"type":"message-created","login":{"sn":1},"timestamp":1790000000000,
 "channel":{"id":"…@chatroom","type":0,"name":"群名"},
 "guild":{"id":"…@chatroom","name":"群名"},
 "user":{"id":"wxid_…","name":"备注","nick":"昵称","avatar":"https://wx.qlogo.cn/…"},
 "member":{"nick":"群内昵称"},
 "message":{"id":"4099","content":"…","timestamp":1790000000000,"channel":{…},"user":{…},"guild":{…},"member":{…},"quote":{…}}}
```

私聊没有 `guild` 与 `member`。头像取自 `img_flag`（微信缓存的公网头像地址，`reserved2` 大图，`reserved1` 小图），没有就不给。

## 轮询

`wx_live` 读一批（50 行）`rowid > 水位` 的新行，一批读满就不睡直接再读。什么时候读：`wx_watch` 用 inotify 盯着账号库所在目录，微信一写 `EnMicroMsg.db` / `-wal` 就醒（毫秒级），醒来读一次，再在 30ms、150ms 各补读一次——通知可能比「提交对第二个连接可见」早那么一点，而单次写入之后不会再有通知；没有通知时 1 秒兜底读一次，inotify 不可用退回 250ms 一次。此前是每 2 秒（v0.10.0 是 1 秒）读一次，一条消息平均白等半个到一个周期。水位只前进到**已处理的最后一行**：

- 总线满了（队列 32 条），被拒的那一行留到下一轮，顺序不变，不丢。
- 读库出错，水位不动，下一轮同样的行再读。
- 一条事件序列化后达到 128 KiB 上限：计数（`internal/status` 的 `events.skipped`）并越过，不卡死后面的消息。
- 不是消息的行（系统提示、通话记录）直接越过。

此前水位是「读完后取库里当前最大 rowid」：一次突发超过一批、或总线满了，超出的行就被永久跳过。

## `message.list`

按 Satori 的双向分页：

| 参数 | 含义 |
| --- | --- |
| `next` | 上一页返回的令牌（消息的 rowid）；空表示从最新一条开始，此时方向只能是 `before` |
| `direction` | `before` 比令牌更早、`after` 更晚、`around` 令牌附近（含它） |
| `order` | `asc`（默认）或 `desc`，是这一页内的顺序，与方向无关 |
| `limit` | 1–50，缺省 50 |

一页按 `(createTime, msgId)` 排序，走 `(talker, createTime)` 索引，不需要对整个会话排序（按 rowid 排会让 SQLite 把一个大群的所有行收集起来再排一遍）；令牌是消息的 `msgId`，取页时再查它的时间。同一毫秒的消息按 `msgId` 定先后，掉线补同步来的旧消息（`msgId` 大、时间早）按时间排在它该在的位置。令牌指向的行已经不存在（被清理）时回 400。

结果 `{data, prev?, next?}`：`prev` 存在表示还有更早的消息，`next` 表示还有更晚的。往回翻就拿 `prev` 当下一次的 `next`、方向 `before`。系统提示与撤回标记不计入一页的数量，所以一页不会因为夹了一条提示而不满。

`message.get` 的 `message_id` 接受本地 id 或服务端 id（`msgSvrId`），两者都对得上时本地 id 优先。

## 好友与头像

`friend.list` 的判定是 `(type & 3) = 3`：`type` 是位标志，`3` 是普通好友，星标（`67`）、`2051`、`2115`、`65539` 这些带额外位的也是好友。此前写成 `type = 3`，这部分好友被漏掉。`friend.list` 与 `guild.list` 的续页游标原先按「小于游标」取下一页，升序列表会反复返回同一页，已改为「大于」。
