# 微信发送各类消息的逆向记录（2026-09-27）

目标：在只反射、不 hook 的前提下，把 Satori 的图片、语音、视频、文件发出去。
纯文本发送与撤回已经做完（见 `docs/wechat-send.md`、`HANDOFF.md` §6）；这里记录其它类型的
接口位置与接入方案，供下一位接手。

微信 8.0.78 / versionCode 671108664，base.apk 在 `~/tmp/satori-wx/base.apk`。
工具见 `tools/dex*.py`（`dexfindstring.py` / `dexmethodsig.py` / `dexfields.py` 最常用；
`dexfindclass.py`、`dexcalls.py` 有误报，别单独信）。

## 已经落地的两条

| | 类 / 方法 | 说明 |
| --- | --- | --- |
| 文本 | `v51.r0.<init>(String,String,int,int,long,String)` + `doScene` | 构造器自己入库，cgi `newsendmsg` |
| 撤回 | `com.tencent.mm.modelsimple.d1.<init>(e9,String,String)` + `doScene` | cgi `revokemsg`；`e9` 来自 `ex0.k0.F0.k(talker,localId)` |

## 微信的通用发送入口：`qs5.v5`（SendMsgMgr）

日志 target 就是 `MicroMsg.SendMsgMgr`。它是一个 Kotlin 类，公开方法里有各类型的发送：

| 方法 | 猜测用途 | 关键参数 |
| --- | --- | --- |
| `b(Context,String toUser,String fileName,int,String,String,String,b41.k7,s0.d)` | **发图片** | 本地文件名 + 目标；内部 new `da0.g(...)` 走协程 flow 上传 |
| `bj(Context,int,List,rn3.x0)` | 批量发图（朋友圈？） | |
| `cj` / `dj(String toUser,byte[],String contentXml,...)` | **发 AppMsg/文件链接** | 直接给 XML 内容；文件类会走 app attach |
| `ej` / `fj` / `rj(String toUser,String path,boolean,pc5.yl)` | 视频/语音转发 | |
| `nj` / `oj` / `pj(String,String toUser,int,int,long,...)` | 视频（带时长/尺寸） | |
| `gj` / `hj(String,String path,...,boolean)` | 转发已收到的媒体 | 走 `MsgRetransmitUI` |
| `sj` / `tj` / `uj` / `vj(...)` | 图片（最多参数那组） | 带 `MsgIdTalker`、回调 |

共同难点：都要 `Context`（可用 `ActivityThread.currentApplication()`），回调是 Kotlin
接口（`b41.k7`、`pc5.yl`、`rn3.z0`、`s0.d` 等），native 造不出来，只能传 `null` 试；
且多是协程，调用返回 void，**成功与否只能看微信自己的日志/库里是否出现新行**。

## 更底层的通用入口：`v51.r1` + `v51.s1` + `v51.n1`

`qs5.v5` 内部和 `MsgRetransmitUI` 都走这套「按本地文件建消息再提交」：

```
v51.r1 b = v51.s1.a(path);      // 按文件路径建 builder
b.h(path);                      // 路径
b.e(talker);                    // 目标
b.i(type);                      // 消息类型：1 文本 / 3 图片 / 34 语音 / 43 视频 / 47 表情 / 49 文件
v51.n1 msg = b.a();             // 交给已注册的 o1 工厂（s1.f468484a）产出场景
boolean ok = msg.a();           // 内部 ((gp0.n) z2.f59772b)...h(scene,0) 提交
```

- `v51.n1.a()` 就是「提交到 NetSceneQueue」，等价于 `r1.h(scene,0)`。
- 工厂由 `com.tencent.mm.pluginsdk.ui.tools.p0.a()` 装配；调用前先 `p0.a()`。
- `v51.s1.a(path)` 会按扩展名/魔数决定类型，所以理论上**给路径就能发图片/语音/视频/文件**，
  不必自己填 CDN 参数。这是最值得先试的一条，因为它绕开了 Kotlin 回调。

## 建议的接入顺序

1. **图片**：先试 `v51.s1.a(path)` → `r1.e(talker)` → `r1.i(3)` → `n1.a()`；
   不行再试 `qs5.v5.b(app, talker, path, 0, "", "", "", null, null)`。
   文件必须放在微信进程读得到的地方（`/data/data/com.tencent.mm/` 下或公共存储）。
2. **文件**：`qs5.v5.cj/dj` 的 XML 路径更可控（AppMsg 的 `attachid` 需要先上传）。
   或者同样先试 `v51.s1` 通用路径。
3. **语音/视频**：`nj/oj/pj` 或 `v51.s1` + 对应 type。
4. 每个动作都复用现有的开关；**默认关闭**，先只在 `filehelper` 上真机验一条，
   看微信日志与库里的新行，再决定是否放进 `features`。

## 其它可复用的事实

- `ActivityThread.currentApplication()` 拿 Context；`getClassLoader()` 拿宿主 ClassLoader。
- `gp0.j1.e()` 和 `b41.h9.e()` 都返回 `com.tencent.mm.modelbase.r1`（NetSceneQueue）。
- `ex0.k0.F0` 是 `ex0.j0` 单例，`k(String talker, long localId)` 取 `com.tencent.mm.storage.e9`（MsgInfo）。
- 这些类都在可读 dex 里；**优先用可读 dex 拼路径，别去碰 `libapp.so`**（发送/撤回都是这么找到的）。
