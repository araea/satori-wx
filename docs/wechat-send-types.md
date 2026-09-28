# 微信发送各类消息的逆向记录

文本能发，图片发不出去，而且在当前约束下走不通。死路清单见下文。

微信 8.0.78 / versionCode 671108664，base.apk 在 `~/tmp/satori-wx/base.apk`。
工具见 `tools/dex*.py`（`dexmethodsig.py` / `dexinvokes.py` / `dexmethodstrings.py` 最常用；
`dexfindclass.py`、`dexrefs.py` 有误报，别单独信）。
**结论性事实要落到「某个类的某个字段/方法」上，并且能和 App 自己的调用点对上。**

## 已经落地的

| | 类 / 方法 | 说明 |
| --- | --- | --- |
| 文本 | `v51.r0.<init>(String,String,int,int,long,String)` + `doScene` | 构造器自己入库，cgi `newsendmsg` |
| 撤回 | `com.tencent.mm.modelsimple.d1.<init>(e9,String,String)` + `doScene` | cgi `revokemsg` |
| 群管理 | `qn.p` / `qn.b` / `qn.e` | 见 [群管理写操作](wechat-room.md) |

## 一个反复踩的坑：`v51.r1` 的字段名不是语义

`v51.r1` 是发送参数构建器，字段是 `a..q` 的短名。曾经按「`r1.h()` 设路径、`r1.e()` 设目标」
记过一版，**是错的**。正确的对应关系要从 App 自己的代码反推：

```java
// qs5.v5.hj(String talkerCsv, String path, String msgSource)  App 的「把一句话发给多个会话」
v51.r1 a19 = v51.s1.a(str4);   // str4 来自 talkerCsv.split(",")
a19.h(str4);                   // 目标 talker
a19.e(str);                    // 正文
a19.i(b41.d2.C(str4));         // 消息类型：d2.C 的入参是 talker（内部只在结尾判 @chatroom）
a19.f468468f = 1;              // flags
a19.f468470h = a17;            // msgsource 附加项（HashMap）
a19.g(str3);                   // msgSource
a19.f468471i = 5;              // 选择器：选中 dy1.g.k() 的第 5 分支
a19.a().a();                   // 构建并提交
```

判定依据（两条互相独立）：

- `dy1.g.k()` 的第 2 分支 `new r0(f468469g, f468468f, f468464b)` 对上 `r0(long,int,String)`，而
  那个构造器里是 `ex0.k0.yi(str, localId)`，第三个参数是 **talker**。`f468464b` 就是 `h()` 写的
  字段，所以 `h()` 是 talker。
- `b41.d2.C(String)` 的字节码里出现 `endsWith("@chatroom")`、`y3.J4/v4/G3`，返回 1/11/36：
  入参是会话名而不是路径。

`r1.i(int)` 写的是 `f468467e`，不是 `f468471i`；`f468471i` 是「用哪个 provider 分支」的选择器。

另外：`qs5.v5.hj` **不是**发图片的，是「同一句话发给多个会话」（`d2.C` 判会话类型、
`com.tencent.mm.ui.g1.a(正文)` 取的是粘贴相似度之类的 msgsource）。

## 图片：试过，撤回了

### 试的那条路（App 的转发路径）

App 的 `qs5.v5.fj`/`gj`（`MsgRetransmitUI` 那套）这样发媒体：

```java
String path = qb.b(src, callback);
v51.r1 b = v51.s1.a(talker);
b.h(talker);
b.e(path);                                  // 本地文件路径放在「正文」位置
b.i(y3.o4(src) ? 66 : 42);                  // 42 / 66
b.f468468f = 0;
b.f468471i = 4;
v51.n1 n = b.a(); n.a();
```

对应到场景就是 `v51.r0` 的媒体重载：

```
(String talker, String content, int type, int flags, long localId, String msgSource)   <- 文本
(String talker, String content, int type, int flags, Object obj, String msgSource)    <- 媒体重载
(long localId, int flags, String talker)                                             <- 重发
```

两个 String 重载构造体一样：`e9.u1(talker)`、`e9.e1(md5(talker))`、`e9.setType(type)`、
`e9.b1(content)`，末尾入库并返回 `f`（本地 id）。`obj` 只在 `(flags & 1) != 0 && obj instanceof
HashMap` 时用于拼 `<msgsource>`，与图片数据无关。

### 真机结果：这条路由不成立

v0.9.0 用 `new v51.r0(talker, path, 42, 0, null, "")` + `doScene` 发到 filehelper，返回 200，
logcat 有 `sent media to filehelper (local id 3710, netId 0, ...)`，但库里那行是：

```
msgId | type | isSend | status | talker     | content                              | imgPath
3710  | 42   | 1      | 5      | filehelper | /data/user/0/.../satori-wx-tmp/x.png | NULL
```

`status=5`、`imgPath` 为空，之后**永不推进**。对照同一个会话里真发出去的图片：

