# 事件

除了 `login-*`（见 [只读账号身份](wechat-account.md)），后端产生这些事件。`internal/capabilities` 的 `event_types` 列出它们，方便客户端区分「没发生」和「不会有」。

| 事件 | 必带资源 | 来源 |
| --- | --- | --- |
| `message-created` | `channel` `message` `user`（群里另有 `guild` `member`） | `message` 表的新行，见 [消息内容](wechat-content.md) |
| `message-deleted` | `channel` `message` `user`（作者不明时没有 `user`） | 撤回：微信原地改写那一行 |
| `guild-member-added` `guild-member-removed` | `guild` `member` `user` | `chatroom.memberlist` 的差异 |
| `guild-added` `guild-removed` | `guild` | 自己入群 / 退群、被移出、群被解散 |
| `friend-added` `friend-removed` | `user` `friend` | `rcontact` 里好友集合的差异 |

`message-created` 是毫秒级的：`wx_watch` 用 inotify 盯着账号库所在目录，微信一写库就立刻读新行（机制见[消息内容](wechat-content.md#轮询)），从微信落库到事件发出通常不到 50ms。下面几类没有「新行」可等，走扫描器，延迟以秒计。

微信不宣告这些变化，它们只体现为库里状态的改动。`native/wx_events.{h,cpp}` 的扫描器每 3 秒对比一次快照。有两条规矩：

1. **启动时只做快照，不发事件。** 已经存在的群、好友、已撤回的消息都不算新闻。
2. **差异要连续两轮看到才算数。** 同步时微信会分几步改写这些行，中间状态会闪一下又变回去（成员表先清空再写回、联系人类型来回翻）；一轮就发会刷出一串假的入群退群。代价是最少 3 秒的延迟。

发不出去的事件（总线满了）留在一个 128 条的重试队列里，按序补发；队列也满了才丢并计数（`internal/status` 的 `events.dropped`）。

## 撤回

微信把被撤回的那一行**原地**改成提示：`msgId` 与 `createTime` 不变，`type` 变成 `268445456`（"某人 撤回了一条消息"）或 `285222674`（"你撤回了一条消息"），`content` 换成提示文字（样本里两种行的 `isSend` 都是 0，不能靠它认出是不是自己撤回的）。所以没有新行可轮询，扫描器每轮查最近 30 分钟内出现的撤回类型行，见过的 id 记在一个 512 项的环里，新的才发 `message-deleted`。

`message.id` 就是当初 `message-created` 用的本地 id。作者取自轮询时记住的最近 512 条消息的发送者（包括自己发的）；没记住的：私聊回退到 `talker`，群里不猜、事件不带 `user`。所以模块启动之前的消息被撤回，群里的事件没有作者。

## 群成员

`chatroom` 表每行有 `memberlist`（`;` 分隔的 wxid）。扫描器用一个便宜的指纹（`modifytime`、`memberCount`、`length(memberlist)`）判断哪个群变过，只对变过的群读完整名单，再和快照做集合差：

- 多出来的 id：`guild-member-added`；少掉的：`guild-member-removed`。名单只是换了顺序不算变化。
- 多出来或少掉的是自己：`guild-added` / `guild-removed`（不再另发成员事件）。
- 出现了一行新的群且名单里有自己：`guild-added`；一行消失且原名单里有自己：`guild-removed`。没有自己的群出现或消失不发任何事件。

`member.user` 与 `user` 来自 `rcontact`（备注、昵称、头像），联系人表里没有这个人就只有 `id`。事件不带 `operator`：微信没有把「谁拉的人 / 谁踢的人」放进这张表。

## 好友

`StoreFriendIds` 读 `(type & 3) = 3` 且非公众号（`gh_` 开头）非群的联系人，每 4 轮（约 12 秒）读一次，做集合差；要两次读到才算数，所以好友事件的延迟是 12–24 秒。

## 没有做的事件

- `friend-request`、`guild-request`、`guild-member-request`：好友申请在 `fmessage_conversation` / `fmessage_msginfo`，这台机器上两张表都是空的，没有真实样本，不凭想象写解析。做的时候要一起解决 `friend.approve`（微信自己的 `verifyuser` 场景需要申请里的 ticket）。
- `message-updated`：微信不能编辑消息。
- `reaction-*`：微信没有表态。
- `guild-updated`、`guild-member-updated`：群改名、昵称变化。库里能看到，但先要决定哪些字段算「更新」，暂不发。

## 自己发的消息：号主手打还是模块发的

微信的库里只有一个 `isSend`，号主在微信里手打的和模块 `message.create` 发的都是 1，`message-created` 里作者都是本账号。消费端需要分开（例如号主贴的链接要处理，机器人自己发出去又读回来的那份要忽略），所以模块自己记账：

- `message.create` 处理某个会话期间，以及结束后 5 秒内，该会话里本账号的行算模块发的（轮询可能被微信冻结拖后，宽限期兜这种情况）；
- 它返回过的本地消息 id（最近 64 个）算模块发的，与时间无关。

不属于这两类的 `isSend=1` 行，事件顶层带 `"satori_wx": {"manual_self": true}`（形状同 satori-qq 的 `satori_qq.manual_self`）。模块发的、别人发的都不带。多设备登录时另一台设备上发的也算「号主手发」。
