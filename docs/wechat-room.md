# 微信群管理写操作（反射）

和发送 / 撤回一样：只反射调用微信自己的请求构造类与网络队列，不 hook、不改代码、不加载 dex。写操作与发送一样没有开关；成功＝「场景已交给微信派发」，**不是服务端已生效**。

微信 8.0.78 / versionCode 671108664。离线工具见 `tools/dex*.py`，静态定位用 `dexfindstring.py` / `dexmethodsig.py` / `dexinvokes.py`，结构确认用 JADX。

## 两类基类

微信这个版本有两套请求基类：

| 基类 | 派发方式 | 例子 |
| --- | --- | --- |
| `com.tencent.mm.modelbase.m1` | `scene.doScene(dispatcher, new com.tencent.mm.network.y2())` | `qn.p`、`qn.o`、`qn.m`、`qn.q`、`v51.r0`、`p3` |
| `com.tencent.mm.modelbase.i` | 构造后 `com.tencent.mm.modelbase.z2.d(this.f, null, false)` 交给 Cgi 运行器 | `qn.b`、`qn.e`、`qn.d` |

`m1` 分支复用 `wx_send.cpp` 的 `Dispatcher()`（`com.tencent.mm.modelbase.r1.y.k()` 优先，`com.tencent.mm.network.a3.c()` 兜底）+ 空回调 `y2`。`i` 分支把请求对象（继承字段 `f`，类型 `Lcom/tencent/mm/modelbase/o;`）交给 `z2.d(o, callback, false)`；`z2` 内部走 `((gp0.n) z2.f59772b).f268046a.f268074b.h(scene, 0)`，与 App 完全一致。

## 已实现的场景

| Satori 方法 | 场景类 | cgi | 构造参数 |
| --- | --- | --- | --- |
| `guild.member.kick` | `qn.p` | `/cgi-bin/micromsg-bin/delchatroommember` | `(String chatroom, List members, int op)`，op=0 |
| `channel.delete`（退群） | `qn.p` | 同上 | members=`[self]`，op=0 |
| `guild.member.role.set` | `qn.b` | `/cgi-bin/micromsg-bin/addchatroomadmin` | `(String chatroom, LinkedList members)` |
| `guild.member.role.unset` | `qn.e` | `/cgi-bin/micromsg-bin/delchatroomadmin` | `(String chatroom, LinkedList members)` |

成员列表用 `java.util.LinkedList<String>`（`qn.p` 形参是 `List`，`qn.b/e` 是 `LinkedList`）；`qn.b/e` 构造器内部会 `y8.b1(list, ";")` 做一次规整。

角色：微信没有自定义角色，`guild.role.list` 合成 `owner`（群主）/ `admin`（管理员）/ `member`（成员）。只有 `admin` 可被 `guild.member.role.set/unset` 变更；`owner` / `member` 固定。

**读侧（v0.9.0 起）认管理员位**：位在 `chatroom.roomdata` 这个 protobuf 里。`ChatRoomData{ repeated ChatRoomMember member = 1 }`、`ChatRoomMember{ string userName = 1; ...; int32 flag = 3 }`，`flag & 2048` 即管理员（App 侧同样是判这个位）。`wx_store.cpp` 里有个只读的小 protobuf 遍历器（`RoomAdmin`），按 wxid 查这个位；没有 roomdata 缓存时（`roomdata` 为 NULL）按普通成员算，不猜。写操作本身仍是真发的；读侧反映的是微信刷新过 `chatroom` 行之后的状态。

真机确认：`21378418394@chatroom` 里 `flag=2049` 的正是两位「心理部负责人」，`38992867588@chatroom` 里 `flag=2049` 的也是群管理员。

## 没做的写操作与原因

| 方法 | 结论 |
| --- | --- |
| `channel.create` | 微信没有「群内子频道」，Satori 语义无法映射。**列入 `unsupported`** |
| `channel.mute` / `guild.member.mute` | 微信没有服务端全员 / 单人禁言（只有客户端「消息免打扰」）。**列入 `unsupported`** |
| `channel.update` | 群改名在可读 dex 里找不到 cgi；可能编译进 `libapp.so` |
| `friend.delete` | 可读 dex 里没有 `delcontact` 这个 cgi（只有 `delcontactlabel`，是标签场景），本地删 + 同步 |
| `friend.approve` | 好友申请审批走 `com.tencent.mm.pluginsdk.model.p3`（`verifyuser`），依赖申请消息里的 ticket |
| `guild.approve` / `guild.member.approve` | 入群审批用 `qn.d`（`approveaddchatroommember`，`(long,String,String,String,List)`），参数语义未确认 |

`channel.create` / `channel.mute` / `guild.member.mute` 是微信真的没有的能力，所以进 `internal/capabilities.unsupported`（客户端据此禁用按钮）；其余仍只是尚未实现，客户端得到 404。

## 安全边界

- 全部始终可用，都在 features 里。
- 只在精确匹配的微信主进程内运行；派发器、JavaVM、宿主 ClassLoader 都来自 App 自身。
- 不伪造成功：构造 / 派发失败会带原因返回 502，`rejected:true` 表示在派发前被开关拒绝。
- 破坏性动作（踢人、退群、设 / 撤管理员）由调用方负责；模块不做二次确认。
