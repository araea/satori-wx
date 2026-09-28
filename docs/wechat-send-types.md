# 微信发送各类消息的逆向记录（2026-09-28）

> v0.9.1 修：`internal:` 链接的解析曾经把字符串字面量写成 `constexpr const char *`，
> `sizeof` 拿到指针大小，所以 v0.9.0 的图片发送在真机上每张都回 `media_unavailable`。
> 解析现在在 `tempstore.cpp` 的 `TempStoreResolveLink`，由 `tests/tempstore_test.cpp` 覆盖。

目标：在只反射、不 hook 的前提下，把 Satori 的图片、语音、视频、文件发出去。
纯文本发送、撤回、群管理已做完（见 `docs/wechat-send.md`、`docs/wechat-room.md`）。

微信 8.0.78 / versionCode 671108664，base.apk 在 `~/tmp/satori-wx/base.apk`。
工具见 `tools/dex*.py`（`dexmethodsig.py` / `dexinvokes.py` / `dexmethodstrings.py` 最常用；
`dexfindclass.py`、`dexrefs.py` 有误报，别单独信）。
**结论性事实要落到「某个类的某个字段/方法」上，并且能和 App 自己的调用点对上**——下面几条
都是这么定下来的。

## 一个反复踩的坑：`v51.r1` 的字段名不是语义

`v51.r1` 是发送参数构建器，字段是 `a..q` 的短名。曾经按「`r1.h()` 设路径、`r1.e()` 设目标」
记过一版，**是错的**，正确的对应关系要从 App 自己的代码反推：

```java
// qs5.v5.hj(String talkerCsv, String path, String msgSource) ——  App 的「把一句话发给多个会话」
v51.r1 a19 = v51.s1.a(str4);   // str4 来自 talkerCsv.split(",")
a19.h(str4);                   // 目标 talker
a19.e(str);                    // 正文/本地路径
a19.i(b41.d2.C(str4));         // 消息类型：d2.C 的入参是 talker（内部只在结尾判 @chatroom）
a19.f468468f = 1;              // flags
a19.f468470h = a17;            // msgsource 附加项（HashMap）
a19.g(str3);                   // msgSource
a19.f468471i = 5;              // 选择器：选中 dy1.g.k() 的第 5 分支
a19.a().a();                   // 构建并提交
```

判定依据（两条互相独立）：

- `dy1.g.k()` 的第 2 分支 `new r0(f468469g, f468468f, f468464b)` 对上 `r0(long,int,String)`，而
  那个构造器里是 `ex0.k0.yi(str, localId)`——第三个参数是 **talker**。`f468464b` 就是 `h()` 写的
  字段，所以 `h()` 是 talker。
- `b41.d2.C(String)` 的字节码里出现 `endsWith("@chatroom")`、`y3.J4/v4/G3`，返回 1/11/36——
  入参是会话名而不是路径，`r1.i(...)` 用它算消息类型。

`r1.i(int)` 写的是 `f468467e`，不是 `f468471i`；`f468471i` 是「用哪个 provider 分支」的选择器，
要自己设。

## 已经落地的

| | 类 / 方法 | 说明 |
| --- | --- | --- |
| 文本 | `v51.r0.<init>(String,String,int,int,long,String)` + `doScene` | 构造器自己入库，cgi `newsendmsg` |
| 图片 | `v51.r0.<init>(String,String,int,int,Object,String)` + `doScene` | 同一个 NetScene 的媒体重载，见下 |
| 撤回 | `com.tencent.mm.modelsimple.d1.<init>(e9,String,String)` + `doScene` | cgi `revokemsg` |

## 图片：`v51.r0` 的 Object 重载

`v51.r0` 有三个业务构造器：

```
(String talker, String content, int type, int flags, long localId, String msgSource)   <- 文本
(String talker, String content, int type, int flags, Object obj, String msgSource)    <- 媒体重载
(long localId, int flags, String talker)                                             <- 重发
```

两个 String 重载的构造体一样：`e9.u1(talker)`、`e9.e1(md5(talker))`、`e9.setType(type)`、
`e9.b1(content)`，末尾入库（日志 `new msg inserted to db , local id = `）并返回 `f`（本地 id）。
`obj` 只在 `(flags & 1) != 0 && obj instanceof HashMap` 时用于拼 `<msgsource>`，与图片数据无关。
长整型重载多一句 `if (flags == 1 && type == 42) e9.q3()`，`q3()` 只是 `F |= 512`（免打扰位），
和图片无关。

