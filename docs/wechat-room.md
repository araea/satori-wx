# 微信群管理写操作（反射，v0.8.0）

和发送/撤回一样：只反射调用微信自己的请求构造类与网络队列，不 hook、不改代码、不加载 dex。
写操作与发送共用 `send=on` 开关；成功＝「场景已交给微信派发」，**不是服务端已生效**。

微信 8.0.78 / versionCode 671108664，base.apk 在 `~/tmp/satori-wx/base.apk`。离线工具见
`tools/dex*.py`，静态定位用 `tools/dexfindstring.py` / `tools/dexmethodsig.py` / `tools/dexinvokes.py`，
结构确认用 JADX（`~/tools/jadx/bin/jadx --single-class <点分名> ...`）。

## 两类基类

微信这个版本有两套请求基类：

| 基类 | 派发方式 | 例子 |
| --- | --- | --- |
| `com.tencent.mm.modelbase.m1` | `scene.doScene(dispatcher, new com.tencent.mm.network.y2())` | `qn.p`、`qn.o`、`qn.m`、`qn.q`、`v51.r0`、`p3` |
| `com.tencent.mm.modelbase.i` | 构造后 `com.tencent.mm.modelbase.z2.d(this.f, null, false)` 交给 Cgi 运行器 | `qn.b`、`qn.e`、`qn.d` |

`m1` 分支复用 `wx_send.cpp` 的 `Dispatcher()`（`com.tencent.mm.modelbase.r1.y.k()` 优先，
`com.tencent.mm.network.a3.c()` 兜底）+ 空回调 `y2`。
`i` 分支把请求对象（继承字段 `f`，类型 `Lcom/tencent/mm/modelbase/o;`）交给
`z2.d(o, callback, false)`；`z2` 内部走
`((gp0.n) z2.f59772b).f268046a.f268074b.h(scene, 0)`，与 App 完全一致。

## 已实现的场景

| Satori 方法 | 场景类 | cgi | 构造参数 |
| --- | --- | --- | --- |
| `guild.member.kick` | `qn.p` | `/cgi-bin/micromsg-bin/delchatroommember` | `(String chatroom, List members, int op)`，op=0 |
| `channel.delete`（退群） | `qn.p` | 同上 | members=`[self]`，op=0 |
| `guild.member.role.set` | `qn.b` | `/cgi-bin/micromsg-bin/addchatroomadmin` | `(String chatroom, LinkedList members)` |
| `guild.member.role.unset` | `qn.e` | `/cgi-bin/micromsg-bin/delchatroomadmin` | `(String chatroom, LinkedList members)` |

成员列表用 `java.util.LinkedList<String>`（`qn.p` 形参是 `List`，`qn.b/e` 是 `LinkedList`）；
`qn.b/e` 构造器内部会 `y8.b1(list, ";")` 做一次规整。

角色：微信没有自定义角色，`guild.role.list` 合成 `owner`(群主) / `admin`(管理员) / `member`(成员)。
只有 `admin` 可被 `guild.member.role.set/unset` 变更；`owner`/`member` 固定。**读侧的
`guild.member.role.list` 目前仍只反映 owner/member**：管理员位在 `chatroom` 成员结构的标志位
（`com.tencent.mm.storage.z2.E0()` 检查 `so.b.f424935f & 2048`），当前 store 没解析它，
所以设/撤管理员后读侧不会立刻体现——写操作本身是真发的。

## 没做的写操作与原因

| 方法 | 结论 |
| --- | --- |
| `channel.create` | 微信没有「群内子频道」，Satori 语义无法映射 |
| `channel.update` | 群改名在可读 dex 里找不到 cgi；可能编译进 `libapp.so` |
| `channel.mute` / `guild.member.mute` | 微信没有服务端全员/单人禁言（只有客户端「消息免打扰」） |
| `friend.approve` / `friend.delete` | 好友申请审批走 `com.tencent.mm.pluginsdk.model.p3`（`verifyuser`）并依赖申请消息里的 ticket；好友删除没有独立 cgi（本地删+同步） |
| `guild.approve` / `guild.member.approve` | 入群审批用 `qn.d`（`approveaddchatroommember`，`(long,String,String,String,List)`），参数语义未确认 |
| `upload.create` 之外的媒体发送 | 见 `docs/wechat-send-types.md` |

这些方法不在 `features` 里，客户端得到 404；`internal/capabilities.unsupported` 只列微信真的
没有概念的能力。

## 安全边界

- 全部默认关闭，`send=on` 才进 features。
- 只在精确匹配的微信主进程内运行；派发器、JavaVM、宿主 ClassLoader 都来自 App 自身。
- 不伪造成功：构造/派发失败会带原因返回 502，`rejected:true` 表示在派发前被开关拒绝。
- 破坏性动作（踢人、退群、设/撤管理员）由调用方负责；模块不做二次确认。