```
msgId | type | isSend | status | talker               | content
3401  | 3    | 1      | 2      | 59012484892@chatroom | <msg><img aeskey="2ff5f1..." encryver...
```

**真发出去的图片是 `type=3` + `status=2`，`content` 是带 CDN 密钥的 `<msg><img aeskey=...>` 全文**；
`type=42` 在整个库里只出现过这一行，就是为了这次实验写进去的。所以 42/66 是「转发一段已经有
CDN 信息的媒体」的中间态，不是「把一个本地文件发成图片」。

### App 真正发新图的路（native 走不通）

`qs5.v5.b(Context, String toUser, String fileName, int, String, String, String, b41.k7, s0.d)`：

```java
if (!h9.b().F()) { ...; return; }                        // 存储就绪
com.tencent.mm.pluginsdk.ui.tools.p0.a();                // 初始化
w90.i0 i0Var = new w90.i0();                             // 发送上下文
i0Var.f480008a = 4; i0Var.f480022o = k7Var; i0Var.f480017j = str3;
da0.g gVar = new da0.g(str2 /*imgPath*/, i17, y1.u() /*fromUsername*/, str /*toUser*/, i0Var);
kotlinx.coroutines.flow.j flow = ((ha0.w)((kt.d1) ph5.n0.c(kt.d1.class))).rj(gVar);
if (dVar != null) ((e36.t0) e36.t0.f237738d).g(new a6(this, flow, dVar));   // 只有这里会跑
```

- `da0.g` 只是个数据类（字段：imgPath / int / fromUsername / toUsername / crossParams / uuid），
  真正干活的是 `kt.d1` 返回的协程 flow。
- **`dVar`（类型 `s0.d`）是收集者**：传 null 就没有任何东西订阅这个 flow，上传根本不会发生。
- `dVar` 是 Kotlin 函数类型，native 反射交不出来（要交就得定义 Java 类）。
  模块的硬约束是纯 native、不定义 Java 类、不加载 dex（见 `README`），所以这条路封死。
- `b41.k7` 反过来是能用的：它有 `()V` 构造器，字段 `b`(J) 就是本地消息 id，可以当回调收结果。

顺带记一笔：`v51.q0`/`p0` **不是**上传步。`q0.run()` 跑的是
`new com.tencent.mm.modelsimple.l1(5,"","","","",false,1,false).doScene(...)`，而
`modelsimple.l1` 是 **NetSceneVerifyPswd**（`/cgi-bin/micromsg-bin/newverifypasswd`），
`p0` 的日志串是 `verifypsw onSceneEnd ... needVerifyPswList ... verifyingPsw`：
它是「发送需要校验支付密码时重试」的包装，名字像上传而已。

### 结语

`message.create` 现在只发纯文本；content 里只有媒体元素时返回 400
`{"error":"media_unsupported"}`，不假报成功，也不往库里写任何行（那行 type=42 是 v0.9.0
实验留下的，在 filehelper 里，只有自己看得到）。
要做图片，得先决定是否接受引入一个极小的 Java 助手 / DEX，那是设计层面的改动，不是逆向问题。

## 其它入口（供参考，都没接）

| 方法 | 用途 | 关键点 |
| --- | --- | --- |
| `qs5.v5.cj` / `dj(String toUser, byte[] contentXml, ...)` | AppMsg / 文件（直接给 XML） | 文件类走 app attach，attachid 要先上传 |
| `qs5.v5.ej` / `rj` / `fj(String,String,boolean,pc5.yl)` | 视频 / 语音 / 图片的转发入口 | `fj` 就是上面那条 type 42 的路 |
| `qs5.v5.nj` / `oj` / `pj(String,String,int,int,long,...)` | 视频（带时长与尺寸） | |
| `qs5.v5.bj(Context,int,List,rn3.x0)` | 批量发图（朋友圈） | |
| `dy1.g.h()` → `s61.w0` / `s61.q1` / `s61.r1` | 文件 / AppMsg / 视频（`l51.m1` 子类 + `f468471i` 取 1/4/20） | 同样要上传，没细查 |

## 其它可复用的事实

- `ActivityThread.currentApplication()` 拿 Context；`getClassLoader()` 拿宿主 ClassLoader。
- `gp0.j1.e()` 和 `b41.h9.e()` 都返回 `com.tencent.mm.modelbase.r1`（NetSceneQueue）。
- `ex0.k0.F0` 是 `ex0.j0` 单例，`k(String talker, long localId)` 取 `com.tencent.mm.storage.e9`（MsgInfo）。
- `v51.n1.a()` 的实现是 `((gp0.n) z2.f59772b).f268046a.f268074b.h(scene, 0)`，与群管理写操作的
  Cgi 派发是同一个队列。
- 这些类都在可读 dex 里；**优先用可读 dex 拼路径，别去碰 `libapp.so`**。