App 自己发本地图片的路径是 `qs5.v5.fj` / `gj`（`MsgRetransmitUI` 那套）：

```java
String path = qb.b(src, callback);
v51.r1 b = v51.s1.a(talker);
b.h(talker);
b.e(path);                                  // 本地文件路径放在「正文」位置
b.i(Y3.o4(src) ? 66 : 42);                  // 42 = 图片，66 = 动图
b.f468468f = 0;
b.f468471i = 4;                             // 选择器 4 -> r0(..., long localId, ...)
v51.n1 n = b.a();
n.a();
```

所以本模块的 `SendMedia` 直接构造同一个场景：

```
new v51.r0(talker, localPath, gif ? 66 : 42, 0, (Object) null, "")
scene.doScene(dispatcher, new com.tencent.mm.network.y2())
scene.f -> 本地消息 id
```

比走 `s1.a()` 少一层依赖：不需要 App 的 `v51.s1.f468484a` 工厂（那是 Kotlin DI 装的，可能为
null），也不需要 `ph5.n0.c(...)` 这类服务定位。文件必须是微信 uid 读得到的路径——模块的
`upload.create` 落在 `<微信数据目录>/files/satori-wx-tmp/`，正是为此。

**边界**：这条路径是 App 的「把已有本地文件当图片发出去」。成功与否看微信自己的日志与库里
是否新增行；失败会返回 502 并带原因。`42` 是按 App 的取值写死的常量，若某天微信改了常量，
症状是消息入库但发不出去。

## 没做的：`qs5.v5` 的其它入口（供下一位接手）

`qs5.v5`（日志 target `MicroMsg.SendMsgMgr`）是 App 的高层发送管理，公开方法里有各类型：

| 方法 | 用途 | 关键参数 |
| --- | --- | --- |
| `b(Context,String toUser,String fileName,int,String,String,String,b41.k7,s0.d)` | **新图片**（上传） | `da0.g` 协程上传任务；`k7` 回调里有本地 id，`s0.d` 可传 null |
| `cj` / `dj(String toUser,byte[],String contentXml,...)` | AppMsg / 文件链接（直接给 XML） | 文件类走 app attach |
| `ej` / `fj` / `rj(String toUser,String path,boolean,pc5.yl)` | 视频 / 语音 / 图片（转发的入口） | `fj` 已按上面分析照搬进 `SendMedia` |
| `nj` / `oj` / `pj(String,String toUser,int,int,long,...)` | 视频（带时长与尺寸） | |
| `gj` / `hj(String,String path,...,boolean)` | 转发已收到的媒体 | 走 `MsgRetransmitUI` |

`b(...)` 才是「相册里新图直接发」的正路：它 new `da0.g(path, i17, u17, toUser, w90.i0)` 交给
`kt.d1` 的协程 flow 上传，回调是 Kotlin 接口。`dVar`（`s0.d`）可传 null（代码里有 null 判断），
`k7Var` 可以自己 `new b41.k7()`（有 `()V` 构造器，`b` 字段就是本地 id）从而拿到消息 id。
难点是 `ph5.n0.c(Class)` 返回的是 `ph5.m`（服务定位），是否为真实 `qs5.v5` 实例或动态代理未验证。

文件（type 49 / AppMsg）走的是另一套：`dy1.g.h()` 对 `l51.m1` 子类按 `f468471i` 取 1 / 4 / 20，
分别 new `s61.w0(str)` / `s61.q1(...)` / `s61.r1(...)`。`l51.m1` 是 `v51.r1` 的子类，多几个字段。
这条路还没接。

## 其它可复用的事实

- `ActivityThread.currentApplication()` 拿 Context；`getClassLoader()` 拿宿主 ClassLoader。
- `gp0.j1.e()` 和 `b41.h9.e()` 都返回 `com.tencent.mm.modelbase.r1`（NetSceneQueue）。
- `ex0.k0.F0` 是 `ex0.j0` 单例，`k(String talker, long localId)` 取 `com.tencent.mm.storage.e9`（MsgInfo）。
- `v51.n1.a()` 的实现是 `((gp0.n) z2.f59772b).f268046a.f268074b.h(scene, 0)`，与群管理写操作的
  Cgi 派发是同一个队列。
- 这些类都在可读 dex 里；**优先用可读 dex 拼路径，别去碰 `libapp.so`**。
